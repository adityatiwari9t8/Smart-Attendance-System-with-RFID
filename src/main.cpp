// =====================================================================
// ESP32 Smart Attendance System
//
// Tap an RFID card -> looked up against the roster -> the tap is saved
// to flash and queued in the "outbox" -> shown on the OLED + LEDs/buzzer
// -> a background task uploads the outbox to a Google Sheet over HTTPS.
//
// A tap is never lost: it is durable the moment it is accepted, and it
// stays queued until the backend confirms it, surviving Wi-Fi drops, a
// backend outage, or power loss. See README.md for wiring and setup and
// docs/ARCHITECTURE.md for how the pieces fit together.
//
// Nothing in loop() calls delay() - every wait (LED hold, message time,
// button press length, retry backoff) is tracked with millis(), so the
// RFID reader and the Serial console stay live even mid-upload. (The one
// deliberate exception is the SELFTEST command.)
// =====================================================================

#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>

#include "AttendanceLogger.h"
#include "DisplayUI.h"
#include "FeedbackIndicators.h"
#include "Outbox.h"
#include "StudentDatabase.h"
#include "TimeUtil.h"
#include "config.h"
#include "secrets.h"  // checked for leftover placeholders at boot

// ---------------------------------------------------------------------
// Roster. Starts empty: enroll each card with ENROLL / ADD over Serial;
// enrolled students persist in flash (see StudentDatabase).
// ---------------------------------------------------------------------
Student roster[MAX_STUDENTS];

MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
DisplayUI oled;
FeedbackIndicators feedback;
AttendanceLogger cloud;
StudentDatabase students(roster, 0, MAX_STUDENTS);
Outbox outbox;

// ---------------------------------------------------------------------
// Screen state - lets a result, a Serial command's confirmation or the
// summary view take over the OLED, then fall back to the ready screen on
// its own after `holdMs`, without ever calling delay().
// ---------------------------------------------------------------------
enum class ScreenState { READY, MESSAGE };
ScreenState screenState = ScreenState::READY;
unsigned long messageShownAt = 0;
unsigned long messageHoldMs = 0;

// What the ready screen last showed, so it redraws itself when the
// Wi-Fi state, the unsynced count or the reader health changes.
bool shownWifi = false;
int shownPending = -1;
bool shownReaderOk = true;

bool readerOk = true;
unsigned long lastReaderCheckMs = 0;
unsigned long lastReaderPollMs = 0;
unsigned long lastDayCheckMs = 0;

void drawReady() {
  oled.showReady(cloud.isConnected(), outbox.size(), readerOk);
  shownWifi = cloud.isConnected();
  shownPending = outbox.size();
  shownReaderOk = readerOk;
  screenState = ScreenState::READY;
}

void showTimedMessage(const String& title, const String& line1, const String& line2,
                       const String& line3, unsigned long holdMs) {
  oled.showMessage(title, line1, line2, line3);
  screenState = ScreenState::MESSAGE;
  messageShownAt = millis();
  messageHoldMs = holdMs;
}

// Same-card debounce (see SAME_CARD_COOLDOWN_MS in config.h).
String lastScannedUid;
unsigned long lastScanMs = 0;

// Upload pacing: after a failed upload wait `uploadFailures`-scaled
// backoff before the next attempt instead of hammering a dead network.
int uploadFailures = 0;
unsigned long nextUploadAt = 0;

// ENROLL <name>: the next card tapped within ENROLL_WINDOW_MS is enrolled.
String enrollName;
unsigned long enrollDeadline = 0;
bool enrollArmed = false;

// Reset-button state, tracked with millis() instead of a busy-wait. A
// SHORT press shows the attendance summary. The long-hold-to-wipe gesture
// only works at boot (see setup()), so an absent-minded long press during
// class can't nuke attendance data.
bool buttonWasDown = false;
unsigned long buttonDownAt = 0;

// Unix time, or 0 if the clock hasn't been set by NTP yet.
uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return isClockValid(t) ? (uint32_t)t : 0;
}

