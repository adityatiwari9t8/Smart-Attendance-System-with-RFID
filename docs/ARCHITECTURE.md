# Architecture

## Module responsibilities

| Module | Files | Responsibility |
|---|---|---|
| `config.h` | `include/config.h` | Every pin number and timing constant. Nothing else hardcodes a GPIO. `test/check_wiring.py` checks it against the README wiring table and ESP32 pin-safety rules. |
| `secrets.h` | `include/secrets.h` (gitignored) | Wi-Fi credentials, Apps Script URL, shared API key. |
| `StudentDatabase` | `StudentDatabase.*` | Roster (built-in + enrolled, add/remove), lookup, per-day attendance flags, NVS persistence. No hardware, no networking. |
| `Outbox` | `Outbox.*` | Persistent FIFO of taps not yet confirmed by the backend. No hardware, no networking. |
| `BackendResponse` | `BackendResponse.*` | Pure function: HTTP code + reply text → `DELIVERED` / `DROP` / `RETRY`. |
| `AttendanceLogger` | `AttendanceLogger.*` | Wi-Fi (connect + reconnect), NTP, and the HTTPS upload on a background FreeRTOS task. One upload in flight at a time. |
| `DisplayUI` | `DisplayUI.*` | SSD1306 behind one `showMessage(title, l1, l2, l3)`; probes I²C 0x3C/0x3D at boot; clips long lines; no-ops if the OLED is absent. |
| `FeedbackIndicators` | `FeedbackIndicators.*` | 3 LEDs + buzzer, non-blocking hold times. |
| `main.cpp` | `src/main.cpp` | Non-blocking state machine: reads cards, drives the modules, runs the serial console and the upload pump. |

The three "no hardware" modules (`StudentDatabase`, `Outbox`,
`BackendResponse`) plus `TimeUtil.h` are compiled and unit-tested on the host
(`pio test -e native`) against mock `String`/`Preferences`.

## Request flow (happy path)

```
Card tapped
  -> main.cpp reads the UID, PICC_HaltA() immediately (reader free for the next tap)
  -> StudentDatabase.lookup(uid) -> NEW_ENTRY
  -> StudentDatabase.markLogged()      # flash: student is PRESENT, durably
  -> Outbox.push(uid, unixTime)        # flash: "still owes the sheet a row", durably
  -> OLED "WELCOME!", green LED + beep # instant feedback; the tap is already safe
  ...
  loop() -> pumpUploads(): Wi-Fi up, nothing in flight, backoff elapsed?
  -> AttendanceLogger.startUpload(front of outbox)   # task on core 0
        -> HTTPS GET  ?uid&name&key&ts   to Apps Script
        -> Apps Script: check key, check duplicate for the TAP's day,
           append row, reply "Success: ..." / "Duplicate: ..."
        -> BackendResponse classifies the reply text -> result queue
  -> loop() -> pollResult(): DELIVERED -> Outbox.popIfFront(), banner on the OLED
```

If any step after the tap fails (no Wi-Fi, TLS error, wrong key, backend
busy, power cut), the record is simply still in the outbox and the pump tries
again — backoff 2 s, 4 s, … capped at 60 s, reset by success or a fresh tap.

## Why an outbox

The first design marked a student present only after the cloud call returned
and, on any failure, said "Recorded Locally" — but nothing ever re-sent it, so
a Wi-Fi blip meant a silently missing row. Splitting "the tap happened"
(durable immediately) from "the sheet knows" (eventually, with retry) fixes
that:

- Present-ness is decided locally and never depends on the network, so entry
  feedback is instant and consistent offline or online.
- Power loss mid-upload is harmless: the record is still queued. If the
  request had actually reached the sheet, the retry is answered
  `Duplicate: …`, which counts as delivered.
- Each record carries the time it was tapped (NTP; omitted if the clock was
  never set). The backend dedupes by the *tap's* calendar day, so a record
  uploaded after midnight still lands on the right day.
- `popIfFront(uid, ts)` removes the entry only if it is still the one that was
  sent, so the queue can't lose the wrong record if it changes mid-upload.
- The queue is bounded (`OUTBOX_CAPACITY`); when full the oldest entry is
  discarded (and logged) so memory stays bounded.

## Why Apps Script replies are parsed

Apps Script returns HTTP 200 for errors too (`Error: Invalid or missing API
key` is a 200). Treating any 2xx as success would show "Synced" for taps that
never reached the sheet. `classifyBackendResponse` accepts only `Success` /
`Duplicate`; a bad request (`Missing uid or name`) is dropped so it can't block
the queue; everything else — wrong key, busy lock, Google's HTML error page —
is retried, because the record might still be good once the config is fixed.

