# Changelog

## Unreleased - Real hardware only

- Dropped the Wokwi simulator: removed the `wokwi` env, `wokwi.toml`,
  `diagram.json` and the four built-in demo students. The roster starts
  empty; enroll cards with `ENROLL` / `ADD`.
- `test/check_wiring.py` now checks `config.h` against the README wiring
  table (what you actually wire from) plus ESP32 pin-safety rules, instead
  of against the Wokwi diagram.
- RFID SPI pins (SCK 18, MISO 19, MOSI 23) are explicit in `config.h` and
  passed to `SPI.begin()`, so every wired pin is defined in one place.
- Boot check also flags a Wi-Fi password shorter than WPA2's 8 characters.
- `test/README`: the simulator checklist became a hardware acceptance
  checklist.

## Unreleased - Hardware bring-up review

Reviewed everything against what happens on a real board (library sources,
boot sequence, wiring), not just in the simulator.

**Bugs fixed**
- **First taps of the day were turned away.** Present-flags from the previous
  session were loaded at power-on, but the date was first checked 30 s later
  - so the first students to tap got `ALREADY LOGGED` and their tap was never
  recorded. Booted without Wi-Fi (no clock), it stayed that way all day.
  Saved flags now only count once the clock confirms they're from today;
  the date is checked every second, and boot waits up to 5 s for NTP.
- **ENROLL could erase another student.** The flash slot for a new student
  came from its roster position, so after a slot was skipped on restore the
  next enrollment overwrote a different saved student. Slots are now
  allocated from the saved count.
- **OLED detection never worked.** `Adafruit_SSD1306::begin()` doesn't check
  that a display answers, so "SSD1306 not found" could never appear and
  `STATUS` said "OLED: OK" with nothing connected. The I2C bus is probed
  now, and 0x3C / 0x3D are both tried automatically.
- **A stalled TLS handshake blocked uploads for 2 minutes** (the ESP32 core's
  default handshake timeout is 120 s). Capped at 15 s.
- `ADD 3F 2A 9B 10 Name` silently enrolled the UID `3F`. UIDs must now be 8 or
  14 hex digits; `:`/`-` separators are accepted.
- The VS Code Upload button / bare `pio run` built every env: flashed the
  board twice, then failed on the native test env. `hardware` is now the
  default env.
- The same-card cooldown swallowed the ENROLL tap if that card had just been
  tapped to read its UID.
- Backspace in the serial monitor corrupted commands; typed text wasn't echoed.
- Backend: "today" was decided in the Apps Script project's time zone (hidden,
  often US) instead of the sheet's own.

**Hardware safety**
- 100 ohm series resistor for the buzzer (diagram, BOM, `check_wiring.py`):
  a magnetic buzzer's ~16-42 ohm coil overloads a GPIO.
- Boot-hold reset wipes only after the button is released; a button wired
  across its joined legs is reported (`BUTTON STUCK?`) instead of wiping
  attendance on every power-on.
- Boot-time `[CONFIG]` warnings for placeholders in `secrets.h`, a non-`/exec`
  URL, or `Wokwi-GUEST` on a real board.
- README: classic-ESP32-only, SSD1306 vs SH1106, OLED pin-order and
  MFRC522-soldering warnings, diagonal button legs, staged bring-up,
  browser test of the backend, Apps Script authorization steps.

**Features**
- `REMOVE <UID>` un-enrolls a student.
- PlatformIO envs: `hardware` (default), `wokwi` (simulator; always joins
  Wokwi-GUEST, so one `secrets.h` serves both), `native`. Library versions
  pinned exactly; crash backtraces decoded in the serial monitor.
- `STATUS` shows the day-confirmation state, OLED address and largest free
  heap block.

**Tests**
- Native: 25 tests (was 17), incl. regressions for each bug above, verified
  to fail on the previous code. Backend: 16 (was 15).

## Unreleased - Real-hardware readiness, durable uploads, automated tests

**Bugs fixed**
- **False "synced".** Apps Script replies HTTP 200 even for errors (wrong API
  key, busy lock, error page), and the firmware treated every 2xx as success,
  so taps could be shown as synced and never reach the sheet. Replies are now
  classified by their text (`BackendResponse`).
- **Lost records.** A tap that failed to upload (offline or HTTP error) was
  marked present locally and never re-sent. Taps now go into a persistent
  outbox (`Outbox`) and are retried with backoff until the backend confirms.
- **No Wi-Fi recovery.** A failed boot-time connection (or a later drop) left
  the device offline forever. Wi-Fi now auto-reconnects and `isConnected()`
  reflects the live link.
- **Stale "present" flags.** Attendance never cleared on its own, while the
  backend dedupes per day - the next morning everyone read "ALREADY LOGGED".
  Flags now clear at midnight once the clock is set.