// Reads the UID of a freshly-detected card as uppercase hex, e.g.
// {0x01,0x02,0x03,0x04} -> "01020304" (same format as the roster).
String readCardUID() {
  String uid;
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// The MFRC522 reports a chip version (0x91/0x92 for genuine parts and
// most clones). 0x00 or 0xFF means nothing is answering on SPI - almost
// always a loose/miswired SCK, MISO, MOSI, SDA or 3.3V jumper.
byte readerVersion() {
  return rfid.PCD_ReadRegister(MFRC522::VersionReg);
}
bool readerResponds() {
  byte v = readerVersion();
  return v != 0x00 && v != 0xFF;
}

// ---------------------------------------------------------------------
// Serial console - manage the roster and diagnose the build:
//   ENROLL <Full Name>      then tap the new card (easiest way to enroll)
//   ADD <UID> <Full Name>   enroll by typing the UID
//   REMOVE <UID>            un-enroll a student (e.g. a typo in the name)
//   LIST                    roster + who's present
//   STATUS                  Wi-Fi, clock, queue, reader, memory
//   SELFTEST                blink every LED, beep, check the reader
//   RESET                   wipe today's attendance (same as the boot-hold)
//   HELP
// Characters are read one at a time from Serial.available() so this never
// blocks loop().
// ---------------------------------------------------------------------
void printHelp() {
  Serial.println(F("\n--- Commands ---"));
  Serial.println(F("ENROLL <Name>      Enroll a card: type this, then tap the card"));
  Serial.println(F("ADD <UID> <Name>   Enroll by typing the UID, e.g. ADD 3F2A9B10 Priya Sharma"));
  Serial.println(F("REMOVE <UID>       Un-enroll a student (to fix a typo: REMOVE, then ENROLL again)"));
  Serial.println(F("LIST               Show the roster"));
  Serial.println(F("STATUS             Wi-Fi, clock, upload queue, reader, memory"));
  Serial.println(F("SELFTEST           Blink LEDs, beep, check the RFID reader"));
  Serial.println(F("RESET              Wipe today's attendance (enrolled students stay)"));
  Serial.println(F("HELP               Show this message"));
}

void printRoster() {
  Serial.printf("\n--- Roster (%d/%d enrolled, %d present) ---\n", students.count(),
                students.capacity(), students.countPresent());
  for (int i = 0; i < students.count(); i++) {
    const Student& s = students.at(i);
    Serial.printf("%2d. %-16s %-20s %s\n", i, s.uid.c_str(), s.name.c_str(),
                  s.logged ? "PRESENT" : "-");
  }
  if (!students.dayConfirmed()) {
    Serial.println(F("(Clock not set yet: taps from before this power-on are not counted until it is.)"));
  }
}

void printStatus() {
  Serial.println(F("\n--- Status ---"));
  Serial.printf("Wi-Fi     : %s", cloud.isConnected() ? "connected" : "OFFLINE");
  if (cloud.isConnected()) Serial.printf(" (%s, %d dBm)", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Serial.println();
  uint32_t epoch = nowEpoch();
  if (epoch) {
    time_t t = (time_t)epoch;
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    Serial.printf("Clock     : %s (%s)\n", buf, TZ_INFO);
  } else {
    Serial.println(F("Clock     : not set yet (waiting for NTP)"));
  }
  Serial.printf("Day       : %s\n", students.dayConfirmed()
                                       ? "confirmed - present flags are today's"
                                       : "NOT confirmed (no clock) - earlier taps not counted yet");
  Serial.printf("Unsynced  : %d tap(s) queued for upload\n", outbox.size());
  Serial.printf("Present   : %d / %d\n", students.countPresent(), students.count());
  Serial.printf("RFID      : version 0x%02X (%s)\n", readerVersion(),
                readerResponds() ? "OK" : "NOT RESPONDING - check wiring");
  if (oled.available()) {
    Serial.printf("OLED      : OK (I2C 0x%02X)\n", oled.address());
  } else {
    Serial.println(F("OLED      : not found on I2C (0x3C/0x3D)"));
  }
  Serial.printf("Free heap : %u bytes (largest block %u)   Uptime: %lu s\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), millis() / 1000UL);
}

void runSelfTest() {
  Serial.println(F("\n--- Self test ---"));
  Serial.println(F("Watch for: green, red, blue LED in turn, then two beeps."));
  oled.showMessage("SELF TEST", "LEDs + buzzer...", "", "");
  feedback.selfTest();
  bool ok = readerResponds();
  Serial.printf("RFID reader: version 0x%02X -> %s\n", readerVersion(),
                ok ? "OK" : "NOT RESPONDING (check SDA/SCK/MOSI/MISO/RST/3.3V)");
  Serial.printf("OLED       : %s\n", oled.available() ? "OK" : "not found");
  showTimedMessage("SELF TEST", ok ? "RFID reader: OK" : "RFID reader: FAIL",
                    oled.available() ? "OLED: OK" : "", "See Serial monitor", 2500);
}

// Enrolls `uid` under `name` and reports the result everywhere.
void enrollAndReport(const String& uid, const String& name, const char* via) {
  String err;
  if (students.enroll(uid, name, err)) {
    Serial.printf("[ENROLL] %s -> \"%s\" (%d/%d)\n", uid.c_str(), name.c_str(), students.count(),
                  students.capacity());
    showTimedMessage("STUDENT ADDED", name, "UID: " + uid, via, SUCCESS_DISPLAY_MS * 2);
    feedback.success();
  } else {
    Serial.printf("[ENROLL] Failed: %s\n", err.c_str());
    showTimedMessage("ENROLL FAILED", err, "", "", RESULT_DISPLAY_MS * 2);
    feedback.error();
  }
}

void handleSerialCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  String upper = line;
  upper.toUpperCase();

  if (upper == "HELP") {
    printHelp();
  } else if (upper == "LIST") {
    printRoster();
  } else if (upper == "STATUS") {
    printStatus();
  } else if (upper == "SELFTEST") {
    runSelfTest();
  } else if (upper == "RESET") {
    students.resetAttendance();
    Serial.println(F("[RESET] Today's attendance wiped."));
    showTimedMessage("ATTENDANCE RESET", "All records wiped", "", "", SUCCESS_DISPLAY_MS * 2);
    feedback.success();
  } else if (upper == "ENROLL" || upper == "ADD" || upper == "REMOVE") {
    Serial.println(F("[?] That command needs more - type HELP for the format."));
  } else if (upper.startsWith("ENROLL ")) {
    String name = line.substring(7);  // from the original line: keep capitalization
    name.trim();
    if (name.length() == 0) {
      Serial.println(F("[ENROLL] Usage: ENROLL <Name>, then tap the card"));
    } else {
      enrollName = name;
      enrollArmed = true;
      enrollDeadline = millis() + ENROLL_WINDOW_MS;
      Serial.printf("[ENROLL] Tap the card for \"%s\" within %d s...\n", name.c_str(),
                    ENROLL_WINDOW_MS / 1000);
      showTimedMessage("ENROLL MODE", name, "Tap the new card", "now...", ENROLL_WINDOW_MS);
    }
  } else if (upper.startsWith("ADD ")) {
    String rest = line.substring(4);
    rest.trim();
    int sep = rest.indexOf(' ');
    if (sep < 0) {
      Serial.println(F("[ADD] Usage: ADD <UID> <Name>"));
    } else {
      String name = rest.substring(sep + 1);
      name.trim();
      enrollAndReport(rest.substring(0, sep), name, "via Serial");
    }
  } else if (upper.startsWith("REMOVE ")) {
    String uid = line.substring(7);
    uid.trim();
    String name, err;
    if (students.remove(uid, name, err)) {
      Serial.printf("[REMOVE] \"%s\" un-enrolled (%d/%d)\n", name.c_str(), students.count(),
                    students.capacity());
      showTimedMessage("STUDENT REMOVED", name, "UID: " + uid, "", SUCCESS_DISPLAY_MS * 2);
      feedback.success();
    } else {
      Serial.printf("[REMOVE] Failed: %s\n", err.c_str());
      feedback.error();
    }
  } else {
    Serial.println(F("[?] Unknown command. Type HELP."));
  }
}

void pollSerialCommands() {
  static String buffer;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      handleSerialCommand(buffer);
      buffer = "";
    } else if (c == '\b' || c == 0x7F) {
      // Backspace: the serial monitor sends every keystroke as typed.
      if (buffer.length() > 0) buffer.remove(buffer.length() - 1);
    } else if (c != '\r') {
      buffer += c;
      if (buffer.length() > 96) buffer = "";  // guard against a runaway line
    }
  }
}

