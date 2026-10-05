#!/usr/bin/env python3
"""Generate CallOnFail v1 KiCad schematics (KiCad 10), rev 1.5.3.

Two hierarchical sheets:
  01-alimentacion  — 5V_PSU, XY-SJVA, gel, buck-boost, Q1, TPL5010@BUS, 74HCT123
  02-io            — WT32, FT232, A7672, PCF8575, campo, RS485, frente

Layout rules (KiCad docs + readable-schematic practice):
  - 50 mil / 1.27 mm grid for symbols, pins and wires
  - Flow left→right (inputs, processing, outputs); power source→load
  - Parts that work together sit together; whitespace between blocks
  - Wires inside a block; labels if a wire would cross another block
  - join() path budget MAX_JOIN_WIRE — beyond that always labels (no spaghetti)
  - IC↔nearby R/C/PU/PD: always wire (join_local), never labels
  - Never run a wire along a symbol pin column; T-junctions only
  - GND down, supplies up
  - Never route a wire across a symbol body (modules + passives + connectors)
  - No overlapping symbol bodies; whitespace between blocks
  - Leave an IC pin straight (then bend); do not turn on top of the pin number

Rev 1.5 vs 1.4: no HCT14/CD4541/ULN/discrete relays; 2N3904 + TPL5010 +
PCF8575 + PC817 + relay module + RS485 + FT232.
Rev 1.5.1: TPL5010 on power sheet; RELAY JD-VCC; ZMPT 10k/15k; J9/J10 +3V3.
Rev 1.5.2: no TLC555 — BTN holds SYS off; TPL RSTn → EN; file version 20260306.
Rev 1.5.3: no Q3; 5V_MODEM=5V_SYS; TPL@5V_BUS+NPN DONE; 74HCT123~2s→Q1;
  trig OR TPL/P12/BTN (active low); /CLR blank on plug; TPL ~10 min; EN=lab.
"""

from __future__ import annotations

import re
import uuid
from pathlib import Path

OUT = Path(__file__).resolve().parent
LIB = Path(r"C:\Program Files\KiCad\10.0\share\kicad\symbols")
KICAD_PRO_TEMPLATE = Path(r"C:\Program Files\KiCad\10.0\share\kicad\template\kicad.kicad_pro")

ROOT_UUID = "a1111111-1111-4111-8111-111111111111"
PWR_UUID = "a2222222-2222-4222-8222-222222222222"
IO_UUID = "a3333333-3333-4333-8333-333333333333"
PROJECT = "cof-v1"

EXTRACT = {
    "Device:R": ("Device.kicad_sym", "R"),
    "Device:C": ("Device.kicad_sym", "C"),
    "Device:C_Polarized": ("Device.kicad_sym", "C_Polarized"),
    "Device:D": ("Device.kicad_sym", "D"),
    "Device:LED": ("Device.kicad_sym", "LED"),
    "Device:Fuse": ("Device.kicad_sym", "Fuse"),
    "Device:Battery": ("Device.kicad_sym", "Battery"),
    "Device:Q_PMOS": ("Device.kicad_sym", "Q_PMOS"),
    "Device:Q_NPN": ("Device.kicad_sym", "Q_NPN"),
    "Switch:SW_Push": ("Switch.kicad_sym", "SW_Push"),
    "Connector_Generic:Conn_01x02": ("Connector_Generic.kicad_sym", "Conn_01x02"),
    "Connector_Generic:Conn_01x04": ("Connector_Generic.kicad_sym", "Conn_01x04"),
    "Connector_Generic:Conn_01x05": ("Connector_Generic.kicad_sym", "Conn_01x05"),
    "Connector_Generic:Conn_01x06": ("Connector_Generic.kicad_sym", "Conn_01x06"),
    "Connector_Generic:Conn_01x08": ("Connector_Generic.kicad_sym", "Conn_01x08"),
    "power:GND": ("power.kicad_sym", "GND"),
    "power:PWR_FLAG": ("power.kicad_sym", "PWR_FLAG"),
}


def g(n: int | float) -> float:
    """50 mil KiCad connection grid."""
    return round(float(n) * 1.27, 2)


ESCAPE = g(8)  # 10.16 mm past the pin, then the first bend
# Prefer labels over spaghetti: join() wires only if the path stays short.
MAX_JOIN_WIRE = g(48)  # ~61 mm manhattan/path budget


def uid() -> str:
    return str(uuid.uuid4())


