#!/usr/bin/env python3
"""Checks include/config.h against the wiring table in README.md.

    python3 test/check_wiring.py

The README table is what you wire from, so this keeps it and the firmware
in lockstep. Fails (exit 1) if:
  * a pin in config.h is missing from, or different in, the README table
  * the same GPIO is used by two signals
  * a signal uses a pin that is unsafe on a real ESP32 (flash pins,
    strapping pins, UART0, input-only pins used as outputs)
  * an LED or the buzzer is listed without its series resistor
  * the OLED or RFID reader is not fed from 3V3
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
errors = []


def err(msg):
    errors.append(msg)


# ---- config.h ---------------------------------------------------------
cfg = {}
for m in re.finditer(r"^\s*#define\s+(PIN_\w+)\s+(\d+)", (ROOT / "include/config.h").read_text(), re.M):
    cfg[m.group(1)] = int(m.group(2))

# config name -> (README row label, is_output, text the row must also contain)
EXPECT = {
    "PIN_RFID_SS": ("MFRC522 SDA", True, None),
    "PIN_RFID_SCK": ("MFRC522 SCK", True, None),
    "PIN_RFID_MOSI": ("MFRC522 MOSI", True, None),
    "PIN_RFID_MISO": ("MFRC522 MISO", False, None),
    "PIN_RFID_RST": ("MFRC522 RST", True, None),
    "PIN_OLED_SDA": ("OLED SDA", True, None),
    "PIN_OLED_SCL": ("OLED SCL", True, None),
    "PIN_LED_GREEN": ("Green LED", True, "220 Ω"),
    "PIN_LED_RED": ("Red LED", True, "220 Ω"),
    "PIN_LED_BLUE": ("Blue LED", True, "220 Ω"),
    "PIN_BUZZER": ("Buzzer", True, "100 Ω"),  # a magnetic buzzer coil is only ~16-42 ohm
    "PIN_BUTTON_RESET": ("Button", False, None),
}
for name in EXPECT:
    if name not in cfg:
        err(f"config.h is missing {name}")
for name in cfg:
    if name not in EXPECT:
        err(f"{name} in config.h has no wiring expectation in check_wiring.py - add one")

# ---- README wiring table ----------------------------------------------
readme = (ROOT / "README.md").read_text()
section = readme.split("## 🔌 Wiring", 1)[-1].split("\n## ", 1)[0]
rows = [re.sub(r"\*\*", "", line) for line in section.splitlines() if line.startswith("|")]


def row(label):
    hits = [r for r in rows if r.startswith(f"| {label}")]
    return hits[0] if len(hits) == 1 else None


for name, (label, _, must_contain) in EXPECT.items():
    if name not in cfg:
        continue
    r = row(label)
    if r is None:
        err(f"README wiring table has no single row for '{label}' ({name})")
        continue
    gpios = sorted(set(int(g) for g in re.findall(r"GPIO\s*(\d+)", r)))
    if gpios != [cfg[name]]:
        err(f"{name}: config.h says GPIO {cfg[name]} but the README row '{label}' says GPIO {gpios or 'nothing'}")
    if must_contain and must_contain not in r:
        err(f"README row '{label}' must include its {must_contain} series resistor")

for label in ("MFRC522 3.3V", "OLED VCC"):
    r = row(label)
    if r is None or "3V3" not in r:
        err(f"README row '{label}' must go to 3V3 (the MFRC522 must never see 5V; a 5V OLED pulls I2C to 5V)")
    elif re.search(r"→\s*\|?\s*(5V|VIN)\b", r):
        err(f"README row '{label}' connects to 5V/VIN")
for label in ("MFRC522 GND", "OLED GND"):
    r = row(label)
    if r is None or "GND" not in r.split("|", 2)[2]:
        err(f"README row '{label}' must go to GND")

# ---- pin safety -------------------------------------------------------
FLASH = set(range(6, 12))          # connected to the SPI flash
UART0 = {1, 3}                     # USB serial / programming
STRAPPING = {0, 2, 12, 15}         # boot-mode / voltage straps
INPUT_ONLY = {34, 35, 36, 39}
# GPIO5 is a strapping pin too, but it is the conventional, safe MFRC522 CS
# (the module's SDA pin is an input, so it can't pull GPIO5 at boot).
GPIO5_ALLOWED = "PIN_RFID_SS"

used = {}
for name, gpio in cfg.items():
    if name not in EXPECT:
        continue
    is_output = EXPECT[name][1]
    if gpio in used:
        err(f"GPIO {gpio} is used by both {used[gpio]} and {name}")
    used[gpio] = name
    if gpio in FLASH:
        err(f"{name} uses GPIO {gpio}, which is wired to the module's flash chip")
    if gpio in UART0:
        err(f"{name} uses GPIO {gpio}, which is the USB serial/programming UART")
    if gpio in STRAPPING:
        err(f"{name} uses GPIO {gpio}, a boot strapping pin - a wrong level at power-up can stop the board booting")
    if gpio == 5 and name != GPIO5_ALLOWED:
        err(f"{name} uses GPIO 5 (strapping pin) - only the RFID chip-select is allowed there")
    if gpio in INPUT_ONLY and (is_output or name == "PIN_BUTTON_RESET"):
        err(f"{name} uses input-only GPIO {gpio} (no output driver and no internal pull-up)")
    if gpio > 39 or gpio < 0:
        err(f"{name} uses non-existent GPIO {gpio}")

# ---- report -----------------------------------------------------------
if errors:
    print(f"WIRING CHECK FAILED ({len(errors)} problem(s)):")
    for e in errors:
        print("  - " + e)
    sys.exit(1)
print(f"Wiring check passed: {len(used)} GPIOs, config.h and the README wiring table agree, "
      f"no unsafe pins, LEDs and buzzer have series resistors, modules on 3V3.")