// Non-blocking reset-button watcher for normal operation (the boot-time
// long-hold-to-wipe check in setup() is separate).
void pollResetButton() {
  bool down = (digitalRead(PIN_BUTTON_RESET) == LOW);

  if (down && !buttonWasDown) {
    buttonWasDown = true;
    buttonDownAt = millis();
  } else if (!down && buttonWasDown) {
    buttonWasDown = false;
    unsigned long heldMs = millis() - buttonDownAt;
    if (heldMs >= BUTTON_DEBOUNCE_MS && heldMs < RESET_HOLD_MS) {
      showTimedMessage("ATTENDANCE",
                        "Present: " + String(students.countPresent()) + " / " +
                            String(students.count()),
                        outbox.size() > 0 ? "Unsynced: " + String(outbox.size()) : String(""),
                        "Tap ID card", SUMMARY_DISPLAY_MS);
    }
    // A hold >= RESET_HOLD_MS here is deliberately ignored - see above.
  }
}

// ---------------------------------------------------------------------
// Outbox -> cloud
// ---------------------------------------------------------------------

// Starts uploading the oldest queued tap if nothing is in flight, Wi-Fi
// is up, and the retry backoff has elapsed.
void pumpUploads() {
  if (cloud.busy() || outbox.empty() || !cloud.isConnected()) return;
  if ((long)(millis() - nextUploadAt) < 0) return;

  const OutboxEntry entry = outbox.front();
  int index;
  String name;
  if (students.lookup(String(entry.uid), index) != LookupResult::UNKNOWN_CARD) {
    name = students.at(index).name;
  } else {
    // The student was REMOVEd after tapping. The tap still happened, so
    // it is still uploaded (the UID column says whose card it was).
    name = "(removed)";
  }
  if (!cloud.startUpload(String(entry.uid), name, entry.ts)) {
    // Couldn't start the task (low memory?) - don't retry every loop().
    nextUploadAt = millis() + UPLOAD_RETRY_MIN_MS;
  }
}