def take_sexp(s: str, start: int) -> str:
    depth = 0
    i = start
    n = len(s)
    while i < n:
        c = s[i]
        if c == '"':
            i += 1
            while i < n:
                if s[i] == "\\":
                    i += 2
                    continue
                if s[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return s[start : i + 1]
        i += 1
    raise ValueError("unterminated s-expr")


def extract_symbol(lib_file: str, name: str) -> str:
    lib_id = f"{lib_file.replace('.kicad_sym', '')}:{name}"
    if LIB.exists():
        text = (LIB / lib_file).read_text(encoding="utf-8")
        needle = f'(symbol "{name}"'
        idx = 0
        while True:
            j = text.find(needle, idx)
            if j < 0:
                break
            after = j + len(needle)
            if after < len(text) and text[after] in " \r\n\t":
                body = take_sexp(text, j)
                return body.replace(f'(symbol "{name}"', f'(symbol "{lib_id}"', 1)
            idx = j + 1
    for sch_name in ("01-alimentacion.kicad_sch", "02-io.kicad_sch"):
        path = OUT / sch_name
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        needle = f'(symbol "{lib_id}"'
        j = text.find(needle)
        if j >= 0:
            return take_sexp(text, j)
    raise KeyError(f"{lib_id} not in KiCad libs or existing sheets")


def parse_pins(sym_text: str) -> dict[str, tuple[float, float, float]]:
    pins: dict[str, tuple[float, float, float]] = {}
    i = 0
    while True:
        j = sym_text.find("(pin ", i)
        if j < 0:
            break
        block = take_sexp(sym_text, j)
        at = re.search(r"\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)", block)
        num = re.search(r'\(number "([^"]+)"', block)
        if at and num:
            pins[num.group(1)] = (
                float(at.group(1)),
                float(at.group(2)),
                float(at.group(3)),
            )
        i = j + 1
    return pins


def rot_xy(x: float, y: float, rot: int) -> tuple[float, float]:
    rot %= 360
    if rot == 0:
        return x, y
    if rot == 90:
        return -y, x
    if rot == 180:
        return -x, -y
    if rot == 270:
        return y, -x
    raise ValueError(rot)


def make_box(
    lib_id: str,
    value: str,
    left: list[tuple[str, str]],
    right: list[tuple[str, str]],
    pitch: float = 5.08,
    width: float = 50.80,
) -> str:
    """Custom module box. left/right: (pin_number, pin_name)."""
    n = max(len(left), len(right), 1)
    y0 = (n - 1) * pitch / 2
    height_half = (n - 1) * pitch / 2 + pitch
    w = width
    pin_len = 3.81
    pins_s = []
    for i, (num, name) in enumerate(left):
        y = y0 - i * pitch
        pins_s.append(
            f"""			(pin passive line
				(at {-w/2 - pin_len:.2f} {y:.2f} 0)
				(length {pin_len:.2f})
				(name "{name}"
					(effects (font (size 1.524 1.524)))
				)
				(number "{num}"
					(effects (font (size 1.27 1.27)))
				)
			)"""
        )
    for i, (num, name) in enumerate(right):
        y = y0 - i * pitch
        pins_s.append(
            f"""			(pin passive line
				(at {w/2 + pin_len:.2f} {y:.2f} 180)
				(length {pin_len:.2f})
				(name "{name}"
					(effects (font (size 1.524 1.524)))
				)
				(number "{num}"
					(effects (font (size 1.27 1.27)))
				)
			)"""
        )
    short = lib_id.split(":")[1]
    return f"""		(symbol "{lib_id}"
			(exclude_from_sim no)
			(in_bom yes)
			(on_board yes)
			(in_pos_files yes)
			(duplicate_pin_numbers_are_jumpers no)
			(property "Reference" "U"
				(at 0 {-height_half - 2.54:.2f} 0)
				(show_name no)
				(effects (font (size 1.27 1.27)))
			)
			(property "Value" "{value}"
				(at 0 0 0)
				(show_name no)
				(effects (font (size 1.524 1.524)))
			)
			(property "Footprint" ""
				(at 0 0 0)
				(hide yes)
				(effects (font (size 1.27 1.27)))
			)
			(property "Datasheet" "~"
				(at 0 0 0)
				(hide yes)
				(effects (font (size 1.27 1.27)))
			)
			(property "Description" "CallOnFail module"
				(at 0 0 0)
				(hide yes)
				(effects (font (size 1.27 1.27)))
			)
			(symbol "{short}_0_1"
				(rectangle
					(start {-w/2:.2f} {-height_half:.2f})
					(end {w/2:.2f} {height_half:.2f})
					(stroke (width 0.254) (type default))
					(fill (type background))
				)
			)
			(symbol "{short}_1_1"
{chr(10).join(pins_s)}
			)
			(embedded_fonts no)
		)"""


BOXES = {
    "Module:XY_SJVA": (
        "XY-SJVA CC/CV",
        [("1", "IN+"), ("2", "IN-")],
        [("3", "OUT+"), ("4", "OUT-")],
    ),
    "Module:BUCKBOOST": (
        "XL6019 auto 5.1V",
        [("1", "VIN+"), ("2", "VIN-"), ("3", "EN")],
        [("4", "VOUT+"), ("5", "VOUT-")],
    ),
    "Module:WT32": (
        "WT32-ETH01",
        [
            ("1", "5V"),
            ("2", "GND"),
            ("3", "3V3"),
            ("4", "TXD0"),
            ("5", "RXD0"),
            ("6", "IO0"),
            ("7", "EN"),
        ],
        [
            ("8", "IO2"),
            ("9", "IO4"),
            ("10", "IO5"),
            ("11", "IO12"),
            ("12", "IO14"),
            ("13", "IO15"),
            ("14", "IO17"),
            ("15", "IO32"),
            ("16", "IO33"),
            ("17", "IO35"),
            ("18", "IO36"),
            ("19", "IO39"),
        ],
    ),
    "Module:A7672": (
        "A7672SA-FASE",
        [("1", "VCC"), ("2", "GND"), ("3", "TXD"), ("4", "RXD"), ("5", "RESET"), ("6", "PWRKEY")],
        [("7", "ANT")],
    ),
    "Module:FT232": (
        "FT232 USB-UART 3V3",
        [("1", "GND"), ("2", "TXD"), ("3", "RXD")],
        [],
    ),
    "Module:OLED": (
        "OLED SH1106 0x3C",
        [("1", "VCC"), ("2", "GND")],
        [("3", "SDA"), ("4", "SCL")],
    ),
    "Module:SHT31": (
        "SHT31 0x44",
        [("1", "VCC"), ("2", "GND")],
        [("3", "SDA"), ("4", "SCL")],
    ),
    "Module:PCF8575": (
        "PCF8575 0x20",
        [
            ("1", "VCC"),
            ("2", "GND"),
            ("3", "SDA"),
            ("4", "SCL"),
            ("5", "A0"),
            ("6", "A1"),
            ("7", "A2"),
        ],
        [
            ("8", "P00"),
            ("9", "P01"),
            ("10", "P02"),
            ("11", "P03"),
            ("12", "P04"),
            ("13", "P05"),
            ("14", "P06"),
            ("15", "P07"),
            ("16", "P10"),
            ("17", "P11"),
            ("18", "P12"),
            ("19", "P13"),
            ("20", "P14"),
            ("21", "P15"),
            ("22", "P16"),
            ("23", "P17"),
        ],
    ),
    "Module:TPL5010": (
        "TPL5010 WDT",
        [("1", "VDD"), ("2", "GND"), ("3", "DONE")],
        [("4", "WAKE"), ("5", "RSTn"), ("6", "DELAY")],
    ),
    # Functional 74HCT123 (one mono used). RCx = REXT/CEXT pin.
    "Module:HCT123": (
        "74HCT123 one-shot",
        [
            ("1", "A1"),
            ("2", "B1"),
            ("3", "/CLR1"),
            ("8", "A2"),
            ("9", "B2"),
            ("10", "/CLR2"),
            ("4", "GND"),
        ],
        [("5", "Q"), ("6", "VCC"), ("7", "RCx")],
    ),
    "Module:PC817_2CH": (
        "PC817 2CH opto",
        [("1", "IN1"), ("2", "IN2"), ("3", "GND_F")],
        [("4", "OUT1"), ("5", "OUT2"), ("6", "VCC"), ("7", "GND")],
    ),
    "Module:WATER_DET": (
        "Water det relay",
        [("1", "VCC"), ("2", "GND"), ("3", "PROBE")],
        [("4", "NO"), ("5", "COM"), ("6", "NC")],
    ),
    "Module:RELAY2_OPTO": (
        "2-relay opto 5V",
        [("1", "VCC"), ("2", "JD-VCC"), ("3", "GND"), ("4", "IN1"), ("5", "IN2")],
        [("6", "COM1"), ("7", "NO1"), ("8", "COM2"), ("9", "NO2")],
    ),
    "Module:RS485": (
        "TTL-RS485 auto",
        [("1", "VCC"), ("2", "GND"), ("3", "TXD"), ("4", "RXD")],
        [("5", "A"), ("6", "B")],
    ),
    "Module:ZMPT": (
        "ZMPT101B",
        [("1", "VCC"), ("2", "GND"), ("3", "OUT")],
        [("4", "VAC")],
    ),
    "Module:DS18B20": (
        "DS18B20 x4 bus",
        [("1", "VDD"), ("2", "GND"), ("3", "DATA")],
        [],
    ),
    "Module:BUZZER": (
        "Buzzer 5V activo",
        [("1", "+"), ("2", "-")],
        [],
    ),
    "Module:LM66200": (
        "LM66200 dual ideal diode",
        [("1", "VIN1"), ("2", "VIN2"), ("3", "GND"), ("5", "EN")],
        [("4", "VOUT"), ("6", "STAT")],
    ),
}


class Sch:
    def __init__(self, sheet_uuid: str, title: str, comment: str, path: str, paper: str = "A2"):
        self.sheet_uuid = sheet_uuid
        self.title = title
        self.comment = comment
        self.path = path
        self.paper = paper
        self.lib_ids: set[str] = set()
        self.parts: dict[str, dict] = {}
        self.items: list[str] = []
        self.pwr_n = 0
        self.pin_lookup: dict[str, dict[str, tuple[float, float, float]]] = {}
        self._wire_ends: list[tuple[float, float]] = []
        self._junc_pts: set[tuple[float, float]] = set()

    def use(self, lib_id: str) -> None:
        self.lib_ids.add(lib_id)

    def place(self, lib_id: str, ref: str, value: str, x: float, y: float, rot: int = 0) -> None:
        self.use(lib_id)
        self.parts[ref] = {"lib": lib_id, "value": value, "x": x, "y": y, "rot": rot}

    def is_ic(self, ref: str) -> bool:
        lib = self.parts[ref]["lib"]
        return lib.startswith(("Module:", "Timer:", "Transistor_Array:"))

    def is_passive(self, ref: str) -> bool:
        lib = self.parts[ref]["lib"]
        return lib.startswith(
            ("Device:R", "Device:C", "Device:C_Polarized", "Device:D", "Device:LED")
        )

    def is_keepout(self, ref: str) -> bool:
        """Bodies wires must not cross (modules, actives, passives, connectors)."""
        lib = self.parts[ref]["lib"]
        return self.is_ic(ref) or lib.startswith(
            (
                "Device:Q_PMOS",
                "Device:Q_NPN",
                "Device:R",
                "Device:C",
                "Device:C_Polarized",
                "Device:LED",
                "Device:D",
                "Device:Battery",
                "Transistor_FET:",
                "Switch:",
                "Connector_Generic:",
            )
        )

    def pin(self, ref: str, num: str) -> tuple[float, float]:
        p = self.parts[ref]
        lx, ly, _prot = self.pin_lookup[p["lib"]][num]
        rx, ry = rot_xy(lx, ly, p["rot"])
        return p["x"] + rx, p["y"] - ry

    def escape_dir(self, ref: str, num: str) -> tuple[float, float]:
        """Unit vector on the sheet, away from the symbol body."""
        p = self.parts[ref]
        _lx, _ly, pin_rot = self.pin_lookup[p["lib"]][num]
        pr = int(pin_rot) % 360
        if pr == 0:
            sx, sy = -1.0, 0.0
        elif pr == 90:
            sx, sy = 0.0, -1.0
        elif pr == 180:
            sx, sy = 1.0, 0.0
        else:
            sx, sy = 0.0, 1.0
        rx, ry = rot_xy(sx, sy, p["rot"])
        return rx, -ry

    def escape_pt(self, ref: str, num: str, d: float = ESCAPE) -> tuple[float, float]:
        x, y = self.pin(ref, num)
        dx, dy = self.escape_dir(ref, num)
        return x + dx * d, y + dy * d

    def body_bbox(self, ref: str) -> tuple[float, float, float, float]:
        xs: list[float] = []
        ys: list[float] = []
        for num in self.pin_lookup[self.parts[ref]["lib"]]:
            x, y = self.pin(ref, num)
            xs.append(x)
            ys.append(y)
        inset = 1.27
        return (min(xs) + inset, min(ys) + inset, max(xs) - inset, max(ys) - inset)

    def _seg_hits_box(
        self, x1: float, y1: float, x2: float, y2: float, box: tuple[float, float, float, float]
    ) -> bool:
        x0, y0, x3, y3 = box
        if x3 <= x0 or y3 <= y0:
            return False
        if abs(y1 - y2) < 0.02:
            y = y1
            if y <= y0 or y >= y3:
                return False
            xa, xb = (x1, x2) if x1 < x2 else (x2, x1)
            return xa < x3 and xb > x0
        if abs(x1 - x2) < 0.02:
            x = x1
            if x <= x0 or x >= x3:
                return False
            ya, yb = (y1, y2) if y1 < y2 else (y2, y1)
            return ya < y3 and yb > y0
        return False

    def _hits_ic(self, x1: float, y1: float, x2: float, y2: float) -> bool:
        for ref in self.parts:
            if not self.is_keepout(ref):
                continue
            if self._seg_hits_box(x1, y1, x2, y2, self.body_bbox(ref)):
                return True
        return False

    def _hits_pin_column(self, x1: float, y1: float, x2: float, y2: float) -> bool:
        """Reject a vertical run that rides an IC's pin column."""
        if abs(x1 - x2) > 0.05:
            return False
        x = x1
        ya, yb = (y1, y2) if y1 < y2 else (y2, y1)
        for ref in self.parts:
            if not self.is_keepout(ref):
                continue
            cols: dict[float, list[float]] = {}
            for num in self.pin_lookup[self.parts[ref]["lib"]]:
                px, py = self.pin(ref, num)
                key = round(px, 2)
                cols.setdefault(key, []).append(py)
            for cx, ys in cols.items():
                if abs(x - cx) > 0.05:
                    continue
                if ya < max(ys) - 0.05 and yb > min(ys) + 0.05:
                    return True
        return False

    def _path_len(self, pts: list[tuple[float, float]]) -> float:
        total = 0.0
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            total += abs(x2 - x1) + abs(y2 - y1)
        return total

    def _stub_keepouts(self) -> list[tuple[float, float, float, float, str]]:
        """Axis-aligned keep-outs along each IC's stub column (x1,y1,x2,y2,axis)."""
        boxes: list[tuple[float, float, float, float, str]] = []
        for ref in self.parts:
            if not self.is_keepout(ref):
                continue
            left_ys: list[float] = []
            right_ys: list[float] = []
            top_xs: list[float] = []
            bot_xs: list[float] = []
            left_x = right_x = top_y = bot_y = None
            for num in self.pin_lookup[self.parts[ref]["lib"]]:
                px, py = self.pin(ref, num)
                dx, dy = self.escape_dir(ref, num)
                if dx < -0.5:
                    left_x = px + dx * ESCAPE
                    left_ys.append(py)
                elif dx > 0.5:
                    right_x = px + dx * ESCAPE
                    right_ys.append(py)
                elif dy < -0.5:
                    top_y = py + dy * ESCAPE
                    top_xs.append(px)
                else:
                    bot_y = py + dy * ESCAPE
                    bot_xs.append(px)
            pad = 0.6
            if left_x is not None and left_ys:
                boxes.append((left_x, min(left_ys) - pad, left_x, max(left_ys) + pad, "V"))
            if right_x is not None and right_ys:
                boxes.append((right_x, min(right_ys) - pad, right_x, max(right_ys) + pad, "V"))
            if top_y is not None and top_xs:
                boxes.append((min(top_xs) - pad, top_y, max(top_xs) + pad, top_y, "H"))
            if bot_y is not None and bot_xs:
                boxes.append((min(bot_xs) - pad, bot_y, max(bot_xs) + pad, bot_y, "H"))
        return boxes

    def _hits_stub_column(self, x1: float, y1: float, x2: float, y2: float) -> bool:
        """A run along an IC stub column would short every pin on that side."""
        for x0, y0, x3, y3, axis in self._stub_keepouts():
            if axis == "V" and abs(x1 - x2) < 0.05 and abs(x1 - x0) < 0.05:
                ya, yb = (y1, y2) if y1 < y2 else (y2, y1)
                if ya < y3 - 0.05 and yb > y0 + 0.05:
                    return True
            if axis == "H" and abs(y1 - y2) < 0.05 and abs(y1 - y0) < 0.05:
                xa, xb = (x1, x2) if x1 < x2 else (x2, x1)
                if xa < x3 - 0.05 and xb > x0 + 0.05:
                    return True
        return False

    def _path_ok(self, pts: list[tuple[float, float]]) -> bool:
        for (a, b) in zip(pts, pts[1:]):
            if self._hits_ic(a[0], a[1], b[0], b[1]):
                return False
            if self._hits_pin_column(a[0], a[1], b[0], b[1]):
                return False
            if self._hits_stub_column(a[0], a[1], b[0], b[1]):
                return False
        return True

    def _draw_path(self, pts: list[tuple[float, float]]) -> None:
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            if abs(x1 - x2) > 0.02 or abs(y1 - y2) > 0.02:
                self.wire(x1, y1, x2, y2)

    def _grid(self, v: float) -> float:
        return round(round(v / 1.27) * 1.27, 2)

    def _around_paths(self, x1: float, y1: float, x2: float, y2: float) -> list[list[tuple[float, float]]]:
        paths: list[list[tuple[float, float]]] = [
            [(x1, y1), (x1, y2), (x2, y2)],
            [(x1, y1), (x2, y1), (x2, y2)],
        ]
        margin = g(2)
        for ref in self.parts:
            if not self.is_keepout(ref):
                continue
            x0, y0, x3, y3 = self.body_bbox(ref)
            top = self._grid(y0 - margin)
            bot = self._grid(y3 + margin)
            left = self._grid(x0 - margin)
            right = self._grid(x3 + margin)
            paths.extend(
                [
                    [(x1, y1), (x1, top), (x2, top), (x2, y2)],
                    [(x1, y1), (x1, bot), (x2, bot), (x2, y2)],
                    [(x1, y1), (left, y1), (left, y2), (x2, y2)],
                    [(x1, y1), (right, y1), (right, y2), (x2, y2)],
                    [(x1, y1), (x1, top), (right, top), (right, y2), (x2, y2)],
                    [(x1, y1), (x1, bot), (right, bot), (right, y2), (x2, y2)],
                    [(x1, y1), (x1, top), (left, top), (left, y2), (x2, y2)],
                    [(x1, y1), (x1, bot), (left, bot), (left, y2), (x2, y2)],
                ]
            )
        for x0, y0, x3, y3, axis in self._stub_keepouts():
            if axis == "V":
                left = self._grid(x0 - g(4))
                right = self._grid(x0 + g(4))
                paths.extend(
                    [
                        [(x1, y1), (left, y1), (left, y2), (x2, y2)],
                        [(x1, y1), (right, y1), (right, y2), (x2, y2)],
                    ]
                )
            else:
                top = self._grid(y0 - g(4))
                bot = self._grid(y0 + g(4))
                paths.extend(
                    [
                        [(x1, y1), (x1, top), (x2, top), (x2, y2)],
                        [(x1, y1), (x1, bot), (x2, bot), (x2, y2)],
                    ]
                )
        return paths

    def _best_path(
        self, x1: float, y1: float, x2: float, y2: float
    ) -> list[tuple[float, float]] | None:
        candidates = [p for p in self._around_paths(x1, y1, x2, y2) if self._path_ok(p)]
        if not candidates:
            return None
        return min(candidates, key=self._path_len)

    def route(self, x1: float, y1: float, x2: float, y2: float) -> bool:
        """Short manhattan route. Never crosses a symbol body or pin column."""
        if abs(x1 - x2) < 0.02 and abs(y1 - y2) < 0.02:
            return True
        path = self._best_path(x1, y1, x2, y2)
        if path is not None:
            self._draw_path(path)
            return True
        xs = [x1, x2]
        ys = [y1, y2]
        for ref in self.parts:
            if not self.is_keepout(ref):
                continue
            x0, y0, x3, y3 = self.body_bbox(ref)
            if max(x1, x2) < x0 - 2 or min(x1, x2) > x3 + 2:
                continue
            if max(y1, y2) < y0 - 2 or min(y1, y2) > y3 + 2:
                continue
            xs.extend([x0, x3])
            ys.extend([y0, y3])
        top = self._grid(min(ys) - g(3))
        hull = [(x1, y1), (x1, top), (x2, top), (x2, y2)]
        if self._path_ok(hull):
            self._draw_path(hull)
            return True
        bot = self._grid(max(ys) + g(3))
        hull = [(x1, y1), (x1, bot), (x2, bot), (x2, y2)]
        if self._path_ok(hull):
            self._draw_path(hull)
            return True
        return False

    def wire(self, x1: float, y1: float, x2: float, y2: float) -> None:
        self.items.append(
            f"""	(wire
		(pts (xy {x1:.2f} {y1:.2f}) (xy {x2:.2f} {y2:.2f}))
		(stroke (width 0) (type default))
		(uuid "{uid()}")
	)"""
        )
        # Track endpoints for auto-junction pass (KiCad does not invent dots).
        self._wire_ends.append((round(x1, 2), round(y1, 2)))
        self._wire_ends.append((round(x2, 2), round(y2, 2)))

    def manh(self, x1: float, y1: float, x2: float, y2: float) -> None:
        self.route(x1, y1, x2, y2)

    def leave_pin(
        self, ref: str, pin: str, dest: tuple[float, float] | None = None
    ) -> tuple[float, float]:
        """One stub straight off an IC pin; never turn on the pin number."""
        x, y = self.pin(ref, pin)
        if not self.is_ic(ref):
            return x, y
        dx, dy = self.escape_dir(ref, pin)
        d = ESCAPE
        if dest is not None:
            tx, ty = dest
            along = (tx - x) * dx + (ty - y) * dy
            perp = abs((tx - x) * (-dy) + (ty - y) * dx)
            if along > g(4) and perp < g(2):
                d = min(d, along - g(2))
                d = max(d, g(4))
            elif 0 < along < d:
                d = max(g(4), along * 0.5)
        ex, ey = x + dx * d, y + dy * d
        self.wire(x, y, ex, ey)
        return ex, ey

    def _side_vec(self, side: str) -> tuple[float, float]:
        return {"L": (-1.0, 0.0), "R": (1.0, 0.0), "U": (0.0, -1.0), "D": (0.0, 1.0)}[side]

    def _label_pair(self, ra: str, pa: str, rb: str, pb: str, x1: float, y1: float, x2: float, y2: float, net: str | None) -> None:
        """Wire would cross a block: labels, never a cable over a body."""
        name = net or f"{ra}_{pa}"
        if self.is_ic(ra):
            dx, dy = self.escape_dir(ra, pa)
            rot1 = self._dir_rot(dx, dy)
        else:
            rot1 = 0 if x2 >= x1 else 180
        if self.is_ic(rb):
            dx, dy = self.escape_dir(rb, pb)
            rot2 = self._dir_rot(dx, dy)
        else:
            rot2 = 180 if x2 >= x1 else 0
        self.llabel(name, x1, y1, rot1)
        self.llabel(name, x2, y2, rot2)

    def join_local(self, ra: str, pa: str, rb: str, pb: str) -> None:
        """Force a short wire (IC pin ↔ nearby R/C/PU/PD). Never labels."""
        x1, y1 = self.pin(ra, pa)
        x2, y2 = self.pin(rb, pb)
        if self.is_ic(ra):
            x1, y1 = self.leave_pin(ra, pa, (x2, y2))
        if self.is_ic(rb):
            x2, y2 = self.leave_pin(rb, pb, (x1, y1))
        if abs(x1 - x2) < 0.02 and abs(y1 - y2) < 0.02:
            return
        if abs(y1 - y2) < 0.02 or abs(x1 - x2) < 0.02:
            self.wire(x1, y1, x2, y2)
            return
        self.wire(x1, y1, x2, y1)
        self.junc(x2, y1)
        self.wire(x2, y1, x2, y2)

    def join(self, ra: str, pa: str, rb: str, pb: str, mid: float | None = None, net: str | None = None) -> None:
        # Any IC↔R/C/D/LED: always wire. Never invent "R27_2" / "C2_1" labels.
        if self.is_passive(ra) or self.is_passive(rb):
            self.join_local(ra, pa, rb, pb)
            return
        p1 = self.pin(ra, pa)
        p2 = self.pin(rb, pb)
        x1, y1 = self.leave_pin(ra, pa, p2)
        x2, y2 = self.leave_pin(rb, pb, p1)
        # Far apart → named labels only (net= required; no ref_pin garbage).
        if abs(x1 - x2) + abs(y1 - y2) > MAX_JOIN_WIRE:
            if net:
                self._label_pair(ra, pa, rb, pb, x1, y1, x2, y2, net)
            else:
                self.join_local(ra, pa, rb, pb)
            return
        path = None
        if mid is not None:
            cand = [(x1, y1), (mid, y1), (mid, y2), (x2, y2)]
            if self._path_ok(cand):
                path = cand
        if path is None:
            path = self._best_path(x1, y1, x2, y2)
        if path is not None and self._path_len(path) <= MAX_JOIN_WIRE:
            self._draw_path(path)
            return
        if net:
            self._label_pair(ra, pa, rb, pb, x1, y1, x2, y2, net)
        else:
            self.join_local(ra, pa, rb, pb)

    def join_via_x(self, ra: str, pa: str, rb: str, pb: str, x: float) -> None:
        p1 = self.pin(ra, pa)
        p2 = self.pin(rb, pb)
        x1, y1 = self.leave_pin(ra, pa, (x, p1[1]))
        x2, y2 = self.leave_pin(rb, pb, (x, p2[1]))
        self.route(x1, y1, x, y1)
        self.junc(x, y1)
        self.wire(x, y1, x, y2)
        self.junc(x, y2)
        self.route(x, y2, x2, y2)

    def tap_x(self, ref: str, pin: str, x: float) -> tuple[float, float]:
        px, py = self.pin(ref, pin)
        dest = (x, py)
        if self.is_ic(ref):
            dx, dy = self.escape_dir(ref, pin)
            if abs(dy) < 0.01 and (x - px) * dx > 0.02:
                self.wire(px, py, x, py)
                self.junc(x, py)
                return x, py
            ex, ey = self.leave_pin(ref, pin, dest)
            if self.route(ex, ey, x, py):
                self.junc(x, py)
                return x, py
            dx, dy = self.escape_dir(ref, pin)
            self.llabel(f"{ref}_{pin}", ex, ey, self._dir_rot(dx, dy))
            return ex, ey
        self.wire(px, py, x, py)
        self.junc(x, py)
        return x, py

    def vspine(self, x: float, pts: list[tuple[float, float]], net: str | None = None, side: str = "L") -> None:
        """Vertical bus: each point hops horizontally to x, then one vertical wire."""
        ys = [py for _px, py in pts]
        for px, py in pts:
            if abs(px - x) > 0.05:
                self.wire(px, py, x, py)
            self.junc(x, py)
        self.wire(x, min(ys), x, max(ys))
        if net:
            if side == "L":
                self.glabel(net, x, min(ys), 180)
            elif side == "R":
                self.glabel(net, x, min(ys), 0)
            elif side == "U":
                self.glabel(net, x, min(ys), 90)
            else:
                self.glabel(net, x, max(ys), 270)

    def tap_net(self, ref: str, pin: str, net: str, side: str = "R", d: float = 7.62) -> None:
        """Global label only at a block boundary."""
        self.stub(ref, pin, net, side, d)

    def llabel(self, name: str, x: float, y: float, rot: int = 0) -> None:
        justify = "right" if rot in (180, 270) else "left"
        self.items.append(
            f"""	(label "{name}"
		(at {x:.2f} {y:.2f} {rot})
		(effects (font (size 1.27 1.27)) (justify {justify}))
		(uuid "{uid()}")
	)"""
        )

    def _dir_rot(self, dx: float, dy: float) -> int:
        if dx < -0.5:
            return 180
        if dx > 0.5:
            return 0
        if dy < -0.5:
            return 90
        return 270

    def _away_vec(self, ref: str, pin: str, side: str) -> tuple[float, float]:
        """Unit vector for a label stub that does not run into another pin of this part."""
        sx, sy = self._side_vec(side)
        x, y = self.pin(ref, pin)
        others = [
            self.pin(ref, n)
            for n in self.pin_lookup[self.parts[ref]["lib"]]
            if n != pin
        ]
        if not others:
            return sx, sy
        hit = False
        for ox, oy in others:
            vx, vy = ox - x, oy - y
            if vx * sx + vy * sy > 0.5 and abs(vx * (-sy) + vy * sx) < 2.0:
                hit = True
                break
        if not hit:
            return sx, sy
        ox, oy = others[0]
        dx, dy = x - ox, y - oy
        if abs(dx) >= abs(dy):
            return (1.0, 0.0) if dx > 0 else (-1.0, 0.0)
        return (0.0, 1.0) if dy > 0 else (0.0, -1.0)

    def tap_local(self, ref: str, pin: str, net: str, side: str = "R", d: float = 7.62) -> None:
        """Same-sheet net; use when a wire would cross unrelated circuitry."""
        x, y = self.leave_pin(ref, pin)
        if self.is_ic(ref):
            dx, dy = self.escape_dir(ref, pin)
            self.llabel(net, x, y, self._dir_rot(dx, dy))
            return
        sx, sy = self._away_vec(ref, pin, side)
        self.wire(x, y, x + sx * d, y + sy * d)
        self.llabel(net, x + sx * d, y + sy * d, self._dir_rot(sx, sy))

    def stub(self, ref: str, pin: str, net: str, side: str, d: float = 10.16) -> None:
        if self.is_ic(ref):
            # Honor d so power/signal stubs on the same side can use distinct X.
            x, y = self.pin(ref, pin)
            dx, dy = self.escape_dir(ref, pin)
            ex, ey = x + dx * d, y + dy * d
            self.wire(x, y, ex, ey)
            self.glabel(net, ex, ey, self._dir_rot(dx, dy))
            return
        x, y = self.leave_pin(ref, pin)
        sx, sy = self._away_vec(ref, pin, side)
        self.wire(x, y, x + sx * d, y + sy * d)
        self.glabel(net, x + sx * d, y + sy * d, self._dir_rot(sx, sy))

    def junc(self, x: float, y: float) -> None:
        key = (round(x, 2), round(y, 2))
        if key in self._junc_pts:
            return
        self._junc_pts.add(key)
        self.items.append(
            f"""	(junction (at {key[0]:.2f} {key[1]:.2f}) (diameter 0) (color 0 0 0 0) (uuid "{uid()}"))"""
        )

    def finalize_junctions(self) -> None:
        """Place dots on every wire node with 3+ segments (T / cross)."""
        from collections import Counter

        ends = getattr(self, "_wire_ends", None)
        if not ends:
            return
        for (x, y), n in Counter(ends).items():
            if n >= 3:
                self.junc(x, y)

    def nc(self, x: float, y: float) -> None:
        self.items.append(f"""	(no_connect (at {x:.2f} {y:.2f}) (uuid "{uid()}"))""")

    def glabel(self, name: str, x: float, y: float, rot: int = 0) -> None:
        justify = "right" if rot in (180, 270) else "left"
        self.items.append(
            f"""	(global_label "{name}"
		(shape input)
		(at {x:.2f} {y:.2f} {rot})
		(effects (font (size 1.27 1.27)) (justify {justify}))
		(uuid "{uid()}")
	)"""
        )

    def text(self, msg: str, x: float, y: float, size: float = 1.27) -> None:
        escaped = msg.replace("\\", "\\\\").replace('"', '\\"')
        self.items.append(
            f"""	(text "{escaped}"
		(exclude_from_sim no)
		(at {x:.2f} {y:.2f} 0)
		(effects (font (size {size:.2f} {size:.2f})) (justify left bottom))
		(uuid "{uid()}")
	)"""
        )

    def stubs(self, ref: str, items: list[tuple[str, str, str]]) -> None:
        """Place labels with alternating stub length so they do not stack."""
        by_side: dict[str, list[tuple[str, str]]] = {}
        for pin, net, side in items:
            by_side.setdefault(side, []).append((pin, net))
        for side, lst in by_side.items():
            for i, (pin, net) in enumerate(lst):
                self.stub(ref, pin, net, side, d=10.16 + (i % 2) * 10.16)

    def rail(self, ref: str, pins: list[str], net: str, side: str | None = None) -> None:
        escaped = [self.leave_pin(ref, p) for p in pins]
        if side is None:
            dx, _dy = self.escape_dir(ref, pins[0]) if self.is_ic(ref) else (-1.0, 0.0)
            side = "L" if dx < 0 else "R"
        extra = g(6)
        if side == "L":
            bx = min(x for x, _y in escaped) - extra
        else:
            bx = max(x for x, _y in escaped) + extra
        ys = [y for _x, y in escaped]
        for x, y in escaped:
            if abs(x - bx) > 0.02:
                self.wire(x, y, bx, y)
            self.junc(bx, y)
        self.wire(bx, min(ys), bx, max(ys))
        self.glabel(net, bx, min(ys), 180 if side == "L" else 0)

    def gnd_rail(self, ref: str, pins: list[str], side: str | None = None) -> None:
        escaped = [self.leave_pin(ref, p) for p in pins]
        if side is None:
            dx, _dy = self.escape_dir(ref, pins[0]) if self.is_ic(ref) else (-1.0, 0.0)
            side = "L" if dx < 0 else "R"
        extra = g(6)
        if side == "L":
            bx = min(x for x, _y in escaped) - extra
        else:
            bx = max(x for x, _y in escaped) + extra
        ys = [y for _x, y in escaped]
        for x, y in escaped:
            if abs(x - bx) > 0.02:
                self.wire(x, y, bx, y)
            self.junc(bx, y)
        self.wire(bx, min(ys), bx, max(ys))
        bot = max(ys) + 5.08
        self.wire(bx, max(ys), bx, bot)
        self.gnd_at(bx, bot)

    def flyback(self, diode_ref: str, relay_ref: str) -> None:
        """1N4007 across coil: K to COIL+, A to COIL-."""
        kp = self.pin(relay_ref, "1")
        km = self.pin(relay_ref, "2")
        dx = min(kp[0], km[0]) - ESCAPE - g(4)
        mid = (kp[1] + km[1]) / 2
        self.place("Device:D", diode_ref, "1N4007", dx, mid, 270)
        self.join(diode_ref, "1", relay_ref, "1")
        self.junc(*self.pin(relay_ref, "1"))
        self.join(diode_ref, "2", relay_ref, "2")
        self.junc(*self.pin(relay_ref, "2"))

    def gnd_at(self, x: float, y: float) -> None:
        self.pwr_n += 1
        ref = f"#PWR{self.pwr_n:02d}"
        self.place("power:GND", ref, "GND", x, y, 0)

    def gnd_pin(self, ref: str, pin: str, side: str = "D") -> None:
        x, y = self.leave_pin(ref, pin)
        sx, sy = self._side_vec(side)
        d = 5.08 if side == "D" else 7.62
        if self.is_ic(ref):
            dx, dy = self.escape_dir(ref, pin)
            if dx * sx + dy * sy > 0.5:
                d = g(4)
        self.wire(x, y, x + sx * d, y + sy * d)
        self.gnd_at(x + sx * d, y + sy * d)

    def flag(self, net_x: float, net_y: float) -> None:
        self.pwr_n += 1
        ref = f"#FLG{self.pwr_n:02d}"
        self.place("power:PWR_FLAG", ref, "PWR_FLAG", net_x, net_y, 0)

    def emit_symbols(self) -> str:
        chunks = []
        for ref, p in self.parts.items():
            hide_ref = ref.startswith("#")
            px, py = p["x"], p["y"]
            pins = self.pin_lookup[p["lib"]]
            pin_lines = "\n".join(f'\t\t(pin "{n}" (uuid "{uid()}"))' for n in pins)
            if p["lib"].startswith("Module:"):
                rx, ry = px, py - 22.86
                vx, vy = px, py + 22.86
            else:
                rx, ry = px + 6.35, py - 8.89
                vx, vy = px + 6.35, py + 6.35
            chunks.append(
                f"""	(symbol
		(lib_id "{p['lib']}")
		(at {px:.2f} {py:.2f} {p['rot']})
		(unit 1)
		(exclude_from_sim no)
		(in_bom {"no" if hide_ref else "yes"})
		(on_board yes)
		(dnp no)
		(uuid "{uid()}")
		(property "Reference" "{ref}"
			(at {rx:.2f} {ry:.2f} 0)
			(effects (font (size 1.27 1.27)){" (hide yes)" if hide_ref else ""})
		)
		(property "Value" "{p['value']}"
			(at {vx:.2f} {vy:.2f} 0)
			(effects (font (size 1.27 1.27)){" (hide yes)" if hide_ref else ""})
		)
		(property "Footprint" ""
			(at {px:.2f} {py:.2f} 0)
			(effects (font (size 1.27 1.27)) (hide yes))
		)
		(property "Datasheet" "~"
			(at {px:.2f} {py:.2f} 0)
			(effects (font (size 1.27 1.27)) (hide yes))
		)
		(property "Description" ""
			(at {px:.2f} {py:.2f} 0)
			(effects (font (size 1.27 1.27)) (hide yes))
		)
{pin_lines}
		(instances
			(project "{PROJECT}"
				(path "{self.path}"
					(reference "{ref}")
					(unit 1)
				)
			)
		)
	)"""
            )
        return "\n".join(chunks)

    def write(self, path: Path, lib_blob: str) -> None:
        self.finalize_junctions()
        body = "\n".join(self.items)
        text = f"""(kicad_sch
	(version 20260306)
	(generator "eeschema")
	(generator_version "10.0")
	(uuid "{self.sheet_uuid}")
	(paper "{self.paper}")
	(title_block
		(title "{self.title}")
		(date "2026-10-04")
		(rev "1.5.3")
		(company "CallOnFail")
		(comment 1 "{self.comment}")
	)
	(lib_symbols
{lib_blob}
	)
{body}
{self.emit_symbols()}
	(sheet_instances
		(path "/"
			(page "1")
		)
	)
	(embedded_fonts no)
)
"""
        path.write_text(text, encoding="utf-8", newline="\n")


def lib_blob_for(ids: set[str], extracted: dict[str, str], box_text: dict[str, str]) -> str:
    parts = []
    for lib_id in sorted(ids):
        if lib_id in extracted:
            # extracted already includes wrapping (symbol "Lib:Name" ... )
            # strip one indent level mismatch by indenting with two tabs
            inner = extracted[lib_id]
            if inner.startswith("(symbol "):
                inner = "\t\t" + inner.replace("\n", "\n\t\t")
            parts.append(inner)
        elif lib_id in box_text:
            parts.append(box_text[lib_id])
        else:
            raise KeyError(lib_id)
    return "\n".join(parts)


def fill_lookups(
    extracted: dict[str, str], box_text: dict[str, str]
) -> dict[str, dict[str, tuple[float, float, float]]]:
    out = {}
    for lib_id, txt in extracted.items():
        out[lib_id] = parse_pins(txt)
    for lib_id, txt in box_text.items():
        out[lib_id] = parse_pins(txt)
    return out


def _conn_stub(s: Sch, ref: str, pin: str) -> tuple[float, float, int]:
    """Stub off a connector pin in escape direction (never through the body).

    Conn_01xN pins face left at rot=0 → stubs go left. Returns (ex, ey, label_rot).
    """
    x, y = s.pin(ref, pin)
    dx, dy = s.escape_dir(ref, pin)
    ex, ey = x + dx * ESCAPE, y + dy * ESCAPE
    s.wire(x, y, ex, ey)
    return ex, ey, s._dir_rot(dx, dy)


def wire_ab_link(s: Sch, j_pwr: str, j_sig: str) -> None:
    """Proto A↔B: bornera 5 V + Molex/bornera señales. Same pinout both ends."""
    gnd_pts = []
    for pin in ("1", "3"):
        ex, ey, _rot = _conn_stub(s, j_pwr, pin)
        gnd_pts.append((ex, ey))
    # Pin 4 was 5V_MODEM (Q3); now same rail as SYS (modem on Q1 cut).
    for pin, net in (("2", "5V_SYS"), ("4", "5V_SYS")):
        ex, ey, rot = _conn_stub(s, j_pwr, pin)
        s.glabel(net, ex, ey, rot)
    # Spine further off-body from the stubs (left when pins face left).
    pin_x = s.pin(j_pwr, "1")[0]
    dx_spine = -1.0 if gnd_pts[0][0] <= pin_x else 1.0
    gx = gnd_pts[0][0] + dx_spine * g(6)
    for ex, ey in gnd_pts:
        s.wire(ex, ey, gx, ey)
        s.junc(gx, ey)
    bot = max(ey for _x, ey in gnd_pts) + g(4)
    s.wire(gx, min(ey for _x, ey in gnd_pts), gx, bot)
    s.gnd_at(gx, bot)

    # Pin3/5 = TRIG_N (P12 SYS_KILL + panel BTN; same OD-OR net as TPL RSTn).
    for pin, net in (
        ("1", "GEL_ADC"),
        ("2", "IO15"),
        ("3", "TRIG_N"),
        ("4", "SPARE"),
        ("5", "TRIG_N"),
        ("6", "3V3"),
        ("7", "SPARE2"),
    ):
        ex, ey, rot = _conn_stub(s, j_sig, pin)
        s.glabel(net, ex, ey, rot)
    ex, ey, _rot = _conn_stub(s, j_sig, "8")
    s.wire(ex, ey, ex, ey + g(4))
    s.gnd_at(ex, ey + g(4))


def build_power(lookups) -> Sch:
    s = Sch(
        PWR_UUID,
        "CallOnFail v1.5.3 — Alimentacion",
        "Gel+XY-SJVA+XL6019+LM66200. Q1 nuclear via 74HCT123. TPL5010@BUS+NPN DONE. Sin Q3.",
        f"/{ROOT_UUID}/{PWR_UUID}",
        paper="A1",
    )
    s.pin_lookup = lookups

    s.text("CallOnFail v1.5.3 — alimentacion (proto)", g(20), g(16), 2.54)
    s.text(
        "Bloques: carga | backup+OR | Q1+74HCT123 | TPL5010@5V_BUS. Labels entre bloques.",
        g(20),
        g(22),
    )

    # --- Carga: 5V_PSU -> XY-SJVA -> fusible -> gel ---
    s.place("Connector_Generic:Conn_01x02", "J1", "PSU 5V 5A", g(32), g(56), 0)
    s.place("Module:XY_SJVA", "U1", "XY-SJVA 6.85V/0.6A", g(90), g(58), 0)
    s.place("Device:Fuse", "F1", "5A 5x20", g(148), g(54), 90)
    s.place("Device:Battery", "BT1", "Gel 6V 7Ah NP7-6", g(190), g(58), 0)
    s.join("J1", "1", "U1", "1")
    s.join("J1", "2", "U1", "2")
    s.gnd_pin("J1", "2", "D")
    gx, gy = s.pin("J1", "2")
    s.junc(gx, gy)
    s.place("power:PWR_FLAG", "#FLG_GND", "PWR_FLAG", gx - g(8), gy, 0)
    s.wire(gx, gy, gx - g(8), gy)
    jx, jy = s.pin("J1", "1")
    s.junc(jx, jy)
    s.place("power:PWR_FLAG", "#FLG_PSU", "PWR_FLAG", jx, jy - g(6), 0)
    s.wire(jx, jy, jx, jy - g(6))
    s.tap_net("J1", "1", "5V_PSU", "L", g(6))
    s.join("U1", "3", "F1", "1")
    s.join("F1", "2", "BT1", "1")
    s.join("U1", "4", "BT1", "2")
    s.gnd_pin("BT1", "2", "D")
    s.junc(*s.pin("BT1", "1"))
    s.tap_net("BT1", "1", "GEL_P", "U", g(8))
    s.text("CV 6,85 V / CC 0,6 A. Gel- solo a OUT-. No dos 6V en serie.", g(70), g(80))

    s.place("Device:R", "R6", "47k", g(232), g(56), 90)
    s.place("Device:R", "R7", "22k", g(256), g(72), 0)
    s.place("Device:C", "C3", "100n", g(272), g(72), 0)
    s.join("BT1", "1", "R6", "1")
    s.join("R6", "2", "R7", "1")
    s.join("R6", "2", "C3", "1")
    s.junc(*s.pin("R6", "2"))
    s.gnd_pin("R7", "2", "D")
    s.gnd_pin("C3", "2", "D")
    s.tap_net("R6", "2", "GEL_ADC", "R", g(10))
    s.text("~2,2 V @ 6,9 V   ~1,75 V @ 5,5 V  -> IO35", g(220), g(86))

    # --- Backup XL6019 + OR LM66200; EN via Q_EN (2N3904) ---
    # Device:R KiCad10: rot=0 vertical (pin1 N / pin2 S); rot=90 horizontal (pin1 W / pin2 E).
    s.place("Device:R", "R1", "10k", g(40), g(108), 0)       # vertical 5V_PSU → nodo
    s.place("Device:R", "R18", "100k", g(40), g(140), 0)      # vertical nodo → GND
    s.place("Device:R", "R21", "10k", g(64), g(124), 90)      # horizontal → base Q_EN
    s.place("Device:Q_NPN", "Q_EN", "2N3904", g(96), g(124), 0)
    s.place("Module:BUCKBOOST", "U2", "XL6019", g(160), g(120), 0)
    # R2 pull-up Gel+ → EN (no va a GND; Q_EN hunde EN). pin2 alineado con U2.EN
    s.place("Device:R", "R2", "10k", g(136), g(121), 0)       # vertical; pin2≈EN
    s.place("Module:LM66200", "U14", "LM66200", g(230), g(120), 0)
    s.place("Device:C_Polarized", "C1", "2200uF/16V", g(290), g(120), 0)
    s.place("Device:C", "C10", "100n", g(312), g(120), 0)
    s.tap_net("R1", "1", "5V_PSU", "U", g(6))
    s.join("R1", "2", "R18", "1")
    s.junc(*s.pin("R1", "2"))
    s.gnd_pin("R18", "2", "D")
    s.join("R1", "2", "R21", "1")
    s.join("R21", "2", "Q_EN", "B")
    s.gnd_pin("Q_EN", "E", "D")
    s.tap_net("U2", "1", "GEL_P", "L", g(6))
    s.tap_net("R2", "1", "GEL_P", "U", g(6))
    s.join("R2", "2", "U2", "3")
    s.junc(*s.pin("U2", "3"))
    s.join("U2", "3", "Q_EN", "C")
    s.gnd_pin("U2", "2", "L")
    s.gnd_pin("U2", "5", "D")  # VOUT-
    s.tap_net("U14", "1", "5V_PSU", "L", g(6))
    s.join("U2", "4", "U14", "2")
    s.gnd_pin("U14", "3", "D")
    s.gnd_pin("U14", "5", "L")
    s.nc(*s.pin("U14", "6"))
    s.join("U14", "4", "C1", "1")
    s.gnd_pin("C1", "2", "D")
    s.join("C1", "1", "C10", "1")
    s.gnd_pin("C10", "2", "D")
    c1p = s.pin("C1", "1")
    s.junc(*c1p)
    s.tap_net("C1", "1", "5V_BUS", "R", g(10))
    s.place("power:PWR_FLAG", "#FLG_BUS", "PWR_FLAG", c1p[0] + g(16), c1p[1], 0)
    s.wire(c1p[0], c1p[1], c1p[0] + g(16), c1p[1])
    s.text("OR: LM66200 (VIN1=PSU, VIN2=XL6019). R2=pull-up Gel+→EN; Q_EN hunde EN→GND si hay PSU.", g(32), g(178))
    s.text("Ajuste XL6019 5,1 V. No XL6009 / ZK-4KX. Sin HCT14 / D10.", g(32), g(182))

    # --- Nuclear: Q1 (izq) · 74HCT123 (centro) · TPL (der) ---
    # Flujo: TRIG_N → A1 | Q → HCT_Q → 1k → gate Q1.  A1 y Q NO se unen.
    s.text(
        "Nuclear: TRIG_N→A1 (flanco↓) · Q→HCT_Q→1k→Q1 (~2s OFF). A1≠Q.",
        g(32),
        g(196),
        1.50,
    )
    s.place("Device:Q_PMOS", "Q1", "NDP6020P", g(60), g(240), 180)
    s.place("Device:R", "R3", "100k", g(68), g(268), 0)  # gate → GND
    s.place("Device:R", "R19", "1k", g(100), g(240), 90)  # HCT_Q → gate
    s.join("Q1", "G", "R3", "1")
    s.join("Q1", "G", "R19", "1")
    s.junc(*s.pin("Q1", "G"))
    s.gnd_pin("R3", "2", "D")
    s.tap_net("Q1", "D", "5V_SYS", "D", g(6))
    s.tap_net("Q1", "S", "5V_BUS", "U", g(6))
    s.tap_net("R19", "2", "HCT_Q", "R", g(6))  # label: no cable largo al 123
    s.place("Switch:SW_Push", "SW1", "RESET gabinete", g(60), g(300), 0)
    s.tap_net("SW1", "1", "TRIG_N", "R", g(6))
    s.gnd_pin("SW1", "2", "D")
    s.text("Q1 idle ON. SW1→TRIG_N. LED PWR en hoja I/O.", g(32), g(320))

    # 74HCT123 — entradas izq / salidas der. Sin cables que crucen el cuerpo.
    s.place("Module:HCT123", "U4", "74HCT123", g(200), g(250), 0)
    a1x, a1y = s.pin("U4", "1")
    rcx, rcy = s.pin("U4", "7")
    clrx, clry = s.pin("U4", "3")

    # A1: stub corto a TRIG_N; PU R9 arriba del stub (mismo nodo, sin cruzar Q)
    trig_x = a1x - g(12)
    s.wire(a1x, a1y, trig_x, a1y)
    s.junc(trig_x, a1y)
    s.glabel("TRIG_N", trig_x - g(8), a1y, 180)
    s.wire(trig_x, a1y, trig_x - g(8), a1y)
    s.place("Device:R", "R9", "100k", trig_x, a1y - g(20), 0)
    r9b = s.pin("R9", "2")
    s.wire(r9b[0], r9b[1], trig_x, a1y)
    s.junc(trig_x, a1y)
    s.tap_net("R9", "1", "5V_BUS", "U", g(4))

    s.stub("U4", "2", "5V_BUS", "L", d=g(6))  # B1 = VCC (trig en A)
    s.gnd_pin("U4", "4", "L")

    # /CLR1 blank: RC pegado al pin (izq)
    s.place("Device:R", "R25", "100k", clrx - g(28), clry - g(14), 0)
    s.place("Device:C", "C18", "2.2uF", clrx - g(28), clry + g(14), 0)
    s.join_local("U4", "3", "R25", "2")
    s.join_local("U4", "3", "C18", "1")
    s.junc(clrx, clry)
    s.tap_net("R25", "1", "5V_BUS", "U", g(4))
    s.gnd_pin("C18", "2", "D")

    # Mitad 2 idle
    s.stub("U4", "8", "5V_BUS", "L", d=g(6))
    s.gnd_pin("U4", "9", "L")
    s.gnd_pin("U4", "10", "L")

    # Q → HCT_Q (salida; red distinta de TRIG_N)
    s.stub("U4", "5", "HCT_Q", "R", d=g(10))
    s.stub("U4", "6", "5V_BUS", "R", d=g(6))

    # RCx timing pegado a la derecha
    s.place("Device:R", "R4", "100k", rcx + g(24), rcy - g(14), 0)
    s.place("Device:C_Polarized", "C17", "47uF/16V", rcx + g(24), rcy + g(16), 0)
    s.join_local("U4", "7", "R4", "2")
    s.join_local("U4", "7", "C17", "1")
    s.junc(rcx, rcy)
    s.tap_net("R4", "1", "5V_BUS", "U", g(4))
    s.gnd_pin("C17", "2", "D")
    s.text(
        "A1=TRIG_N (PU 100k). Q=HCT_Q→R19→Q1. RCx=100k+47u ~2s. /CLR blank 100k+2.2u.",
        g(160),
        g(330),
    )

    # TPL5010 — top-right, clear of HCT123 / link headers
    s.text("TPL5010 @5V_BUS ~10 min. DONE←NPN←IO15. RSTn→TRIG_N.", g(360), g(40), 1.50)
    s.place("Module:TPL5010", "U3", "TPL5010", g(420), g(90), 0)
    s.place("Device:R", "R8", "57.6k", g(490), g(78), 0)
    s.place("Device:R", "R10", "10k", g(360), g(100), 0)
    s.place("Device:R", "R23", "1k", g(360), g(140), 90)
    s.place("Device:Q_NPN", "Q_DONE", "2N3904", g(390), g(140), 0)
    s.place("Device:C", "C9", "100n", g(380), g(60), 0)
    s.gnd_pin("C9", "2", "D")

    s.tap_net("U3", "1", "5V_BUS", "L", g(6))
    s.join("C9", "1", "U3", "1")  # decouple wired to VDD
    s.junc(*s.pin("U3", "1"))
    s.gnd_pin("U3", "2", "L")
    s.join("U3", "3", "R10", "2")
    s.join("U3", "3", "Q_DONE", "C")
    s.junc(*s.pin("U3", "3"))
    s.tap_net("R10", "1", "5V_BUS", "U", g(4))
    s.gnd_pin("Q_DONE", "E", "D")
    s.join("R23", "2", "Q_DONE", "B")
    s.tap_net("R23", "1", "IO15", "L", g(6))
    s.nc(*s.pin("U3", "4"))
    s.join("U3", "6", "R8", "1")
    s.gnd_pin("R8", "2", "D")
    s.tap_net("U3", "5", "TRIG_N", "R", g(8))
    s.junc(*s.pin("U3", "5"))
    s.text(
        "DONE OD: IO15 alto→DONE bajo; pateo IO15 bajo. REXT 57.6k≈10 min. /CLR blank 100k+2.2u.",
        g(360),
        g(180),
    )

    # Link headers — bottom-right, below nuclear (no overlap with TPL)
    s.place("Connector_Generic:Conn_01x04", "J7", "Enlace 5V A", g(420), g(360), 0)
    s.place("Connector_Generic:Conn_01x08", "J9", "Enlace sig A", g(490), g(360), 0)
    wire_ab_link(s, "J7", "J9")
    s.text(
        "Proto A↔B: J7 bornera 5,08 (GND 5V_SYS GND 5V_SYS) + J9 Molex. Modem=SYS.",
        g(300),
        g(420),
    )
    s.text(
        "J9: GEL_ADC IO15 TRIG_N SPARE TRIG_N 3V3 SPARE2 GND.",
        g(300),
        g(426),
    )
    return s


def build_io(lookups) -> Sch:
    s = Sch(
        IO_UUID,
        "CallOnFail v1.5.3 — ESP32 / modem / sensores / campo",
        "WT32+FT232+A7672. PCF8575@3V3. PC817+water+relay2+RS485. EN=lab. P12=SYS_KILL. No 5V USB.",
        f"/{ROOT_UUID}/{IO_UUID}",
        paper="A1",
    )
    s.pin_lookup = lookups

    s.text("CallOnFail v1.5.3 — WT32, FT232, A7672, PCF8575, campo", g(20), g(16), 2.54)
    s.text(
        "FT232/BOOT/EN | WT32 | modem  —  I2C/sensores  —  PCF / campo / panel / RS485.",
        g(20),
        g(22),
    )

    # --- MCU row: FT232 (rot180) | WT32 | modem ---
    s.place("Module:WT32", "U5", "WT32-ETH01", g(120), g(80), 0)
    # Align FT232 TXD/RXD with WT32 RXD0/TXD0 (pins 5/4), then short parallel hops.
    rxd0_y = s.pin("U5", "5")[1]
    s.place("Module:FT232", "U15", "FT232", g(40), rxd0_y, 180)
    s.place("Module:A7672", "U6", "A7672SA-FASE", g(260), g(72), 0)
    s.place("Switch:SW_Push", "SW_BOOT", "BOOT IO0", g(100), g(130), 0)
    s.place("Switch:SW_Push", "SW_EN", "EN reset", g(130), g(130), 0)

    s.tap_net("U5", "1", "5V_SYS", "L", g(6))
    s.gnd_pin("U5", "2", "L")
    # 3V3 stub + PWR_FLAG on the same endpoint (mismo d, si no el flag queda flotando)
    d3 = g(6)
    s.stub("U5", "3", "3V3", "L", d=d3)
    px, py = s.pin("U5", "3")
    dx, dy = s.escape_dir("U5", "3")
    tx, ty = px + dx * d3, py + dy * d3
    s.junc(tx, ty)
    s.place("power:PWR_FLAG", "#FLG_3V3", "PWR_FLAG", tx, ty - g(6), 0)
    s.wire(tx, ty, tx, ty - g(6))

    s.gnd_pin("U15", "1", "D")
    mid_uart = g(80)
    s.join_via_x("U15", "2", "U5", "5", mid_uart)  # FT232 TXD → WT32 RXD0
    s.join_via_x("U15", "3", "U5", "4", mid_uart)  # FT232 RXD ← WT32 TXD0
    s.join("SW_BOOT", "1", "U5", "6", net="IO0_BOOT")
    s.gnd_pin("SW_BOOT", "2", "D")
    s.join("SW_EN", "1", "U5", "7", net="EN_SW")
    s.gnd_pin("SW_EN", "2", "D")
    s.text("FT232 rot180: TXD→RXD0, RXD←TXD0 (cables cortos). No 5V USB. EN=lab.", g(20), g(148))

    # Decouple WT32 5V — beside IC, wired to pin + 5V_SYS label only on rail
    s.place("Device:C", "C11", "100n", g(95), g(44), 0)
    s.place("Device:C_Polarized", "C15", "10uF", g(115), g(44), 0)
    s.join("C11", "1", "U5", "1")
    s.join("C15", "1", "U5", "1")
    s.junc(*s.pin("U5", "1"))
    s.gnd_pin("C11", "2", "D")
    s.gnd_pin("C15", "2", "D")

    # Modem VCC: private column (short stub). Never share X with UART labels.
    s.gnd_pin("U6", "2", "L")
    vccx, vccy = s.pin("U6", "1")
    pwr_x = vccx - g(4)
    s.wire(vccx, vccy, pwr_x, vccy)
    s.junc(pwr_x, vccy)
    s.glabel("5V_SYS", pwr_x - g(8), vccy, 180)
    s.wire(pwr_x, vccy, pwr_x - g(8), vccy)
    s.place("Device:C_Polarized", "C2", "1000uF/16V", pwr_x, vccy + g(22), 0)
    s.place("Device:C", "C12", "100n", pwr_x + g(14), vccy + g(22), 0)
    for cref in ("C2", "C12"):
        cx, cy = s.pin(cref, "1")
        s.wire(cx, cy, pwr_x, cy)
        s.junc(pwr_x, cy)
        s.gnd_pin(cref, "2", "D")
    s.wire(pwr_x, vccy, pwr_x, s.pin("C2", "1")[1])
    # UART: labels only (long stubs on modem — distinct X from pwr_x)
    s.tap_net("U5", "10", "MODEM_RX", "R", g(6))
    s.tap_net("U5", "14", "MODEM_TX", "R", g(6))
    s.stub("U6", "3", "MODEM_RX", "L", d=g(14))
    s.stub("U6", "4", "MODEM_TX", "L", d=g(14))
    io4 = s.pin("U5", "9")
    io2 = s.pin("U5", "8")
    # 1k series beside WT32; hop al módem por label (no cable por columna VCC)
    s.place("Device:R", "R32", "1k", io4[0] + g(16), io4[1], 90)
    s.place("Device:R", "R33", "1k", io2[0] + g(16), io2[1], 90)
    s.join_local("U5", "9", "R32", "1")
    s.join_local("U5", "8", "R33", "1")
    s.tap_net("R32", "2", "MODEM_RST", "R", g(4))
    s.tap_net("R33", "2", "MODEM_PWRKEY", "R", g(4))
    s.stub("U6", "5", "MODEM_RST", "L", d=g(14))
    s.stub("U6", "6", "MODEM_PWRKEY", "L", d=g(14))
    s.nc(*s.pin("U6", "7"))
    s.text("IO4/IO2 via 1k. Modem VCC=5V_SYS (nuclear Q1). UART/RST=labels.", g(200), g(118))

    s.tap_net("U5", "13", "IO15", "R", g(6))
    s.tap_net("U5", "17", "GEL_ADC", "R", g(6))
    s.text("IO15→J10.2→Q_DONE. P12/BTN→TRIG_N. Labels entre bloques (no cables largos).", g(120), g(148))

    # I2C bus from IO32/IO33
    sda_x, sda_y = s.leave_pin("U5", "15")
    scl_x, scl_y = s.leave_pin("U5", "16")
    bus_sda = sda_x + g(8)
    bus_scl = scl_x + g(14)
    s.wire(sda_x, sda_y, bus_sda, sda_y)
    s.wire(scl_x, scl_y, bus_scl, scl_y)
    s.junc(bus_sda, sda_y)
    s.junc(bus_scl, scl_y)

    s.place("Device:R", "R13", "4k7", bus_sda, sda_y - g(20), 0)
    s.place("Device:R", "R14", "4k7", bus_scl, scl_y - g(20), 0)
    s.tap_net("R13", "1", "3V3", "U", g(4))
    s.tap_net("R14", "1", "3V3", "U", g(4))
    r13b = s.pin("R13", "2")
    r14b = s.pin("R14", "2")
    s.wire(r13b[0], r13b[1], bus_sda, sda_y)
    s.wire(r14b[0], r14b[1], bus_scl, scl_y)
    s.junc(bus_sda, sda_y)
    s.junc(bus_scl, scl_y)

    # Sensors under WT32 (own column — no overlap with modem/campo)
    s.place("Module:OLED", "U7", "OLED 0x3C", g(80), g(200), 0)
    s.place("Module:SHT31", "U8", "SHT31 0x44", g(80), g(246), 0)
    s.place("Module:PCF8575", "U11", "PCF8575", g(220), g(230), 0)
    s.gnd_pin("U7", "2", "L")
    s.gnd_pin("U8", "2", "L")
    s.tap_net("U11", "1", "3V3", "L", g(6))
    s.gnd_pin("U11", "2", "L")
    s.gnd_rail("U11", ["5", "6", "7"], "L")
    # Decouple + PUs glued to PCF pins (wire only; supply end = 3V3 label)
    s.place("Device:C", "C13", "100n", g(180), g(200), 0)
    s.join("C13", "1", "U11", "1")
    s.junc(*s.pin("U11", "1"))
    s.gnd_pin("C13", "2", "D")
    # PUs: X alternado (no apilar). 3V3 sale a la DERECHA del R (no arriba:
    # un stub vertical caería sobre el cable horizontal del pin vecino).
    for i, (ref, pin) in enumerate(
        (("R27", "8"), ("R28", "9"), ("R29", "10"), ("R30", "19"))
    ):
        px, py = s.pin("U11", pin)
        s.place("Device:R", ref, "10k", px + g(18) + (i % 2) * g(14), py, 90)
        s.join(ref, "1", "U11", pin)
        s.tap_net(ref, "2", "3V3", "R", g(6))

    for ref, sda_pin, scl_pin in (("U7", "3", "4"), ("U8", "3", "4")):
        s.tap_x(ref, sda_pin, bus_sda)
        s.tap_x(ref, scl_pin, bus_scl)
    i2c_sda_ys = [sda_y, s.pin("U7", "3")[1], s.pin("U8", "3")[1]]
    i2c_scl_ys = [scl_y, s.pin("U7", "4")[1], s.pin("U8", "4")[1]]
    s.wire(bus_sda, min(i2c_sda_ys), bus_sda, max(i2c_sda_ys))
    s.wire(bus_scl, min(i2c_scl_ys), bus_scl, max(i2c_scl_ys))
    s.glabel("I2C_SDA", bus_sda, min(i2c_sda_ys), 90)
    s.glabel("I2C_SCL", bus_scl, min(i2c_scl_ys), 90)
    s.tap_net("U11", "3", "I2C_SDA", "L", g(6))
    s.tap_net("U11", "4", "I2C_SCL", "L", g(6))
    s.text("I2C IO32/IO33. PCF8575 VCC=3V3 dir 0x20. A0-A2 GND.", g(20), g(280))

    s.place("Module:DS18B20", "U9", "DS18B20 x4", g(80), g(292), 0)
    s.place("Device:R", "R15", "4k7", g(40), g(292), 0)
    s.gnd_pin("U9", "2", "L")
    s.tap_net("R15", "1", "3V3", "U", g(4))
    s.join("U9", "3", "R15", "2")  # PU wired to sensor
    s.junc(*s.pin("R15", "2"))
    s.tap_net("R15", "2", "OW_DATA", "R", g(6))  # only the long hop to WT32
    s.tap_net("U5", "12", "OW_DATA", "R", g(6))
    v3_x = g(28)
    s.vspine(v3_x, [s.pin("U7", "1"), s.pin("U8", "1"), s.pin("U9", "1")])
    s.glabel("3V3", v3_x, s.pin("U7", "1")[1], 180)
    s.text("1-Wire cadena, no estrella. UTP DATA+GND.", g(20), g(316))

    # ZMPT rot180: OUT mira al divisor (derecha). Labels a WT32.
    s.place("Module:ZMPT", "U10", "ZMPT101B", g(200), g(330), 180)
    out_x, out_y = s.pin("U10", "3")
    s.place("Device:R", "R34", "10k", out_x + g(16), out_y, 90)
    s.place("Device:R", "R35", "15k", out_x + g(16), out_y + g(14), 0)
    s.place("Device:C", "C14", "100n", out_x + g(28), out_y + g(14), 0)
    s.tap_net("U10", "1", "5V_SYS", "R", g(6))
    s.gnd_pin("U10", "2", "R")
    s.join("U10", "3", "R34", "1")
    s.join("R34", "2", "R35", "1")
    s.join("R34", "2", "C14", "1")
    s.junc(*s.pin("R34", "2"))
    s.gnd_pin("R35", "2", "D")
    s.gnd_pin("C14", "2", "D")
    s.tap_net("R34", "2", "IO36", "R", g(6))
    s.tap_net("U5", "18", "IO36", "R", g(6))
    s.nc(*s.pin("U10", "4"))
    s.text("ZMPT OUT→10k→IO36; 15k+100n GND. No 220 hasta validar.", g(160), g(370))

    s.place("Module:RS485", "U16", "RS485 auto", g(160), g(390), 0)
    s.place("Connector_Generic:Conn_01x02", "J11", "RS485 A/B", g(230), g(390), 0)
    s.tap_net("U16", "1", "3V3", "L", g(6))
    s.gnd_pin("U16", "2", "L")
    s.tap_net("U5", "11", "RS485_TX", "R", g(6))
    s.tap_net("U5", "19", "RS485_RX", "R", g(6))
    s.tap_net("U16", "3", "RS485_TX", "L", g(6))
    s.tap_net("U16", "4", "RS485_RX", "L", g(6))
    s.join("U16", "5", "J11", "1", net="RS485_A")
    s.join("U16", "6", "J11", "2", net="RS485_B")
    s.text("RS485: IO12 TX, IO39 RX (labels). Servicio = PCF P02.", g(140), g(420))

    # --- Campo ---
    # Water: contacto seco → P00 directo (NO usa PC817). PC817 = solo IN1+IN2.
    s.place("Module:WATER_DET", "U17", "Water det", g(360), g(50), 0)
    s.place("Module:PC817_2CH", "U12", "PC817 2CH", g(360), g(130), 0)
    s.place("Connector_Generic:Conn_01x02", "J3", "Campo IN1", g(430), g(100), 0)
    s.place("Connector_Generic:Conn_01x02", "J4", "Campo IN2", g(430), g(140), 0)
    s.place("Module:RELAY2_OPTO", "K1", "2-relay opto", g(360), g(210), 0)
    s.place("Connector_Generic:Conn_01x04", "J5", "Campo OUT", g(450), g(210), 0)
    s.place("Module:BUZZER", "LS1", "Buzzer 5V activo", g(400), g(290), 0)
    s.place("Device:Q_NPN", "Q_BZ", "2N3904", g(360), g(290), 0)
    s.place("Device:R", "R36", "1k", g(330), g(290), 90)
    s.place("Switch:SW_Push", "SW2", "Servicio P02", g(300), g(250), 0)

    s.tap_net("U17", "1", "5V_SYS", "L", g(6))
    s.gnd_pin("U17", "2", "L")
    s.nc(*s.pin("U17", "3"))
    s.nc(*s.pin("U17", "6"))
    # Water NO → P00, COM → GND (dry contact; PU R27 en PCF)
    s.tap_net("U17", "4", "PCF_P00", "R", g(6))
    s.tap_net("U11", "8", "PCF_P00", "R", g(6))
    s.gnd_pin("U17", "5", "D")

    # PC817: ch1=IN1→P01, ch2=IN2→P13 (ambos canales para campo)
    s.join("J3", "1", "U12", "1", net="IN1")
    s.join("J3", "2", "U12", "3", net="OPTO_COM")
    s.join("J4", "1", "U12", "2", net="IN2")
    s.join("J4", "2", "U12", "3", net="OPTO_COM")
    # OUT/VCC all exit right — stagger stub lengths so labels never share a spine.
    s.stub("U12", "4", "PCF_P01", "R", d=g(10))
    s.stub("U12", "5", "PCF_P13", "R", d=g(14))
    s.stub("U12", "6", "3V3", "R", d=g(18))
    s.gnd_pin("U12", "7", "R")
    s.tap_net("U11", "9", "PCF_P01", "R", g(6))
    s.tap_net("U11", "19", "PCF_P13", "R", g(6))

    # Servicio P02: botón lab/mantenimiento (NO es RESET, NO silencia alarmas)
    s.tap_net("U11", "10", "PCF_P02", "R", g(6))
    s.tap_net("SW2", "1", "PCF_P02", "U", g(6))
    s.gnd_pin("SW2", "2", "D")
    s.text("Servicio P02: input lab/mantenimiento. No RESET. No silencia alarmas.", g(280), g(270))

    s.tap_net("U11", "11", "PCF_P03", "R", g(6))
    s.tap_net("R36", "1", "PCF_P03", "L", g(6))
    s.join("R36", "2", "Q_BZ", "B")
    s.gnd_pin("Q_BZ", "E", "D")
    s.join("Q_BZ", "C", "LS1", "2")
    s.tap_net("LS1", "1", "5V_SYS", "L", g(6))

    s.tap_net("K1", "1", "3V3", "L", g(6))
    s.tap_net("K1", "2", "5V_SYS", "L", g(6))
    s.gnd_pin("K1", "3", "L")
    s.tap_net("U11", "12", "PCF_P04", "R", g(6))
    s.tap_net("U11", "13", "PCF_P05", "R", g(6))
    s.tap_net("K1", "4", "PCF_P04", "L", g(6))
    s.tap_net("K1", "5", "PCF_P05", "L", g(6))
    s.join("K1", "6", "J5", "1", net="FIELD_COM1")
    s.join("K1", "7", "J5", "2", net="OUT1")
    s.join("K1", "8", "J5", "3", net="FIELD_COM2")
    s.join("K1", "9", "J5", "4", net="OUT2")
    s.text("jumper VCC-JD-VCC OFF", g(350), g(240))
    s.text("Water→P00 directo. PC817=IN1→P01 + IN2→P13. Buzzer P03→NPN.", g(300), g(320))

    p11 = s.pin("U11", "17")
    s.place("Device:D", "D8", "1N4148", p11[0] + g(20), p11[1], 0)
    s.join("U11", "17", "D8", "1")
    s.tap_net("D8", "2", "PHY_NRST", "R", g(8))
    s.text("ETH_RST: 1N4148→PHY nRST (R43). P12→TRIG_N.", g(220), g(340))

    s.tap_net("U11", "18", "TRIG_N", "R", g(8))
    s.nc(*s.pin("U11", "20"))
    s.nc(*s.pin("U11", "21"))
    s.nc(*s.pin("U11", "22"))
    s.nc(*s.pin("U11", "23"))
    s.text("P14-P17 NC. P13=IN2 (opto).", g(220), g(348))

    # Panel LEDs — una fila por riel (5V_SYS ≠ 3V3). LED rot180: A← K→.
    # Device:LED pin1=K@left(rot0) → rot180 pone A a la izquierda. J6 a la derecha.
    s.place("Connector_Generic:Conn_01x06", "J6", "Panel 6 pin", g(600), g(80), 0)

    def _led_row(y, rref, dref, dval, rail, jpin, cath_net=None, to_gnd=False, panel_anode=False):
        s.place("Device:R", rref, "330R", g(500), y, 90)
        s.place("Device:LED", dref, dval, g(535), y, 180)
        r1x, r1y = s.pin(rref, "1")
        s.wire(r1x - g(10), r1y, r1x, r1y)
        s.glabel(rail, r1x - g(10), r1y, 180)
        # R.2 → LED.A (pin2), junction explícita
        r2x, r2y = s.pin(rref, "2")
        ax, ay = s.pin(dref, "2")
        s.wire(r2x, r2y, ax, ay)
        s.junc(r2x, r2y)
        s.junc(ax, ay)
        kx, ky = s.pin(dref, "1")  # K
        if to_gnd:
            s.wire(kx, ky, kx + g(8), ky)
            s.gnd_at(kx + g(8), ky)
        else:
            s.wire(kx, ky, kx + g(10), ky)
            s.glabel(cath_net, kx + g(10), ky, 0)
        if panel_anode:
            # Panel ánodo = mismo nodo que LED.A — label, no cable largo a J6
            s.wire(ax, ay, ax, ay - g(6))
            s.glabel("LED_PWR", ax, ay - g(6), 90)
            s.tap_net("J6", jpin, "LED_PWR", "L", g(6))
        else:
            s.tap_net("J6", jpin, cath_net, "L", g(6))

    _led_row(g(40), "R5", "D2", "LED GRN PWR", "5V_SYS", "2", to_gnd=True, panel_anode=True)
    _led_row(g(60), "R16", "D6", "LED NET", "3V3", "3", cath_net="PCF_P06")
    _led_row(g(80), "R17", "D7", "LED ALARMA", "3V3", "4", cath_net="PCF_P07")
    _led_row(g(100), "R26", "D9", "LED LTE", "3V3", "5", cath_net="PCF_P10")
    s.tap_net("U11", "14", "PCF_P06", "R", g(6))
    s.tap_net("U11", "15", "PCF_P07", "R", g(6))
    s.tap_net("U11", "16", "PCF_P10", "R", g(6))
    s.gnd_pin("J6", "1", "L")
    s.tap_net("J6", "6", "TRIG_N", "L", g(6))
    s.text(
        "Panel: PWR=5V_SYS→330→LED→GND (J6.2=LED_PWR). NET/ALARM/LTE=3V3→330→LED→PCF (J6=cátodo). Sin mezclar rieles.",
        g(480),
        g(128),
    )

    s.place("Connector_Generic:Conn_01x04", "J8", "Enlace 5V B", g(480), g(450), 0)
    s.place("Connector_Generic:Conn_01x08", "J10", "Enlace sig B", g(540), g(450), 0)
    wire_ab_link(s, "J8", "J10")
    s.text(
        "Mismo pinout que J7/J9. Cable 0,75 mm2 en 5 V. Molex polarizado o bornera, no IDC.",
        g(360),
        g(490),
    )
    s.text(
        "GPIO: 0 BOOT  2 PWRKEY  4 RESET  5/17 modem  12/39 RS485  14 1-Wire  15 DONE  32/33 I2C  35 gel  36 ZMPT  EN=lab  P12=TRIG_N↓",
        g(20),
        g(490),
    )
    return s


def build_root() -> str:
    return f"""(kicad_sch
	(version 20260306)
	(generator "eeschema")
	(generator_version "10.0")
	(uuid "{ROOT_UUID}")
	(paper "A3")
	(title_block
		(title "CallOnFail v1.5.3")
		(date "2026-10-04")
		(rev "1.5.3")
		(company "CallOnFail")
		(comment 1 "WT32 + A7672 + gel 6V + TPL5010 + 74HCT123 + PCF8575. Ver docs/HARDWARE_V1.md")
	)
	(lib_symbols
	)
	(text "CallOnFail hardware v1.5.3"
		(exclude_from_sim no)
		(at 50.80 30.48 0)
		(effects (font (size 3.81 3.81)) (justify left bottom))
		(uuid "{uid()}")
	)
	(text "Dos hojas: A alimentacion+TPL+123 + B I/O. Enlace proto bornera/Molex. Luego una placa."
		(exclude_from_sim no)
		(at 50.80 40.64 0)
		(effects (font (size 1.27 1.27)) (justify left bottom))
		(uuid "{uid()}")
	)
	(text "5V_PSU  5V_BUS  5V_SYS  3V3  GEL_P  GEL_ADC  IO15  TRIG_N  HCT_Q  PHY_NRST"
		(exclude_from_sim no)
		(at 50.80 48.26 0)
		(effects (font (size 1.27 1.27)) (justify left bottom))
		(uuid "{uid()}")
	)
	(text "Rev 1.5.3: sin Q3; modem=SYS; TPL@BUS+NPN; 74HCT123~2s→Q1; P12 SYS_KILL↓; EN=lab."
		(exclude_from_sim no)
		(at 50.80 55.88 0)
		(effects (font (size 1.27 1.27)) (justify left bottom))
		(uuid "{uid()}")
	)
	(sheet
		(at 50.80 76.20)
		(size 71.12 40.64)
		(exclude_from_sim no)
		(in_bom yes)
		(on_board yes)
		(dnp no)
		(stroke (width 0.1524) (type solid))
		(fill (color 0 0 0 0.0000))
		(uuid "{PWR_UUID}")
		(property "Sheetname" "Alimentacion"
			(at 50.80 75.00 0)
			(effects (font (size 1.27 1.27)) (justify left bottom))
		)
		(property "Sheetfile" "01-alimentacion.kicad_sch"
			(at 50.80 117.50 0)
			(effects (font (size 1.27 1.27)) (justify left top))
		)
	)
	(sheet
		(at 160.02 76.20)
		(size 81.28 40.64)
		(exclude_from_sim no)
		(in_bom yes)
		(on_board yes)
		(dnp no)
		(stroke (width 0.1524) (type solid))
		(fill (color 0 0 0 0.0000))
		(uuid "{IO_UUID}")
		(property "Sheetname" "ESP32 modem sensores"
			(at 160.02 75.00 0)
			(effects (font (size 1.27 1.27)) (justify left bottom))
		)
		(property "Sheetfile" "02-io.kicad_sch"
			(at 160.02 117.50 0)
			(effects (font (size 1.27 1.27)) (justify left top))
		)
	)
	(sheet_instances
		(path "/"
			(page "1")
		)
	)
	(embedded_fonts no)
)
"""


def write_pro() -> None:
    if not KICAD_PRO_TEMPLATE.exists():
        print("skip .kicad_pro (no KiCad template on this machine)")
        return
    text = KICAD_PRO_TEMPLATE.read_text(encoding="utf-8")
    text = text.replace('"filename": "kicad.kicad_pro"', f'"filename": "{PROJECT}.kicad_pro"')
    text = text.replace(
        '"sheets": []',
        f'''"sheets": [
    [
      "{ROOT_UUID}",
      "Root"
    ],
    [
      "{PWR_UUID}",
      "Alimentacion"
    ],
    [
      "{IO_UUID}",
      "ESP32 modem sensores"
    ]
  ]''',
    )
    (OUT / f"{PROJECT}.kicad_pro").write_text(text, encoding="utf-8", newline="\n")


def write_bom(sheets: list[Sch]) -> None:
    """Grouped BOM of real parts (no GND / PWR_FLAG)."""
    rows: dict[tuple[str, str], list[str]] = {}
    for sheet in sheets:
        for ref, p in sheet.parts.items():
            if ref.startswith("#") or p["lib"].startswith("power:"):
                continue
            key = (p["value"], p["lib"])
            rows.setdefault(key, []).append(ref)
    lines = ["Qty,Value,Refs,Lib"]
    for (value, lib) in sorted(rows, key=lambda k: (k[1], k[0])):
        refs = sorted(rows[(value, lib)], key=lambda r: (re.sub(r"\d+", "", r), int(re.sub(r"\D+", "", r) or "0")))
        val = value.replace('"', '""')
        lines.append(f'{len(refs)},"{val}","{" ".join(refs)}",{lib}')
    path = OUT / f"{PROJECT}-bom.csv"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("wrote", path)


def main() -> None:
    extracted = {}
    for lib_id, (fname, name) in EXTRACT.items():
        raw = extract_symbol(fname, name)
        # extract_symbol used filename[:-10] which is wrong for some paths
        # force correct lib prefix
        lib = lib_id.split(":")[0]
        # raw starts with (symbol "Device:R" or "Device.kicad:R" if bug
        raw = re.sub(r'^\(symbol "[^"]+"', f'(symbol "{lib_id}"', raw, count=1)
        extracted[lib_id] = raw

    box_text = {}
    for lib_id, (value, left, right) in BOXES.items():
        box_text[lib_id] = make_box(lib_id, value, left, right)

    lookups = fill_lookups(extracted, box_text)

    pwr = build_power(lookups)
    io = build_io(lookups)

    pwr.write(OUT / "01-alimentacion.kicad_sch", lib_blob_for(pwr.lib_ids, extracted, box_text))
    io.write(OUT / "02-io.kicad_sch", lib_blob_for(io.lib_ids, extracted, box_text))
    (OUT / f"{PROJECT}.kicad_sch").write_text(build_root(), encoding="utf-8", newline="\n")
    write_pro()
    write_bom([pwr, io])
    print("wrote", OUT)


if __name__ == "__main__":
    main()