- Concurrent taps spawned concurrent TLS sessions (heap risk); uploads are
  now strictly one at a time.
- Backend: a blank/hand-edited timestamp cell made every later request throw
  (`formatDate` on an invalid Date); such rows are now skipped.
- Long names overflowed the OLED into the next line; lines are clipped.
- A restored enrollment could duplicate a built-in student's UID; skipped.
- A failed OLED init halted the whole board; it now continues headless.

**Hardware safety (found by reviewing the wiring for a real build)**
- Red LED moved GPIO12 -> 26 and buzzer GPIO15 -> 25: both were ESP32
  *strapping* pins that can stop the board booting on real hardware.
- Added the missing 220 ohm series resistors on all three LEDs (diagram + BOM).
- OLED powered from 3V3 instead of 5V (5V pulled the I2C lines to 5V).
- The Apps Script file shipped with the real API key committed to git; it now
  has a placeholder and refuses to run until configured. **If that repo was
  ever public, rotate the key** (it remains in git history).

**Features**
- `ENROLL <Name>` (tap the card), `STATUS`, `SELFTEST`, `RESET` serial
  commands.
- NTP time: each tap carries its real timestamp (`ts`) so late uploads keep
  their tap time; backend dedupes by the tap's day.
- MFRC522 health check at boot and every 5 s, with auto re-init and an
  on-screen warning; reader polled at a fixed 25 Hz.
- `hardware` PlatformIO env (no demo roster); `espressif32` platform pinned;
  `-Wall -Wextra`.
- README: shopping list (incl. 125 kHz vs 13.56 MHz card warning), full wiring
  table, power-on checklist, troubleshooting.

**Tests (new)**
- `pio test -e native` (17 host unit tests), `node --test` backend suite
  (15 tests against the real `AppsScript.gs`), `test/check_wiring.py`,
  `test/run_all.sh`, and a GitHub Actions workflow.

## Unreleased — Apps Script hardening (backend)

**Reliability**
- `doGet()` is now wrapped in a try/catch. Previously, an uncaught
  exception (e.g. a transient Spreadsheet API error) made Apps Script
  serve its own HTML error page — which Apps Script still returns as
  HTTP 200, so `HTTPClient` would read it as success, parse garbled
  HTML into the sync message, and the firmware would mark the student
  logged even though nothing was written to the sheet. Every path now
  reliably returns the plain-text response the firmware expects.
- Added `LockService.getScriptLock()` around the duplicate-check +
  `appendRow()` section. Without it, two students tapping within the
  same second (test/README scenario 7) could both pass the "not a
  duplicate yet" check before either had written a row, producing two
  rows for effectively-simultaneous taps — a real race, not just a
  theoretical one, since Apps Script can run concurrent `doGet()`
  invocations for the same deployment.

## Unreleased — Retry connection-level sync failures

**Reliability**
- `AttendanceLogger::doSync()` now retries up to 3 times when a sync
  attempt fails at the connection/TLS level (a negative `HTTPClient`
  code, e.g. mbedTLS `-29312` "SSL - The connection indicated an EOF"),
  with a short backoff between attempts (400ms, 800ms). A real HTTP
  response — 2xx, 4xx, or 5xx — is never retried, so a genuine
  duplicate/rejected response from the Apps Script backend can't turn
  into a repeated write.
  - This targets Wokwi's simulated network occasionally dropping a
    connection mid-handshake and the first HTTPS handshake right after
    Wi-Fi association being flakier than later ones — both more likely
    explanations for an EOF-during-handshake than a stack-size problem.
    An earlier revision of this fix guessed the FreeRTOS sync task's
    stack was too small (10KB → 20KB); that guess wasn't well-founded
    (a real stack overflow on ESP32 typically panics and reboots rather
    than failing cleanly inside mbedTLS) and is superseded by this
    retry, which addresses the failure whether it's Wokwi network
    flakiness or handshake timing. The larger stack is left in place
    since it's harmless.
  - Each retry opens a fresh `WiFiClientSecure`/`HTTPClient` pair rather
    than reusing one whose handshake already failed partway through.
  - Still runs entirely on the background sync task (core 0), so
    retrying never blocks the RFID reader on core 1.

## Unreleased — Async architecture, dynamic roster, non-blocking loop

**Performance / architecture**
- The cloud sync (`AttendanceLogger.logAttendance()`) used to run
  synchronously in `loop()`, blocking the RFID reader for the entire
  HTTPS round trip. It now runs on a dedicated FreeRTOS task pinned to
  the ESP32's second core (`logAttendanceAsync()`), reporting back
  through a queue (`pollResult()`). The reader is never blocked by a
  slow or hung network call. See `docs/ARCHITECTURE.md` for the full
  reasoning, including why the queued struct uses `char[]` instead of
  `String` members.