void handleUploadResult(const SyncOutcome& o) {
  String name(o.uid);
  int index;
  if (students.lookup(String(o.uid), index) != LookupResult::UNKNOWN_CARD) {
    name = students.at(index).name;
  }

  String line1 = name, line2, line3;
  const char* title;

  switch (o.action) {
    case BackendAction::DELIVERED:
      outbox.popIfFront(o.uid, o.ts);
      uploadFailures = 0;
      nextUploadAt = 0;
      Serial.printf("[SYNC] %s: %s\n", o.uid, o.message);
      title = "ATTENDANCE LOGGED";
      line2 = String(o.message).startsWith("Duplicate") ? "Already in sheet" : "Status: Present";
      line3 = "Cloud: Synced";
      break;

    case BackendAction::DROP:
      outbox.popIfFront(o.uid, o.ts);
      uploadFailures = 0;
      Serial.printf("[SYNC] %s rejected for good, dropped: %s\n", o.uid, o.message);
      title = "CLOUD REJECTED";
      line2 = "Not uploaded";
      line3 = "See Serial log";
      break;

    case BackendAction::RETRY:
    default: {
      uploadFailures++;
      unsigned long backoff = UPLOAD_RETRY_MIN_MS;
      for (int i = 1; i < uploadFailures && backoff < UPLOAD_RETRY_MAX_MS; i++) backoff *= 2;
      if (backoff > UPLOAD_RETRY_MAX_MS) backoff = UPLOAD_RETRY_MAX_MS;
      nextUploadAt = millis() + backoff;

      Serial.printf("[SYNC] %s not delivered (HTTP %d: %s) - retry in %lu s, %d queued\n", o.uid,
                    o.httpCode, o.message, backoff / 1000UL, outbox.size());
      title = "SAVED LOCALLY";
      line2 = "Will sync later";
      line3 = String(o.message).indexOf("API key") >= 0 ? "Check API key!"
                                                         : "Err: " + String(o.httpCode);
      break;
    }
  }

  // Only interrupt the screen for the tap that was JUST made; older
  // queued taps draining in the background stay quiet (Serial only).
  if (name.length() > 0 && String(o.uid) == lastScannedUid &&
      (millis() - lastScanMs) < RESULT_BANNER_WINDOW_MS) {
    showTimedMessage(title, line1, line2, line3, SUCCESS_DISPLAY_MS);
  }
}

