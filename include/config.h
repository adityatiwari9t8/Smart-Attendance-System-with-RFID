#pragma once
// =====================================================================
// config.h — Non-secret hardware & timing configuration
//
// Every pin you wire is defined here, and must match the wiring table in
// README.md. test/check_wiring.py checks both, plus ESP32 pin safety.
// If you change a pin here, update the README table too and redraw the
// pictures with `python3 tools/wiring_diagram.py`.
// =====================================================================

// ---------------------------------------------------------------------
// LEDs & Buzzer
// ---------------------------------------------------------------------
// GPIO12 and GPIO15 are ESP32 "strapping" pins: what's wired to them at
// power-up changes how the chip boots (GPIO12 high = wrong flash voltage
// = boot loop on many dev boards), so the red LED and buzzer live on
// 26/25 instead.
// Every output below also avoids the flash pins (6-11) and the
// input-only pins (34-39). Each LED needs a ~220 ohm series resistor, and
// the buzzer a ~100 ohm one (a "magnetic" buzzer's coil is only ~16-42
// ohm and would pull more current than a GPIO is rated for).
// test/check_wiring.py enforces all of this.
#define PIN_LED_GREEN   13   // Attendance accepted / success
#define PIN_LED_RED     26   // Rejected: unknown card or duplicate tap
#define PIN_LED_BLUE    14   // Busy: Wi-Fi connecting / cloud sync
#define PIN_BUZZER      25   // PASSIVE buzzer (driven by tone()), via 100 ohm

// ---------------------------------------------------------------------
// Attendance reset button
// A separate push button, not the board's BOOT button (that one is GPIO0,
// a strapping pin, and holding it at power-on enters flashing mode).
// Wired active-LOW with INPUT_PULLUP, so the other leg just goes to GND.
// ---------------------------------------------------------------------
#define PIN_BUTTON_RESET  4

// ---------------------------------------------------------------------
// MFRC522 RFID Reader (SPI)
// NOTE: RST is on GPIO27, NOT GPIO21 — GPIO21 is used by the OLED (SDA).
// Sharing that pin was a real bug in an earlier revision of this project.
// ---------------------------------------------------------------------
#define PIN_RFID_SS     5    // the module labels this pin "SDA"
#define PIN_RFID_RST    27
#define PIN_RFID_SCK    18   // the ESP32's default VSPI pins
#define PIN_RFID_MISO   19
#define PIN_RFID_MOSI   23

// ---------------------------------------------------------------------
// SSD1306 OLED Display (I2C)
// ---------------------------------------------------------------------
#define PIN_OLED_SDA        21
#define PIN_OLED_SCL        22
#define OLED_SCREEN_WIDTH    128
#define OLED_SCREEN_HEIGHT   64
#define OLED_RESET_PIN       -1     // Shares ESP32 reset, no dedicated pin
// Most modules answer at 0x3C (printed "0x78" on the back of some); a
// few at 0x3D ("0x7A"). DisplayUI probes both, so either just works.
#define OLED_I2C_ADDRESS      0x3C
#define OLED_I2C_ADDRESS_ALT  0x3D

// ---------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------
#define WIFI_CONNECT_TIMEOUT_MS   15000  // Give up and go offline after this
// Uploads run on a background task (see AttendanceLogger), so a generous
// timeout costs no scanning responsiveness; it just has to cover a slow
// phone hotspot.
#define HTTP_REQUEST_TIMEOUT_MS    8000  // Abort a hung cloud request
// The TLS handshake has its own timer, which defaults to 120 s on the
// ESP32 core - one stalled handshake on a flaky hotspot would block every
// upload behind it for two minutes.
#define TLS_HANDSHAKE_TIMEOUT_MS  15000
#define RESULT_DISPLAY_MS           900  // How long ACCESS DENIED / DUPLICATE stays up
#define SUCCESS_DISPLAY_MS          900  // How long WELCOME / LOGGED stays up
#define SUCCESS_BEEP_HOLD_MS        150  // LED/beep hold time on success
#define ERROR_BEEP_HOLD_MS          200  // LED/beep hold time on error
#define RESET_HOLD_MS              3000  // Hold reset button this long to wipe attendance
// After the hold, the wipe only happens once the button is RELEASED. A
// button wired across two legs that are always connected reads "pressed"
// forever; if it is still down this long after the hold, nothing is wiped.
#define BUTTON_RELEASE_WAIT_MS     5000

