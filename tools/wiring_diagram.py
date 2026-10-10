#!/usr/bin/env python3
"""Draws the wiring diagrams in docs/wiring/ from the pins in include/config.h.

    python3 tools/wiring_diagram.py           # (re)write the SVGs
    python3 tools/wiring_diagram.py --check   # exit 1 if they are out of date

The board drawn is the common 30-pin "ESP32 DevKit V1" (DOIT layout), seen
from the top with the USB socket at the bottom. Every GPIO comes from
config.h, so the pictures can't disagree with the firmware; --check (run by
test/run_all.sh) fails if someone changes a pin without regenerating them.

Output: wiring.svg (everything) plus one picture per bring-up stage in the
README (step-2-rfid.svg, step-3-oled.svg, step-4-leds-buzzer-button.svg).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "wiring"

# ---------------------------------------------------------------------------
# The board: DOIT ESP32 DevKit V1, 30 pins, top view, USB at the bottom.
# ---------------------------------------------------------------------------
LEFT = ["EN", "VP", "VN", "D34", "D35", "D32", "D33", "D25", "D26", "D27", "D14", "D12", "D13", "GND", "VIN"]
RIGHT = ["D23", "D22", "TX0", "RX0", "D21", "D19", "D18", "D5", "TX2", "RX2", "D4", "D2", "D15", "GND", "3V3"]
SPECIAL_GPIO = {"VP": 36, "VN": 39, "TX0": 1, "RX0": 3, "TX2": 17, "RX2": 16}


def label_gpio(label):
    if label in SPECIAL_GPIO:
        return SPECIAL_GPIO[label]
    if re.fullmatch(r"D\d+", label):
        return int(label[1:])
    return None


W, H = 1500, 1090
BG = "#fbfbfd"
BX, BY, BW, BH = 560, 270, 240, 650          # board outline
PIN_XL, PIN_XR = BX + 18, BX + BW - 18        # pin centres, left / right column
PIN_Y0, PITCH = BY + 85, 36

RAIL_TOP_3V3, RAIL_TOP_GND = 52, 70
RAIL_BOT_3V3, RAIL_BOT_GND = 1032, 1050
RAIL_X0, RAIL_X1 = 40, 1460
LINK_3V3_X, LINK_GND_X = 1440, 1455           # bridges joining top and bottom rails
TITLE_X, TITLE_Y = 1190, 420                  # free space right of the wires

C = {  # wire colours
    "3v3": "#d62828", "gnd": "#1f1f1f",
    "ss": "#00a6a6", "sck": "#f08c00", "mosi": "#8338ec", "miso": "#3a86ff", "rst": "#ff006e",
    "scl": "#2b9348", "sda": "#b5179e",
    "buzzer": "#9c6644", "led_red": "#e5383b", "led_blue": "#0096c7", "led_green": "#38b000",
    "button": "#5c677d",
}


def esp_pin(gpio=None, label=None, side=None):
    """(side, x, y, label) of a board pin, by GPIO number or by label."""
    for s, names, x in (("L", LEFT, PIN_XL), ("R", RIGHT, PIN_XR)):
        if side and s != side:
            continue
        for i, name in enumerate(names):
            if (gpio is not None and label_gpio(name) == gpio) or (label is not None and name == label):
                return s, x, PIN_Y0 + i * PITCH, name
    raise SystemExit(f"wiring_diagram: GPIO {gpio} / {label} is not a pin on the 30-pin DevKit V1")


def read_config():
    text = (ROOT / "include" / "config.h").read_text()
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"^\s*#define\s+(PIN_\w+)\s+(\d+)", text, re.M)}


# ---------------------------------------------------------------------------
# SVG helpers
# ---------------------------------------------------------------------------
FONT = "font-family=\"Helvetica, Arial, sans-serif\""
MONO = "font-family=\"Menlo, Consolas, 'DejaVu Sans Mono', monospace\""


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=12, fill="#222", anchor="start", weight="normal", mono=False, extra=""):
    fam = MONO if mono else FONT
    return (f'<text x="{x}" y="{y}" {fam} font-size="{size}" fill="{fill}" '
            f'text-anchor="{anchor}" font-weight="{weight}" {extra}>{esc(s)}</text>')


def rounded_path(pts, r=10):
    """SVG path through `pts` with rounded corners."""
    d = [f"M{pts[0][0]},{pts[0][1]}"]
    for i in range(1, len(pts) - 1):
        (x0, y0), (x1, y1), (x2, y2) = pts[i - 1], pts[i], pts[i + 1]
        l1 = max(abs(x1 - x0), abs(y1 - y0))
        l2 = max(abs(x2 - x1), abs(y2 - y1))
        rr = min(r, l1 / 2, l2 / 2)
        ax = x1 - (rr if x1 > x0 else -rr if x1 < x0 else 0)
        ay = y1 - (rr if y1 > y0 else -rr if y1 < y0 else 0)
        bx = x1 + (rr if x2 > x1 else -rr if x2 < x1 else 0)
        by = y1 + (rr if y2 > y1 else -rr if y2 < y1 else 0)
        d.append(f"L{ax},{ay} Q{x1},{y1} {bx},{by}")
    d.append(f"L{pts[-1][0]},{pts[-1][1]}")
    return " ".join(d)


class Drawing:
    def __init__(self):
        self.under, self.wires, self.over = [], [], []

    def wire(self, pts, color, width=4.2):
        d = rounded_path(pts)
        # The light halo underneath makes every crossing read as "this wire
        # passes over that one", never as a junction.
        self.wires.append(f'<path d="{d}" fill="none" stroke="{BG}" stroke-width="{width + 5}" '
                          f'stroke-linecap="round" stroke-linejoin="round"/>')
        self.wires.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}" '
                          f'stroke-linecap="round" stroke-linejoin="round"/>')

    def dot(self, x, y, color, r=4.5):
        self.over.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{color}" stroke="{BG}" stroke-width="1.5"/>')


# ---------------------------------------------------------------------------
# Parts
# ---------------------------------------------------------------------------
def draw_rails(dw):
    for y3, yg, label_y in ((RAIL_TOP_3V3, RAIL_TOP_GND, 30), (RAIL_BOT_3V3, RAIL_BOT_GND, 1080)):
        dw.under.append(f'<rect x="{RAIL_X0 - 14}" y="{y3 - 14}" width="{RAIL_X1 - RAIL_X0 + 28}" height="{yg - y3 + 28}" '
                        f'rx="6" fill="#efe9dc" stroke="#d6cdb8"/>')
        dw.under.append(f'<line x1="{RAIL_X0}" y1="{y3}" x2="{RAIL_X1}" y2="{y3}" stroke="{C["3v3"]}" stroke-width="3"/>')
        dw.under.append(f'<line x1="{RAIL_X0}" y1="{yg}" x2="{RAIL_X1}" y2="{yg}" stroke="#1d4ed8" stroke-width="3"/>')
        dw.under.append(text(RAIL_X0 - 6, y3 + 4, "+", 13, C["3v3"], "end", "bold"))
        dw.under.append(text(RAIL_X0 - 6, yg + 4, "−", 15, "#1d4ed8", "end", "bold"))
    dw.under.append(text(RAIL_X0, 26, "Breadboard power rails:  + = 3V3 (red)   − = GND (blue)", 14, "#6b5e45", weight="bold"))
    # The top and bottom rail pairs are joined on the right.
    dw.wire([(LINK_3V3_X, RAIL_TOP_3V3), (LINK_3V3_X, RAIL_BOT_3V3)], C["3v3"], 3.2)
    dw.wire([(LINK_GND_X, RAIL_TOP_GND), (LINK_GND_X, RAIL_BOT_GND)], C["gnd"], 3.2)
    for x, y, c in ((LINK_3V3_X, RAIL_TOP_3V3, C["3v3"]), (LINK_3V3_X, RAIL_BOT_3V3, C["3v3"]),
                    (LINK_GND_X, RAIL_TOP_GND, C["gnd"]), (LINK_GND_X, RAIL_BOT_GND, C["gnd"])):
        dw.dot(x, y, c)
    dw.over.append(text(LINK_3V3_X - 10, 560, "join the top and bottom rails", 11, "#6b5e45", "middle",
                        extra=f'transform="rotate(-90 {LINK_3V3_X - 10} 560)"'))


def draw_esp32(dw, used):
    """`used` maps pin label -> wire colour for the pins this picture uses."""
    s = dw.under
    s.append(f'<rect x="{BX}" y="{BY}" width="{BW}" height="{BH}" rx="10" fill="#2b2d42" stroke="#111" stroke-width="2"/>')
    mx, mw = BX + 66, BW - 132
    s.append(f'<rect x="{mx}" y="{BY + 10}" width="{mw}" height="250" rx="3" fill="#1a1a1a"/>')
    zz = " ".join(f"{mx + 12 + i * 13},{BY + 22 + (i % 2) * 24}" for i in range(9))
    s.append(f'<polyline points="{zz}" fill="none" stroke="#c9a24a" stroke-width="3"/>')
    s.append(f'<rect x="{mx + 6}" y="{BY + 62}" width="{mw - 12}" height="190" rx="2" fill="#c7c9cc" stroke="#8a8d91"/>')
    s.append(text(BX + BW / 2, BY + 150, "ESP-WROOM-32", 10, "#333", "middle", "bold"))
    s.append(text(BX + BW / 2, BY + 168, "Wi-Fi + BT", 10, "#555", "middle"))
    s.append(text(BX + BW / 2, BY + 330, "ESP32", 16, "#e8e8e8", "middle", "bold"))
    s.append(text(BX + BW / 2, BY + 350, "DEVKIT V1", 12, "#c8c8c8", "middle"))
    for bx, lab in ((BX + 48, "EN"), (BX + BW - 68, "BOOT")):
        s.append(f'<rect x="{bx}" y="{BY + BH - 42}" width="20" height="14" rx="2" fill="#555" stroke="#222"/>')
        s.append(text(bx + 10, BY + BH - 16, lab, 10, "#bbb", "middle"))
    s.append(f'<rect x="{BX + BW / 2 - 28}" y="{BY + BH - 20}" width="56" height="34" rx="4" fill="#b0b3b8" stroke="#6b6e73"/>')
    s.append(text(BX + BW / 2, BY + BH + 30, "USB", 11, "#777", "middle"))
    for side, names, x in (("L", LEFT, PIN_XL), ("R", RIGHT, PIN_XR)):
        for i, name in enumerate(names):
            y = PIN_Y0 + i * PITCH
            color = used.get((side, name))
            if color:
                dw.over.append(f'<circle cx="{x}" cy="{y}" r="8" fill="#d4af37" stroke="{color}" stroke-width="3.5"/>')
            else:
                dw.over.append(f'<circle cx="{x}" cy="{y}" r="6" fill="#8d7a3c" stroke="#5c4f22" stroke-width="1"/>')
            tx, anchor = (x + 16, "start") if side == "L" else (x - 16, "end")
            dw.over.append(text(tx, y + 5, name, 15 if color else 12, "#ffffff" if color else "#8f95a3",
                                anchor, "bold" if color else "normal", mono=True))
    s.append(text(BX + BW / 2, BY + BH + 50, "ESP32 DevKit V1 · 30-pin · top view", 13, "#444", "middle", "bold"))


def draw_rfid(dw, pins):
    """MFRC522 with its 8-pin header on the bottom edge; pins = {label: (x, y)}."""
    xs = [p[0] for p in pins.values()]
    x0, x1, y1 = min(xs) - 34, max(xs) + 34, list(pins.values())[0][1]
    y0 = y1 - 190
    s = dw.under
    s.append(f'<rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0 + 6}" rx="8" fill="#1f5fae" stroke="#123c70" stroke-width="2"/>')
    for k, inset in enumerate((14, 26, 38)):
        s.append(f'<rect x="{x0 + inset}" y="{y0 + inset}" width="{x1 - x0 - 2 * inset}" height="{y1 - y0 - 80 - 2 * inset + 40}" '
                 f'rx="{14 - k * 3}" fill="none" stroke="#d9b45a" stroke-width="2"/>')
    s.append(text((x0 + x1) / 2, y0 + 72, "RFID-RC522", 15, "#ffffff", "middle", "bold"))
    s.append(text((x0 + x1) / 2, y0 + 90, "13.56 MHz", 11, "#cfe0f5", "middle"))
    for lab, (x, y) in pins.items():
        dw.over.append(f'<rect x="{x - 5}" y="{y - 5}" width="10" height="10" fill="#d4af37" stroke="#7a6320"/>')
        dw.over.append(text(x + 5, y - 12, lab, 13, "#ffffff", "start", "bold", True,
                            f'transform="rotate(-90 {x + 4} {y - 12})"'))


def draw_oled(dw, pins):
    """SSD1306 with its 4-pin header on the top edge; pins = {label: (x, y)}."""
    xs = [p[0] for p in pins.values()]
    y0 = list(pins.values())[0][1]
    x0, x1, y1 = min(xs) - 40, max(xs) + 40, y0 + 150
    s = dw.under
    s.append(f'<rect x="{x0}" y="{y0 - 6}" width="{x1 - x0}" height="{y1 - y0 + 6}" rx="8" fill="#10305a" stroke="#081a33" stroke-width="2"/>')
    s.append(f'<rect x="{x0 + 14}" y="{y0 + 32}" width="{x1 - x0 - 28}" height="86" rx="3" fill="#050505"/>')
    s.append(text(x0 + 24, y0 + 52, "SYSTEM READY", 12, "#7fdbff", mono=True, weight="bold"))
    s.append(f'<line x1="{x0 + 24}" y1="{y0 + 58}" x2="{x1 - 24}" y2="{y0 + 58}" stroke="#7fdbff"/>')
    s.append(text(x0 + 24, y0 + 78, "Wi-Fi: Connected", 11, "#7fdbff", mono=True))
    s.append(text(x0 + 24, y0 + 98, "Tap your ID Card", 11, "#7fdbff", mono=True))
    s.append(text((x0 + x1) / 2, y1 - 12, "SSD1306 OLED 0.96\" I²C", 11, "#cfe0f5", "middle"))
    for lab, (x, y) in pins.items():
        dw.over.append(f'<rect x="{x - 5}" y="{y - 5}" width="10" height="10" fill="#d4af37" stroke="#7a6320"/>')
        dw.over.append(text(x, y + 23, lab, 13, "#ffffff", "middle", "bold", True))


def draw_resistor(dw, xa, xb, y, bands, label):
    """Horizontal resistor between xa and xb (left, right lead ends)."""
    bx0, bx1 = xa + 14, xb - 14
    dw.wires.append(f'<line x1="{xa}" y1="{y}" x2="{xb}" y2="{y}" stroke="#9aa0a6" stroke-width="3"/>')
    dw.over.append(f'<rect x="{bx0}" y="{y - 9}" width="{bx1 - bx0}" height="18" rx="8" fill="#e9d3a7" stroke="#b39b6b"/>')
    for i, col in enumerate(bands):
        dw.over.append(f'<rect x="{bx0 + 9 + i * 9}" y="{y - 9}" width="5" height="18" fill="{col}"/>')
    dw.over.append(text((bx0 + bx1) / 2, y - 15, label, 14, "#333", "middle", "bold"))


BANDS_220 = ["#c62828", "#c62828", "#6d4c41", "#c9a227"]  # red red brown gold
BANDS_100 = ["#6d4c41", "#111111", "#6d4c41", "#c9a227"]  # brown black brown gold


def draw_led(dw, cx, y, color, name):
    """LED standing on the wire at height y: short leg (-) left, long leg (+) right."""
    o = dw.over
    o.append(f'<line x1="{cx - 7}" y1="{y - 20}" x2="{cx - 7}" y2="{y}" stroke="#9aa0a6" stroke-width="3"/>')
    o.append(f'<line x1="{cx + 7}" y1="{y - 20}" x2="{cx + 7}" y2="{y}" stroke="#9aa0a6" stroke-width="3"/>')
    o.append(f'<rect x="{cx - 17}" y="{y - 26}" width="34" height="6" rx="2" fill="{color}" opacity="0.85"/>')
    o.append(f'<path d="M{cx - 14},{y - 22} L{cx - 14},{y - 44} A14,14 0 0 1 {cx + 14},{y - 44} L{cx + 14},{y - 22} Z" '
             f'fill="{color}" stroke="#00000040"/>')
    o.append(f'<ellipse cx="{cx - 5}" cy="{y - 46}" rx="4" ry="7" fill="#ffffff" opacity="0.45"/>')
    o.append(text(cx - 18, y - 5, "−", 15, "#444", "middle", "bold"))
    o.append(text(cx + 18, y - 5, "+", 15, "#444", "middle", "bold"))
    o.append(text(cx - 26, y - 34, name, 15, "#222", "end", "bold"))


def draw_buzzer(dw, cx, y, name):
    o = dw.over
    o.append(f'<line x1="{cx - 8}" y1="{y - 16}" x2="{cx - 8}" y2="{y}" stroke="#9aa0a6" stroke-width="3"/>')
    o.append(f'<line x1="{cx + 8}" y1="{y - 16}" x2="{cx + 8}" y2="{y}" stroke="#9aa0a6" stroke-width="3"/>')
    o.append(f'<circle cx="{cx}" cy="{y - 40}" r="24" fill="#1c1c1c" stroke="#000"/>')
    o.append(f'<circle cx="{cx}" cy="{y - 40}" r="5" fill="#444"/>')
    o.append(text(cx + 13, y - 50, "+", 12, "#ddd", "middle", "bold"))
    o.append(text(cx - 19, y - 5, "−", 15, "#444", "middle", "bold"))
    o.append(text(cx + 19, y - 5, "+", 15, "#444", "middle", "bold"))
    o.append(text(cx - 32, y - 36, name, 15, "#222", "end", "bold"))


def draw_button(dw, cx, cy):
    """4-leg tactile switch, top view. Returns (top-left leg, bottom-right leg)."""
    o = dw.over
    legs = {"tl": (cx - 30, cy - 15), "bl": (cx - 30, cy + 15), "tr": (cx + 30, cy - 15), "br": (cx + 30, cy + 15)}
    for (lx, ly) in legs.values():
        o.append(f'<line x1="{cx - 20 if lx < cx else cx + 20}" y1="{ly}" x2="{lx}" y2="{ly}" stroke="#9aa0a6" stroke-width="4"/>')
    o.append(f'<rect x="{cx - 22}" y="{cy - 22}" width="44" height="44" rx="4" fill="#3d3d3d" stroke="#1d1d1d"/>')
    o.append(f'<circle cx="{cx}" cy="{cy}" r="12" fill="#111" stroke="#555"/>')
    o.append(text(cx, cy - 32, "Push button", 15, "#222", "middle", "bold"))
    o.append(text(cx, cy + 46, "use diagonal legs", 13, "#555", "middle"))
    return legs["tl"], legs["br"]


# ---------------------------------------------------------------------------
# The picture
# ---------------------------------------------------------------------------
def build(cfg, groups, title, subtitle):
    dw = Drawing()
    used = {}

    def pin(name):
        side, x, y, label = esp_pin(cfg[name])
        return side, x, y, label

    def expect_side(name, side):
        if pin(name)[0] != side:
            raise SystemExit(f"wiring_diagram: the layout expects {name} on the "
                             f"{'left' if side == 'L' else 'right'} column - update tools/wiring_diagram.py")

    for n in ("PIN_RFID_SS", "PIN_RFID_SCK", "PIN_RFID_MOSI", "PIN_RFID_MISO", "PIN_OLED_SDA", "PIN_OLED_SCL",
              "PIN_BUTTON_RESET"):
        expect_side(n, "R")
    for n in ("PIN_RFID_RST", "PIN_LED_GREEN", "PIN_LED_RED", "PIN_LED_BLUE", "PIN_BUZZER"):
        expect_side(n, "L")

    _, gx, gy, _ = esp_pin(label="GND", side="R")
    _, vx, vy, _ = esp_pin(label="3V3", side="R")

    draw_rails(dw)
    # Board power always goes to the bottom rails.
    dw.wire([(gx, gy), (gx + 38, gy), (gx + 38, RAIL_BOT_GND)], C["gnd"])
    dw.wire([(vx, vy), (vx + 20, vy), (vx + 20, RAIL_BOT_3V3)], C["3v3"])
    dw.dot(gx + 38, RAIL_BOT_GND, C["gnd"])
    dw.dot(vx + 20, RAIL_BOT_3V3, C["3v3"])
    used[("R", "GND")] = C["gnd"]
    used[("R", "3V3")] = C["3v3"]

    # ---- RFID reader: top right, header facing down -----------------------
    if "rfid" in groups:
        order = ["3.3V", "RST", "GND", "IRQ", "MISO", "MOSI", "SCK", "SDA"]
        hy, hx0, hp = 300, 930, 30
        rp = {lab: (hx0 + i * hp, hy) for i, lab in enumerate(order)}
        draw_rfid(dw, rp)
        # SPI: straight out of the board, then up into the header.
        for name, lab, col in (("PIN_RFID_MOSI", "MOSI", "mosi"), ("PIN_RFID_MISO", "MISO", "miso"),
                               ("PIN_RFID_SCK", "SCK", "sck"), ("PIN_RFID_SS", "SDA", "ss")):
            side, x, y, plabel = pin(name)
            px, py = rp[lab]
            dw.wire([(x, y), (px, y), (px, py)], C[col])
            used[(side, plabel)] = C[col]
        # Power from the top rails, RST from the left column over the top of
        # the board; they reach the header from the left, below the module.
        band = {"3.3V": hy + 16, "RST": hy + 28, "GND": hy + 40}
        lanes = {"GND": 832, "RST": 847, "3.3V": 862}
        dw.wire([(lanes["3.3V"], RAIL_TOP_3V3), (lanes["3.3V"], band["3.3V"]), (rp["3.3V"][0], band["3.3V"]), rp["3.3V"]], C["3v3"])
        dw.wire([(lanes["GND"], RAIL_TOP_GND), (lanes["GND"], band["GND"]), (rp["GND"][0], band["GND"]), rp["GND"]], C["gnd"])
        dw.dot(lanes["3.3V"], RAIL_TOP_3V3, C["3v3"])
        dw.dot(lanes["GND"], RAIL_TOP_GND, C["gnd"])
        side, x, y, plabel = pin("PIN_RFID_RST")
        over_y = BY - 22
        dw.wire([(x, y), (BX - 22, y), (BX - 22, over_y), (lanes["RST"], over_y), (lanes["RST"], band["RST"]),
                 (rp["RST"][0], band["RST"]), rp["RST"]], C["rst"])
        used[(side, plabel)] = C["rst"]

    # ---- OLED: top left, header facing up ---------------------------------
    if "oled" in groups:
        order = ["GND", "VCC", "SCL", "SDA"]
        oy, ox0, op = 150, 250, 36
        orp = {lab: (ox0 + i * op, oy) for i, lab in enumerate(order)}
        draw_oled(dw, orp)
        dw.wire([(orp["GND"][0], RAIL_TOP_GND), orp["GND"]], C["gnd"])
        dw.wire([(orp["VCC"][0], RAIL_TOP_3V3), orp["VCC"]], C["3v3"])
        dw.dot(orp["GND"][0], RAIL_TOP_GND, C["gnd"])
        dw.dot(orp["VCC"][0], RAIL_TOP_3V3, C["3v3"])
        # I2C: out of the right column, up past the board, left over the top.
        for name, lab, col, lane, top in (("PIN_OLED_SCL", "SCL", "scl", 812, 112), ("PIN_OLED_SDA", "SDA", "sda", 822, 100)):
            side, x, y, plabel = pin(name)
            px, py = orp[lab]
            dw.wire([(x, y), (lane, y), (lane, top), (px, top), (px, py)], C[col])
            used[(side, plabel)] = C[col]

    # ---- LEDs + buzzer: left, each on its own row -------------------------
    if "leds" in groups:
        bus_x, part_x, res = 60, 200, (290, 380)
        rows = []
        for name, kind, col, label in (("PIN_BUZZER", "buzzer", "buzzer", "Buzzer"),
                                       ("PIN_LED_RED", "led", "led_red", "Red LED"),
                                       ("PIN_LED_BLUE", "led", "led_blue", "Blue LED"),
                                       ("PIN_LED_GREEN", "led", "led_green", "Green LED")):
            side, x, y, plabel = pin(name)
            rows.append((y, name, kind, col, label, x, plabel, side))
        rows.sort()
        # The LEDs sit level with their pins (pushed apart only if pins are
        # too close); the buzzer, the top row, is lifted clear above them.
        row_y = {}
        prev = None
        for y, name, *_ in rows[1:]:
            ry = y if prev is None else max(y, prev + 72)
            row_y[name] = ry
            prev = ry
        row_y[rows[0][1]] = min(rows[0][0], row_y[rows[1][1]] - 120)
        lane = BX - 40
        for y, name, kind, col, label, x, plabel, side in rows:
            ry = row_y[name]
            if ry == y:
                dw.wire([(x, y), (res[1], ry)], C[col])
            else:
                dw.wire([(x, y), (lane, y), (lane, ry), (res[1], ry)], C[col])
            used[(side, plabel)] = C[col]
            dw.wire([(bus_x, ry), (part_x - 7 if kind == "led" else part_x - 8, ry)], C["gnd"])
            draw_resistor(dw, part_x + 8, res[1], ry, BANDS_220 if kind == "led" else BANDS_100,
                          "220 Ω" if kind == "led" else "100 Ω")
            if kind == "led":
                draw_led(dw, part_x, ry, C[col], label)
            else:
                draw_buzzer(dw, part_x, ry, label + " (passive)")
            dw.dot(bus_x, ry, C["gnd"])
        top_row = min(row_y.values())
        dw.wire([(bus_x, top_row), (bus_x, RAIL_BOT_GND)], C["gnd"])
        dw.dot(bus_x, RAIL_BOT_GND, C["gnd"])

        # Button: right column, wired across diagonal legs.
        side, x, y, plabel = pin("PIN_BUTTON_RESET")
        tl, br = draw_button(dw, 1040, y + 45)
        dw.wire([(x, y), (tl[0] - 26, y), (tl[0] - 26, tl[1]), tl], C["button"])
        dw.wire([br, (br[0] + 26, br[1]), (br[0] + 26, RAIL_BOT_GND)], C["gnd"])
        dw.dot(br[0] + 26, RAIL_BOT_GND, C["gnd"])
        used[(side, plabel)] = C["button"]

    draw_esp32(dw, used)

    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
           f'<rect width="{W}" height="{H}" fill="{BG}"/>']
    svg += dw.under + dw.wires + dw.over
    y = TITLE_Y
    for i, line in enumerate(title):
        svg.append(text(TITLE_X, y, line, 26 if i == 0 else 21, "#111", "start", "bold"))
        y += 30
    for line in subtitle:
        svg.append(text(TITLE_X, y, line, 15, "#444"))
        y += 21
    svg.append("</svg>")
    return "\n".join(svg) + "\n"


PICTURES = {
    "wiring.svg": ({"rfid", "oled", "leds"}, ["Full wiring"],
                   ["Match pins by the label printed", "on your board (D5 = GPIO 5).",
                    "Everything runs on 3.3 V.", "Wire with the USB unplugged."]),
    "step-2-rfid.svg": ({"rfid"}, ["Step 2", "RFID reader"],
                        ["7 wires; 3.3V and GND go", "to the top power rails.", "Never connect the reader", "to 5V / VIN."]),
    "step-3-oled.svg": ({"oled"}, ["Step 3", "OLED display"],
                        ["4 wires.", "VCC/GND order differs between", "sellers: go by YOUR module's", "printed labels."]),
    "step-4-leds-buzzer-button.svg": ({"leds"}, ["Step 4", "LEDs, buzzer, button"],
                                      ["LED long leg (+) goes towards", "the resistor; short leg (−)", "to the GND rail."]),
}


def main():
    cfg = read_config()
    check = "--check" in sys.argv[1:]
    stale = []
    OUT.mkdir(parents=True, exist_ok=True)
    for fname, (groups, title, sub) in PICTURES.items():
        svg = build(cfg, groups, title, sub)
        path = OUT / fname
        if check:
            if not path.exists() or path.read_text() != svg:
                stale.append(str(path.relative_to(ROOT)))
        else:
            path.write_text(svg)
    if stale:
        print("Wiring diagrams are out of date with include/config.h - run: python3 tools/wiring_diagram.py")
        for s in stale:
            print("  - " + s)
        sys.exit(1)
    print("Wiring diagrams " + ("are up to date." if check else f"written to {OUT.relative_to(ROOT)}/"))


if __name__ == "__main__":
    main()