// ---------------------------------------------------------------------
// Housekeeping
// ---------------------------------------------------------------------

void checkReaderHealth() {
  bool ok = readerResponds();
  if (!ok) {
    // Try to bring it back - a re-init recovers a reader that browned out
    // or glitched. A truly unplugged one just fails the check again.
    rfid.PCD_Init();
    ok = readerResponds();
  }
  if (ok != readerOk) {
    readerOk = ok;
    Serial.println(ok ? F("[RFID] Reader is responding again.")
                      : F("[RFID] Reader NOT responding - check SDA/SCK/MOSI/MISO/RST/3.3V wiring."));
    if (!ok) feedback.error();
  }
}

void checkDayRollover() {
  uint32_t epoch = nowEpoch();
  if (epoch == 0) return;  // clock not set yet
  bool firstCheck = !students.dayConfirmed();
  if (students.rolloverIfNewDay(dayKey((time_t)epoch))) {
    Serial.println(F("[DAY] New day - earlier attendance cleared."));
    showTimedMessage("NEW DAY", "Earlier attendance", "cleared", "Tap ID card", SUCCESS_DISPLAY_MS * 2);
  } else if (firstCheck) {
    Serial.println(F("[DAY] Clock set - attendance saved earlier today restored."));
  }
}

// Leftover placeholders in secrets.h are the #1 reason a freshly flashed
// board "does nothing": warn loudly instead of failing silently.
void checkConfiguration() {
  bool ok = true;
  String url = GOOGLE_SCRIPT_URL;
  if (url.indexOf("YOUR_SCRIPT_ID") >= 0) {
    Serial.println(F("[CONFIG] GOOGLE_SCRIPT_URL in secrets.h is still the placeholder - nothing will upload."));
    ok = false;
  } else if (!url.endsWith("/exec")) {
    Serial.println(F("[CONFIG] GOOGLE_SCRIPT_URL should end in /exec (the Web app URL from Deploy, not /dev)."));
    ok = false;
  }
  if (String(API_KEY) == "change-this-shared-secret") {
    Serial.println(F("[CONFIG] API_KEY in secrets.h is still the placeholder."));
    ok = false;
  }
  String ssid = WIFI_SSID;
  if (ssid == "Wokwi-GUEST" || ssid == "YOUR_WIFI_NAME" || ssid.length() == 0) {
    Serial.println(F("[CONFIG] WIFI_SSID in secrets.h is a placeholder, not your network. "
                     "Put your phone hotspot's name and password there."));
    ok = false;
  }
  if (String(WIFI_PASSWORD).length() > 0 && String(WIFI_PASSWORD).length() < 8) {
    Serial.println(F("[CONFIG] WIFI_PASSWORD is shorter than 8 characters - a WPA2 password can't be."));
    ok = false;
  }
  if (!ok) {
    oled.showMessage("CHECK secrets.h", "Setup problem -", "see Serial monitor", "");
    feedback.error();
    delay(3000);
  }
}

