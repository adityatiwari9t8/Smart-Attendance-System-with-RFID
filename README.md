# 🎓 ESP32 Smart Attendance System

![C++](https://img.shields.io/badge/Language-C++-blue.svg)
![Platform](https://img.shields.io/badge/Platform-ESP32-lightgrey.svg)
![Framework](https://img.shields.io/badge/Framework-Arduino-00979D.svg)
![License](https://img.shields.io/badge/License-MIT-green.svg)

An RFID-based classroom attendance system on an ESP32. Students tap a card;
the reader checks it against the roster, shows the result on an OLED with
LEDs and a buzzer, and logs the tap to a Google Sheet. Built for the
3rd-semester IoT & Robotics Lab.

A tap is **never lost**: it is saved to flash and queued the instant it is
accepted, and stays queued until the Google Sheet confirms it — so Wi-Fi
drops, a backend outage, or pulling the plug mid-class lose nothing. See
[Architecture](docs/ARCHITECTURE.md).

> **🌍 SDG alignment:** UN Sustainable Development Goal 4 — Quality
> Education — by digitizing attendance tracking to reduce administrative
> overhead and give faculty real-time visibility into classroom presence.

---

## Table of contents

- [Features](#-features)
- [Build it on real hardware](#-build-it-on-real-hardware)
- [Wiring](#-wiring)
- [Bring it up in stages](#-bring-it-up-in-stages)
- [First power-on checklist](#-first-power-on-checklist)
- [Troubleshooting](#-troubleshooting)
- [Serial commands](#-serial-commands)
- [Deploying the Google Sheets backend](#-deploying-the-google-sheets-backend)
- [Automated tests](#-automated-tests)
- [Project structure](#-project-structure)
- [Known limitations](#-known-limitations)
- [License](#-license)

---

## ✨ Features

- **Durable, retrying uploads (outbox).** Every accepted tap is written to
  flash and queued; a background task uploads it and removes it only after
  the backend confirms. Failures retry with backoff (2 s → 60 s). Taps made
  offline upload automatically when Wi-Fi returns, with their **original tap
  time** (NTP-stamped), not the upload time.
- **Honest sync status.** Apps Script answers HTTP 200 even for errors, so the
  firmware reads the reply *text* (`Success` / `Duplicate` / `Error: …`)
  instead of trusting the status code. A wrong API key shows "Check API key!"
  instead of a fake "Synced".
- **Never blocks on the network.** HTTPS runs on its own FreeRTOS task on
  core 0, one upload at a time; the RFID loop on core 1 stays responsive.
  Wi-Fi reconnects on its own if it drops.
- **Persistent attendance.** "Already present" flags live in NVS flash and
  survive resets and power loss. They clear themselves at midnight (once the
  clock is set) to match the backend's once-per-day rule, or by `RESET` /
  holding the button during boot.
- **Easy enrollment.** `ENROLL <Name>` then tap the card — no need to know its
  UID. Enrolled students persist in flash.
- **Self-diagnosing.** `SELFTEST` blinks every LED and beeps; `STATUS` shows
  Wi-Fi, clock, queue, reader and memory; the firmware checks the RFID reader
  over SPI at boot and every 5 s (and re-initialises it if it glitches); a
  missing OLED no longer bricks the board.
- **Same-card debounce, server-side duplicate protection**, a short-press
  attendance summary, a roster that survives reboots, and secrets kept out of
  git in `secrets.h`.

---

## 🧰 Build it on real hardware

### Shopping list

| Part | Notes |
|---|---|
| **ESP32 DevKit V1, 30-pin**, with an **ESP-WROOM-32** module | The [wiring diagrams](#-wiring) show this board. ⚠️ Must be the *classic* ESP32. **Not** ESP32-S2 / S3 / C3 / C6 boards: those are different chips and the pin numbers here don't apply. A 38-pin board ("DevKitC") also works, but its pins sit in a different order — match the printed labels, not the positions in the picture. |
| **MFRC522 RFID module (13.56 MHz)** + cards/tags | ⚠️ **Must be 13.56 MHz (MIFARE).** Many college ID cards and cheap key fobs are **125 kHz** and an MFRC522 *cannot read them*. Test your actual ID card before relying on it — if it's 125 kHz, use the cards/tags that come with the module. The header pins usually come loose in the bag: **solder them** (see wiring notes). |
| **SSD1306 OLED, 0.96", 128×64, I²C** (4-pin: GND VCC SCL SDA) | ⚠️ **SSD1306, not SH1106.** Most **1.3"** OLEDs are SH1106 and show garbage with this code. Not the 7-pin SPI version either. Address 0x3C or 0x3D — both are detected automatically. |
| **3 LEDs** (green, red, blue) + **3 × 220 Ω resistors** | The resistors are **required** — an LED wired straight to a GPIO overloads the pin. |
| **Passive buzzer** + **1 × 100 Ω resistor** | *Passive* (no built-in oscillator) so `tone()` can play two pitches. The 100 Ω resistor protects the GPIO from "magnetic" buzzers, whose coil is only ~16–42 Ω. Avoid 3-pin buzzer *modules* marked "low-level trigger": they beep non-stop whenever the pin is LOW (i.e. all the time). |
| **Push button** (4-leg tactile switch) | Any momentary button. |
| Breadboard + male-to-female and male-to-male jumpers, USB cable | Use a **data** USB cable (many are charge-only — the PC won't see the board). |
| *(optional)* 100–470 µF capacitor across 3V3/GND | Cheap insurance against brown-out resets when Wi-Fi transmits, if your board browns out on weak USB ports. |

### Software setup

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
   Your OS may need the USB-serial driver for your board's chip (CP2102 or
   CH340 — it's printed on the small chip next to the USB port).
2. `cp include/secrets.example.h include/secrets.h` and fill it in:
   - `WIFI_SSID` / `WIFI_PASSWORD` — your **2.4 GHz** network. ESP32 cannot
     join 5 GHz-only networks, and it **cannot join WPA2-Enterprise or
     captive-portal networks** (typical campus Wi-Fi). Use your **phone's
     hotspot** (set to 2.4 GHz / "maximize compatibility") for the demo.
   - `GOOGLE_SCRIPT_URL` / `API_KEY` — from [the backend
     steps](#-deploying-the-google-sheets-backend). Test the URL in a browser
     first (step 7 there) — then you know the cloud side works before the
     board is even plugged in.
3. Wire it up in stages ([below](#-bring-it-up-in-stages)).
4. Build and flash — the VS Code **→ Upload** button, or:
   ```bash
   pio run -t upload
   pio device monitor        # 115200 baud; what you type is echoed
   ```
   If upload fails to start, hold the board's **BOOT** button while it says
   `Connecting...`.
5. Walk through the [power-on checklist](#-first-power-on-checklist).

At boot the firmware checks `secrets.h`: a leftover placeholder, a URL that
doesn't end in `/exec`, or an impossible Wi-Fi password prints a `[CONFIG]`
warning and shows **CHECK secrets.h** on the OLED.

---

## 🔌 Wiring

**Everything runs from 3.3 V — never connect the MFRC522 to 5 V / VIN.** Wire
with the USB cable **unplugged**.

[![Full wiring diagram: ESP32 DevKit V1 with the RFID reader, OLED, three LEDs, buzzer and push button](docs/wiring/wiring.svg)](docs/wiring/wiring.svg)

The picture shows the 30-pin ESP32 DevKit V1 from the top, USB at the bottom.
Match each wire by the **label printed on your board** (`D5` is GPIO 5; some
boards print `IO5`, `G5` or just `5`). The long red/blue strips are the
breadboard power rails: plug the ESP32's `3V3` into the red (+) rail and `GND`
into the blue (−) rail, and everything else takes power from there. Building
it one part at a time? There's a picture per step in
[Bring it up in stages](#-bring-it-up-in-stages).

The same wiring as a table — `test/check_wiring.py` fails if it, the pictures
and `include/config.h` ever disagree:

| Module pin | → | ESP32 | Notes |
|---|---|---|---|
| **MFRC522** SDA (SS) | → | GPIO 5 | |
| MFRC522 SCK | → | GPIO 18 | |
| MFRC522 MOSI | → | GPIO 23 | |
| MFRC522 MISO | → | GPIO 19 | |
| MFRC522 RST | → | GPIO 27 | |
| MFRC522 3.3V | → | **3V3** | not 5V / VIN! |
| MFRC522 GND | → | GND | |
| MFRC522 IRQ | | *not connected* | |
| **OLED** SDA | → | GPIO 21 | |
| OLED SCL | → | GPIO 22 | |
| OLED VCC | → | **3V3** | |
| OLED GND | → | GND | |
| **Green LED** | GPIO 13 → 220 Ω → LED long leg (anode) | | LED short leg → GND |
| **Red LED** | GPIO 26 → 220 Ω → LED long leg | | short leg → GND |
| **Blue LED** | GPIO 14 → 220 Ω → LED long leg | | short leg → GND |
| **Buzzer** + | GPIO 25 → 100 Ω → buzzer + | | buzzer − → GND |
| **Button** | one leg → GPIO 4 | | **diagonally opposite** leg → GND (internal pull-up used) |

**Read before you plug in:**

- **Go by the printed labels, not pin positions.** OLED boards from different
  sellers order the pins differently — some are `VCC GND SCL SDA`, others
  `GND VCC SCL SDA`. Swapping VCC and GND can destroy the display.
- **Solder the MFRC522 header.** Pins just pushed through the holes make
  intermittent contact: the reader works, then randomly stops
  (`RFID: check wiring!` on the screen).
- **Button legs:** a 4-leg button's legs are joined in pairs inside. Use two
  *diagonally opposite* legs. Wired across a joined pair, the button reads
  "pressed" forever — the firmware detects that at boot (`BUTTON STUCK?`) and
  refuses to wipe anything.
- **LEDs:** long leg (anode) towards the resistor/GPIO. If the blue LED looks
  very dim, 100 Ω instead of 220 Ω is fine for blue.

> **Why GPIO 26 / 25 and not 12 / 15?** GPIO 12 and 15 are ESP32 *strapping*
> pins — a wrong level at power-up (an LED or buzzer can be enough) can stop
> the board booting. None of the pins above are strapping (except GPIO 5,
> the standard, safe RFID chip-select), flash or input-only pins
> (`check_wiring.py` enforces this).
>
> RFID `RST` is on GPIO 27, *not* GPIO 21 — 21 is the OLED's SDA.

---

## 🪜 Bring it up in stages

Don't wire everything and hope. The firmware runs fine with any module
missing (no OLED → it runs headless; no reader → it warns), so add one part
at a time and confirm it before the next. If something is wrong you then
know exactly which ten wires to look at.

Keep the serial monitor open (`pio device monitor`, 115200) the whole time.

**Step 1 — bare board.** Nothing wired, just USB. Upload works; the boot log
appears; `[CONFIG]` warnings (if any) say what to fix in `secrets.h`; you see
`Wi-Fi connected` and `Clock set via NTP`.

**Step 2 — RFID reader.**

[![Step 2 wiring: RFID reader](docs/wiring/step-2-rfid.svg)](docs/wiring/step-2-rfid.svg)

Check: the boot log says `MFRC522 version 0x92 (OK)` (or 0x91; clones say
0x12 / 0x88 / 0xB2 — also fine). `0x00`/`0xFF` = recheck the 7 wires. Tap a
card → `[SCAN] UID: ...`.

**Step 3 — OLED display.**

[![Step 3 wiring: OLED display](docs/wiring/step-3-oled.svg)](docs/wiring/step-3-oled.svg)

Check: the boot log says `OLED found at I2C 0x3C`; the screen shows
`SYSTEM READY`.

**Step 4 — LEDs, buzzer, button.**

[![Step 4 wiring: LEDs, buzzer and button](docs/wiring/step-4-leds-buzzer-button.svg)](docs/wiring/step-4-leds-buzzer-button.svg)

Check: `SELFTEST` → green, red, blue in turn, then two beeps. Short-press the
button → `Present: 0 / N` on the screen.

---|---|---|
| 1 | **Nothing** — bare board on USB | Upload works; boot log appears; `[CONFIG]` warnings (if any) tell you what to fix in `secrets.h`; `Wi-Fi connected` and `Clock set via NTP`. |
| 2 | + **MFRC522** | Boot log says `MFRC522 version 0x92 (OK)` (or 0x91; clones say 0x12 / 0x88 / 0xB2 — also fine). `0x00`/`0xFF` = recheck the 7 wires. Tap a card → `[SCAN] UID: ...`. |
| 3 | + **OLED** | Boot log says `OLED found at I2C 0x3C`; screen shows `SYSTEM READY`. |
| 4 | + **LEDs, buzzer, button** | `SELFTEST` → green, red, blue in turn, then two beeps. Short-press the button → `Present: 0 / N` on screen. |

---

## ✅ First power-on checklist

With everything wired, open the serial monitor (115200) and work down the
list; each step tells you whether the *next* one is worth debugging.

1. **Boot log** shows `MFRC522 version 0x.. (OK)`, `OLED found`, `Wi-Fi
   connected` and no `[CONFIG]` warnings.
2. **OLED** shows `SYSTEM READY` / `Wi-Fi: Connected`.
3. Type **`SELFTEST`** → green, red, blue LEDs light in turn, then two beeps.
   A dark LED is usually reversed (long leg must face the resistor/GPIO).
4. Type **`ENROLL Your Name`**, tap your card within 15 s → `STUDENT ADDED`.
   Note: the UID printed is what the sheet will store. Typo in the name?
   `REMOVE <UID>` and enroll again.
5. Wait ~3 s, tap it again → green LED, beep, `WELCOME!`, then `ATTENDANCE
   LOGGED`; a row appears in the Google Sheet.
6. Tap it a third time → red LED, `ALREADY LOGGED`.
7. Tap an un-enrolled card → red LED, `ACCESS DENIED` and its UID in the log.
8. **Offline test:** turn off the hotspot, tap another enrolled card →
   `Saved (offline)`, the ready screen shows `Unsynced: 1`. Turn the hotspot
   back on → within ~a minute it uploads by itself, and the sheet gets the
   *original* tap time.
9. Unplug and replug power → `LIST` still shows your students and who's
   present.

---

## 🛠 Troubleshooting

| Symptom | Likely cause |
|---|---|
| PC doesn't see the board / no COM port | Charge-only USB cable, or the CP2102/CH340 driver isn't installed. |
| `MFRC522 version 0x00` / `0xFF`, or `RFID: check wiring!` on the OLED | Loose or swapped SPI jumper, unsoldered header, or the module isn't on 3.3 V. Breadboard jumpers are the usual culprit. |
| Reader works but your ID card is never detected | The card is 125 kHz (not MIFARE). Use the tags shipped with the MFRC522. |
| `OLED not found at I2C 0x3C or 0x3D` | SDA/SCL swapped, VCC/GND not connected, or a loose jumper. |
| OLED shows random dots / a shifted picture | It's an **SH1106** (most 1.3" displays), not an SSD1306. |
| `CHECK secrets.h` on the OLED | A `[CONFIG]` line in the serial log says exactly what is still a placeholder. |
| Board resets when Wi-Fi connects | Weak USB port/cable (brown-out). Try another port/cable, add the capacitor, or a powered hub. |
| `Wi-Fi: Offline` forever | 5 GHz or enterprise/captive-portal network, wrong password, or SSID typo (it's case-sensitive). Use a 2.4 GHz phone hotspot. |
| `SAVED LOCALLY … Check API key!` | `API_KEY` in `secrets.h` ≠ the one in the Apps Script, or you edited the script and didn't redeploy a **new version**. |
| Log says `HTTP -1` / `-29312`, or `Err: 302/401` with an HTML reply | `-1`/`-29312`: TLS/connect failure, retried automatically — check internet access. HTML reply: the URL needs a Google login — make sure it's the `/exec` URL with access **Anyone**, deployed from a personal Gmail account (college Workspace accounts often can't share with "Anyone"). |
| Sheet timestamps or "today" off by hours | Set the Sheet's time zone (**File → Settings → Time zone**), and `TZ_INFO` in `config.h` for the device's day rollover. |
| `BUTTON STUCK?` at boot | Button wired across two legs that are joined inside — use diagonally opposite legs. |
| Everyone shows `ALREADY LOGGED` | Only possible for taps made since power-on; earlier days clear themselves once the clock is set. `RESET` wipes today's flags. |
| `Unsynced: N` won't go down | Run `STATUS` and read the `[SYNC]` log lines: they show the backend's actual reply. |

To start completely fresh (roster, attendance and upload queue all erased):
`pio run -t erase`, then upload again.

---

## ⌨️ Serial commands

| Command | Does |
|---|---|
| `ENROLL <Name>` | Next card tapped (within 15 s) is enrolled under that name |
| `ADD <UID> <Name>` | Enroll by typing the UID, e.g. `ADD 3F2A9B10 Priya Sharma` (8 or 14 hex digits; `3F:2A:9B:10` also accepted) |
| `REMOVE <UID>` | Un-enroll a student (to fix a typo: `REMOVE`, then `ENROLL` again) |
| `LIST` | Roster and who's present |
| `STATUS` | Wi-Fi, clock, unsynced count, reader version, OLED, free heap |
| `SELFTEST` | Blink LEDs, beep, check reader and OLED |
| `RESET` | Wipe today's attendance (enrolled students stay) |
| `HELP` | List commands |

The reset **button**: a short press shows "Present: X / Y"; holding it while
powering on (≥ 3 s, then release when the screen says so) wipes today's
attendance — unsynced uploads are kept.

---

## ☁️ Deploying the Google Sheets backend

Use a **personal Gmail account** — college Google Workspace accounts often
aren't allowed to share a web app with "Anyone", and the board then gets a
login page instead of an answer.

1. Create a new Google Sheet. Check **File → Settings → Time zone** is your
   local zone: it decides what "today" means for duplicate taps.
2. **Extensions → Apps Script**, delete the boilerplate, paste in
   `backend/AppsScript.gs`.
3. Generate a key (`openssl rand -hex 16`) and set it in **both**
   `API_KEY` at the top of the script *and* `API_KEY` in `include/secrets.h`.
   The script ships with a placeholder and refuses to run until you change it.
   Don't commit your real key. Stick to letters and digits: the board
   encodes any key correctly, but a `+`, `/`, `&` or `#` pasted into the
   browser test in step 7 gets mangled and looks like a wrong key.
4. **Deploy → New deployment → Web app** — Execute as **Me**, access
   **Anyone**.
5. Google asks you to **authorize** the script: pick your account →
   *"Google hasn't verified this app"* → **Advanced** → **Go to … (unsafe)** →
   **Allow**. (It's your own script, asking for access to your own sheet.)
6. Copy the `.../exec` URL into `GOOGLE_SCRIPT_URL` in `secrets.h`.
7. **Test it from a browser** before touching hardware — open
   `<your /exec URL>?uid=TEST0001&name=Browser%20Test&key=<your key>`.
   It should say `Success: Attendance logged for Browser Test` and a row
   appears in the sheet (delete it afterwards). `Error: Invalid or missing API
   key` means the keys don't match; a Google sign-in page means the access
   isn't **Anyone**.
8. After any later edit: **Deploy → Manage deployments → ✏️ → New version**.

---

## 🤖 Automated tests

```bash
./test/run_all.sh        # everything below
```

| Suite | What it proves |
|---|---|
| `test/check_wiring.py` | `config.h` ↔ the README wiring table agree; no pin reuse; no flash/strapping/UART pins; every LED and the buzzer has a resistor; modules on 3V3 |
| `tools/wiring_diagram.py --check` | The wiring pictures in `docs/wiring/` were drawn from the current `config.h` |
| `node --test test/backend/appsscript.test.js` | The real `AppsScript.gs` against a fake sheet: auth, duplicates (incl. numeric UIDs), late-upload timestamps, the sheet's time zone, bad cells, lock contention |
| `pio test -e native` | Roster/attendance persistence, stale flags after power-on, enroll/remove, UID validation, outbox (FIFO, reboot, overflow, corrupt flash), reply classification, day rollover, 7-byte UIDs |
| `pio run -e hardware` | The firmware compiles warning-free |

GitHub Actions runs the same script on every push. What these **cannot**
cover is physical behaviour (RFID read range, buzzer pitch, your Wi-Fi) —
that's what the staged bring-up and the checklist are for. See
[`test/README`](test/README) for the full list of hardware acceptance checks.

---

## 📁 Project structure

```
├── backend/AppsScript.gs        # Google Sheets Web App
├── docs/ARCHITECTURE.md         # Data flow + design decisions
├── docs/wiring/                 # Wiring diagrams (generated from config.h)
├── include/                     # config.h (pins/timing), headers, secrets
├── src/
│   ├── main.cpp                 # Orchestration, serial console, upload pump
│   ├── StudentDatabase.*        # Roster + attendance in NVS
│   ├── Outbox.*                 # Persistent upload queue
│   ├── AttendanceLogger.*       # Wi-Fi, NTP, HTTPS upload task
│   ├── BackendResponse.*        # What an Apps Script reply means
│   ├── DisplayUI.*  FeedbackIndicators.*
├── test/                        # run_all.sh, native/, backend/, mocks/, check_wiring.py
├── tools/wiring_diagram.py      # Redraws docs/wiring/ after a pin change
└── platformio.ini               # envs: hardware (default, the ESP32), native (tests)
```

---

## ⚠️ Known limitations

- **No TLS certificate validation** (`setInsecure()`): the request is
  encrypted but the server isn't verified. The payload is a UID and a name,
  protected by the shared `API_KEY`. See `docs/ARCHITECTURE.md`.
- **API key travels in the URL** (Apps Script can't read custom headers). It
  is a spam guard, not strong authentication.
- **Roster is capped** at `MAX_STUDENTS` (20) and lives in the device.
- **The device has no battery clock.** Until it has been online once after
  power-on, taps are stamped by the backend when they upload, and "present"
  flags from before the power-on aren't counted (so a re-tap is accepted and
  the backend's duplicate check drops it).
- **One record per student per day** — both device and backend deduplicate by
  calendar day, so multiple classes on the same day need a different scheme.
- **Cards are identified by UID only**, which is trivially cloneable. Fine
  for a classroom; not for access control.
- **What's verified automatically:** compilation, unit logic, backend
  behaviour and wiring/pin safety — not a physical board. Use the staged
  bring-up and the checklist.

## 📄 License

MIT — see [`LICENSE`](LICENSE).

---
*Developed for the 3rd-semester IoT & Robotics Lab.*