- Removed every remaining `delay()` call from `loop()`. LED/buzzer hold
  times (`FeedbackIndicators::update()`), result-screen display time,
  and reset-button press length are all tracked with `millis()`
  instead, so the RFID reader and the new Serial command console stay
  responsive at all times.
- `rfid.PICC_HaltA()`/`PCD_StopCrypto1()` now run immediately after
  reading a card's UID instead of at the end of `loop()`, releasing the
  reader for the next tap right away instead of after the full
  display/beep/sync sequence.
- Added a same-UID cooldown (`SAME_CARD_COOLDOWN_MS`) so a card
  lingering near the antenna isn't reprocessed as a second scan.

**Features**
- **Dynamic roster enrollment.** `StudentDatabase::enroll()` adds a new
  student at runtime, persisted to a new `"roster"` NVS namespace
  (separate from the existing `"attendance"` namespace, so wiping
  attendance between classes doesn't un-enroll anyone). Exposed via a
  Serial command console in `main.cpp`: `ADD <UID> <Name>`, `LIST`,
  `HELP`. Roster capacity is capped at `MAX_STUDENTS` (config.h).
- **Attendance summary view.** A short press of the reset button during
  normal operation (distinct from the boot-time long-hold-to-wipe)
  shows "Present: X / Y" on the OLED for a few seconds.
- The Apps Script's actual response text is now read
  (`HTTPClient::getString()`) and shown/logged, instead of only
  checking the HTTP status code.

## Unreleased — NVS persistence & reset button

**Reliability**
- Attendance state now survives a reset or power loss mid-class.
  `StudentDatabase` is backed by the ESP32's NVS flash via the
  `Preferences` library instead of a RAM-only array: `begin()` restores
  each student's saved `logged` state on boot, `markLogged()` writes
  through to flash immediately.
- Added a physical reset button (`PIN_BUTTON_RESET`, GPIO4) — hold it
  through boot for 3s to wipe the saved attendance and start a fresh
  session (e.g. the next class). Release early to cancel. Wired as a
  real `wokwi-pushbutton` part rather than the devkit's silkscreen BOOT
  button, since we couldn't confirm that's clickable in Wokwi's UI.
- The Apps Script's server-side duplicate check remains in place as a
  second backstop, independent of whether the reset button gets
  pressed between sessions.

**Note on ESP32 NVS key limits:** each student's UID is used directly as
the NVS storage key, and NVS keys are capped at 15 characters. Our UIDs
(8 hex chars) are safely under that; keep it in mind if the roster ever
moves to a longer UID format.

## Unreleased — Code review & hardening pass

**Bug fixes**
- Fixed a real pin conflict: RFID `RST` was wired to GPIO21, which is also
  the OLED's I2C SDA line. Moved to GPIO27 in both `main.cpp` and
  `diagram.json`.
- HTTPS requests to the Apps Script now use `WiFiClientSecure`
  explicitly. The previous code called `http.begin(url)` with an
  `https://` URL and no secure client configuration, which is not
  guaranteed to negotiate TLS correctly on all ESP32 Arduino core
  versions.
- Wi-Fi connect no longer blocks forever — added a 15s timeout with a
  graceful fallback to offline mode.
- `students[i].logged` is now set *after* the cloud sync attempt
  completes, not before, so a mid-request crash/reset can't leave a
  student marked present with nothing actually sent.

**Security**
- Removed the hardcoded Wi-Fi/Apps-Script URL from `main.cpp`. Real
  values now live in `include/secrets.h`, which is gitignored;
  `include/secrets.example.h` is the committed template.
- Added a shared-secret `API_KEY` check on both firmware and backend, so
  the Apps Script URL can't be used to write junk data if it leaks or is
  guessed.
- Backend now rejects requests with a missing/invalid key or missing
  `uid`/`name` instead of silently writing incomplete rows.

**Reliability**
- Backend now checks for a duplicate UID logged earlier the same day and
  skips re-appending a row, as a server-side backstop for the fact that
  the device's own "already logged" state resets on every reboot.
- HTTP success is now checked as a `200–299` range instead of `> 0`
  (which also treated negative `HTTPClient` error codes as "success").

**Structure**
- Split the single 230-line `main.cpp` into `StudentDatabase`,
  `DisplayUI`, `FeedbackIndicators`, and `AttendanceLogger` modules with
  matching headers in `include/`. `main.cpp` is now orchestration only.
- Added `docs/ARCHITECTURE.md`, this changelog, `LICENSE` (MIT), and a
  full project `README.md`.
- Moved the backend into `backend/AppsScript.gs` so it's version
  controlled alongside the firmware instead of living only in the Apps
  Script editor.