// Hold the reset button through power-on to wipe today's attendance (e.g.
// at the start of a new class). Unsynced taps in the outbox are NOT wiped
// - they still upload. Runs once, before the main loop, so blocking is
// fine here.
void bootResetCheck() {
  if (digitalRead(PIN_BUTTON_RESET) != LOW) return;

  oled.showMessage("RESET ATTENDANCE", "Keep holding...", "Release to cancel", "");
  unsigned long start = millis();
  while (millis() - start < RESET_HOLD_MS) {
    if (digitalRead(PIN_BUTTON_RESET) == HIGH) {
      Serial.println(F("[SETUP] Reset cancelled (released early)."));
      oled.showMessage("RESET CANCELLED", "Released too soon", "Nothing was wiped", "");
      delay(1000);
      return;
    }
    delay(10);
  }

  // Held long enough - but only wipe once it is let go. A button wired
  // across two legs that are always connected reads "held" forever, and
  // must not silently wipe attendance on every power-on.
  oled.showMessage("RESET ATTENDANCE", "Now RELEASE the", "button to confirm", "");
  start = millis();
  while (digitalRead(PIN_BUTTON_RESET) == LOW) {
    if (millis() - start >= BUTTON_RELEASE_WAIT_MS) {
      Serial.println(F("[SETUP] Reset button never released - is it wired across two legs that are "
                       "always connected? Use diagonally opposite legs. Nothing was wiped."));
      oled.showMessage("BUTTON STUCK?", "Nothing was wiped", "Check button wiring", "");
      feedback.error();
      delay(2500);
      return;
    }
    delay(10);
  }

  students.resetAttendance();
  Serial.println(F("[SETUP] Attendance reset via button hold."));
  oled.showMessage("ATTENDANCE RESET", "All records wiped", "Starting fresh", "session");
  feedback.success();
  delay(1500);
}

void setup() {
  Serial.begin(115200);
  feedback.begin();
  students.begin();  // enrolled roster + saved attendance
  outbox.begin();    // taps still waiting to upload

  pinMode(PIN_BUTTON_RESET, INPUT_PULLUP);

  // The OLED must be initialized before any showMessage() call - the
  // Adafruit_SSD1306 driver allocates its frame buffer inside begin().
  // A missing display is NOT fatal: attendance still works through the
  // LEDs, buzzer and Serial, so don't brick the reader over it.
  if (oled.begin()) {
    Serial.printf("[SETUP] OLED found at I2C 0x%02X.\n", oled.address());
  } else {
    Serial.println("[WARN] OLED not found at I2C 0x3C or 0x3D - running without a display. "
                   "Check SDA->21, SCL->22, VCC->3V3, GND.");
    feedback.error();
  }

  bootResetCheck();      // first: the button is being held from power-on
  checkConfiguration();

  oled.showMessage("SMART ATTENDANCE", "Booting System...", "Please wait", "");

  SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
  rfid.PCD_Init();
  readerOk = readerResponds();
  Serial.printf("[SETUP] MFRC522 version 0x%02X (%s)\n", readerVersion(),
                readerOk ? "OK" : "NOT RESPONDING - check wiring");

  feedback.networkBusy(true);
  oled.showMessage("WI-FI CONNECTION", "Connecting to:", cloud.ssid(), "");
  Serial.printf("[SETUP] Connecting to Wi-Fi \"%s\"", cloud.ssid());
  bool wifiOk = cloud.connect();
  cloud.beginAsync();

  if (wifiOk) {
    Serial.println("\n[SETUP] Wi-Fi connected.");
    feedback.wifiConnected();
    // Give NTP a moment, so the first tap already has a real timestamp
    // and yesterday's "present" flags are settled before anyone taps.
    oled.showMessage("WI-FI CONNECTED", "Getting the time...", "", "");
    unsigned long start = millis();
    while (nowEpoch() == 0 && millis() - start < CLOCK_SYNC_WAIT_MS) delay(100);
    Serial.println(nowEpoch() ? "[SETUP] Clock set via NTP." : "[SETUP] Clock not set yet - will keep trying.");
  } else {
    Serial.println("\n[SETUP] Wi-Fi unavailable - continuing offline; will keep retrying.");
  }
  feedback.networkBusy(false);
  Serial.printf("[SETUP] %d tap(s) waiting to upload.\n", outbox.size());

  printHelp();
  drawReady();
  checkDayRollover();  // may show "NEW DAY" over the ready screen
  lastDayCheckMs = millis();
}