// Ignore repeat reads of the SAME card UID within this window. Cheap
// MFRC522 modules commonly re-detect a card that is still resting on
// (or bouncing near) the antenna as a "new" tap even after PICC_HaltA(),
// which is what makes a single physical tap look like a double-scan.
// This is independent of the per-session `logged` flag in
// StudentDatabase — that only stops a duplicate row from being written,
// it doesn't stop the reader from re-processing the same tap and
// re-beeping/re-displaying "ALREADY LOGGED" a moment later.
#define SAME_CARD_COOLDOWN_MS      3000

// How long the "X of Y present" summary screen stays up after a short
// press of the reset button (a short press, unlike the boot-time
// long-hold, does NOT wipe anything — see main.cpp::loop()).
#define SUMMARY_DISPLAY_MS         3000

// A button press shorter than this is treated as switch bounce and
// ignored, rather than as a real short press.
#define BUTTON_DEBOUNCE_MS          40

// How many completed background sync results can be queued up waiting
// for loop() to read them. 4 is generous for a single RFID reader —
// taps can't physically arrive faster than a person can tap a card.
#define SYNC_QUEUE_LENGTH             4

// ---------------------------------------------------------------------
// Roster capacity
// Students are enrolled at runtime (ENROLL / ADD over Serial) and kept in
// flash. MAX_STUDENTS is the ceiling on how many the roster can hold;
// raise it for a bigger class (each student costs ~100 bytes of RAM).
// ---------------------------------------------------------------------
#define MAX_STUDENTS                 20

// ---------------------------------------------------------------------
// Cloud upload outbox (see Outbox.h)
// ---------------------------------------------------------------------
#define OUTBOX_CAPACITY              60      // unsynced taps kept in flash
#define UPLOAD_RETRY_MIN_MS        2000      // first retry after a failed upload
#define UPLOAD_RETRY_MAX_MS       60000      // backoff ceiling
#define WIFI_RECHECK_MS           20000      // how often to nudge a dropped Wi-Fi link
#define RESULT_BANNER_WINDOW_MS   20000      // show the sync result on the OLED only if the tap was this recent

// ---------------------------------------------------------------------
// Clock (NTP) - gives every tap a real timestamp, so records uploaded
// late (after an outage) keep the time they were TAPPED, and the roster's
// "present" flags clear themselves at midnight.
// POSIX TZ string; default is India (UTC+5:30, no DST). Examples:
//   "UTC0"   "GMT0BST,M3.5.0/1,M10.5.0"   "EST5EDT,M3.2.0,M11.1.0"
// ---------------------------------------------------------------------
#define TZ_INFO                      "IST-5:30"
#define NTP_SERVER_1                 "pool.ntp.org"
#define NTP_SERVER_2                 "time.google.com"
// Checked every second: right after power-on the clock arrives within a
// few seconds, and yesterday's "present" flags must be cleared before the
// first student taps (see StudentDatabase::rolloverIfNewDay()).
#define DAY_CHECK_INTERVAL_MS        1000
// At boot, once Wi-Fi is up, wait this long for NTP so the very first tap
// already has a real timestamp and yesterday's flags are already gone.
#define CLOCK_SYNC_WAIT_MS           5000

// ---------------------------------------------------------------------
// RFID reader health
// ---------------------------------------------------------------------
#define READER_POLL_MS                40     // how often to look for a card
#define READER_HEALTH_CHECK_MS      5000     // how often to verify the MFRC522 still answers on SPI
#define ENROLL_WINDOW_MS           15000     // how long ENROLL <name> waits for a card

// ESP32 NVS (Preferences) keys are capped at 15 characters, and
// StudentDatabase uses a student's UID directly as the flash key in the
// "attendance" namespace. enroll() rejects anything longer than this so a
// too-long UID fails loudly at enrollment time instead of silently
// failing to persist later. Card UIDs are 4, 7 or 10 bytes (8, 14 or 20
// hex digits); the first two fit.
#define MAX_UID_LENGTH               15

// ---------------------------------------------------------------------
// Buzzer tones
// ---------------------------------------------------------------------
#define TONE_SUCCESS_HZ   1500
#define TONE_SUCCESS_MS    150
#define TONE_ERROR_HZ      500
#define TONE_ERROR_MS      400
#define TONE_WIFI_OK_HZ   2000
#define TONE_WIFI_OK_MS    100