## Why the upload runs on a second core, one at a time

HTTPS handshakes take seconds. Running them in `loop()` would freeze RFID
polling, so the request runs on a FreeRTOS task pinned to core 0 while
`loop()` stays on core 1. Only one task runs at a time: two concurrent TLS
sessions roughly double peak heap use, and the outbox already serializes the
work. The task reports back through a FreeRTOS queue; `SyncOutcome` is a plain
POD struct (fixed `char[]`, no `String`) because queues copy bytes, and a
`String` memcpy'd across a queue would end up with a dangling heap pointer.

## What survives a reset

Three independent NVS namespaces:

| Namespace | Holds | Cleared by |
|---|---|---|
| `attendance` | who is present today (+ the day it applies to) | `RESET`, boot-hold of the button, midnight rollover |
| `roster` | students enrolled at runtime, as slots `u<n>`/`m<n>` + `cnt` | `REMOVE` (one slot), erasing flash |
| `outbox` | taps awaiting upload | successful delivery only |

UIDs are used as NVS keys (15-character limit), so `enroll()` accepts only 4-
and 7-byte card UIDs (8 and 14 hex chars) and rejects anything else up front.
A new student always gets the next free slot (`cnt`, written last), never one
derived from its position in the roster, so restoring can skip a slot without
a later enrollment overwriting someone else. `REMOVE` erases that slot's
uid first, which makes it dead on the next restore.

## Time

`configTzTime(TZ_INFO, …)` starts NTP once Wi-Fi is up; at boot the firmware
waits up to `CLOCK_SYNC_WAIT_MS` for it. Until it answers, `time()` is near
1970; `isClockValid()` guards every use, and taps made before then are queued
with `ts = 0` so the backend stamps them on arrival.

The ESP32 has no battery-backed clock, so right after power-on it cannot tell
whether the present-flags in flash are today's or yesterday's. They therefore
stay in flash, **not counted**, until the clock is valid: only taps made since
power-on count as "present" until then. The first valid reading (checked every
second) settles it — `_day` equal to today restores the flags, anything else
(an earlier day, or no `_day` because the clock was never set) discards them
but keeps the taps made since power-on. After that, `rolloverIfNewDay()`
clears the flags at local midnight, matching the backend's once-per-day rule.

Erring this way is deliberate: a stale flag would turn a real tap away
(`ALREADY LOGGED`, nothing recorded), whereas an extra tap just goes into the
outbox and the backend answers `Duplicate`. The backend decides "today" in the
spreadsheet's time zone (File → Settings), the one its timestamps are shown in.

## Reader health

`PCD_ReadRegister(VersionReg)` returns `0x91`/`0x92` on a working MFRC522 and
`0x00`/`0xFF` when nothing answers on SPI (a loose wire); clones report other
values (`0x12`, `0x88`, `0xB2`) and work. It is checked at boot and every
`READER_HEALTH_CHECK_MS`; on failure the reader is re-initialised and the
ready screen shows `RFID: check wiring!`. A missing OLED is likewise non-fatal
— the system keeps working through LEDs, buzzer and Serial. (The Adafruit
driver's `begin()` can't detect a missing display, so `DisplayUI` probes the
I²C bus itself.)

## Hardware safety at boot

- `checkConfiguration()` warns (Serial + OLED) about placeholders left in
  `secrets.h`, a URL that isn't `/exec`, or a Wi-Fi password too short to
  be WPA2.
- The boot-hold wipe happens only when the button is **released** after the
  3 s hold. A button wired across two internally-joined legs reads "pressed"
  forever; it is reported as `BUTTON STUCK?` instead of wiping attendance on
  every power-on.

## Why `WiFiClientSecure::setInsecure()`

The ESP32 has no auto-updating CA store, and Google rotates its chains.
`setInsecure()` keeps the connection encrypted but doesn't verify the server
(the handshake itself is capped at `TLS_HANDSHAKE_TIMEOUT_MS`; the core's
default is 120 s);
acceptable here because the payload (a UID and a name) isn't sensitive and the
endpoint is guarded by `API_KEY`. A production deployment would use
`setCACert()` with Google's root (GTS Root R1) plus NTP-validated time — the
clock is now available for that, but a stale pinned root would silently stop
every upload, so it is left as a deliberate, documented trade-off.