void loop() {
  feedback.update();     // turn off a timed-out success/error LED
  pollSerialCommands();
  pollResetButton();
  cloud.maintain();      // reconnect Wi-Fi if dropped, start NTP once up
  feedback.networkBusy(cloud.busy());

  SyncOutcome outcome;
  if (cloud.pollResult(outcome)) handleUploadResult(outcome);
  pumpUploads();

  unsigned long now = millis();
  if (now - lastReaderCheckMs >= READER_HEALTH_CHECK_MS) {
    lastReaderCheckMs = now;
    checkReaderHealth();
  }
  if (now - lastDayCheckMs >= DAY_CHECK_INTERVAL_MS) {
    lastDayCheckMs = now;
    checkDayRollover();
  }

  // The ENROLL window closed without a card.
  if (enrollArmed && (long)(now - enrollDeadline) >= 0) {
    enrollArmed = false;
    Serial.println(F("[ENROLL] Timed out - no card tapped."));
  }

  // Back to the ready screen once a message's hold time is over; and
  // while idle, keep it in step with Wi-Fi / queue / reader changes.
  if (screenState == ScreenState::MESSAGE) {
    if (now - messageShownAt >= messageHoldMs) drawReady();
  } else if (cloud.isConnected() != shownWifi || outbox.size() != shownPending ||
             readerOk != shownReaderOk) {
    drawReady();
  }

  // Poll the reader at a steady rate rather than flat out: a tap is
  // seconds long, so ~25 polls/s is plenty and keeps the SPI bus and CPU
  // calm.
  if (now - lastReaderPollMs < READER_POLL_MS) return;
  lastReaderPollMs = now;

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  String uid = readCardUID();

  // Release the card right away, before any display/beep/lookup work, so
  // the antenna is free for the next tap as fast as possible.
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // Same-card cooldown: swallow repeat reads of the same UID (see config.h).
  // Not while ENROLL is waiting - that card was probably just tapped to see
  // its UID, and the teacher shouldn't have to wait out the cooldown.
  if (!enrollArmed && uid == lastScannedUid && (now - lastScanMs) < SAME_CARD_COOLDOWN_MS) {
    return;
  }
  lastScannedUid = uid;
  lastScanMs = now;

  Serial.print("\n[SCAN] UID: ");
  Serial.println(uid);

  // ENROLL <name> was typed: this tap is the card to enroll.
  if (enrollArmed) {
    enrollArmed = false;
    enrollAndReport(uid, enrollName, "via card tap");
    return;
  }

  int index;
  LookupResult result = students.lookup(uid, index);

  if (result == LookupResult::UNKNOWN_CARD) {
    showTimedMessage("ACCESS DENIED", "Unknown Card!", "UID: " + uid, "Not Registered",
                      RESULT_DISPLAY_MS);
    Serial.println("[SCAN] Unknown card. To enroll it: ENROLL <Name>, then tap again.");
    feedback.error();
    return;
  }

  if (result == LookupResult::ALREADY_LOGGED) {
    showTimedMessage("ALREADY LOGGED", students.at(index).name, "Status: Present",
                      "Duplicate scan", RESULT_DISPLAY_MS);
    feedback.error();
    return;
  }

  // NEW_ENTRY: record it durably FIRST (flash + outbox), then tell the
  // student. The cloud upload happens in the background whenever Wi-Fi
  // allows; even a power cut right now loses nothing.
  const String& name = students.at(index).name;
  students.markLogged(index);
  if (!outbox.push(uid, nowEpoch())) {
    Serial.println("[SYNC] WARNING: upload queue was full - the oldest unsynced tap was discarded.");
  }
  showTimedMessage("WELCOME!", name, cloud.isConnected() ? "Saving to cloud..." : "Saved (offline)",
                    "", SUCCESS_DISPLAY_MS);
  feedback.success();
  nextUploadAt = 0;  // a fresh tap shouldn't wait out an old backoff
}
