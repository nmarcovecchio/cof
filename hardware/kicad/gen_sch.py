#!/usr/bin/env python3
"""Generate CallOnFail v1 KiCad schematics (KiCad 10).

Two hierarchical sheets:
  01-alimentacion  — 5V_PSU, XY-SJVA, gel 6 V, buck-boost, WDT, high-side
  02-io            — WT32, A7672, sensores, PCF, frente

Layout rules (KiCad docs + readable-schematic practice):
  - 50 mil / 1.27 mm grid for symbols, pins and wires
  - Flow left→right (inputs, processing, outputs); power source→load
  - Parts that work together sit together; whitespace between blocks
  - Wires inside a block; labels if a wire would cross another block
  - Never run a wire along a symbol pin column; T-junctions only
  - GND down, supplies up
  - Never route a wire across an IC / module body
  - Leave an IC pin straight (then bend); do not turn on top of the pin number

Matches docs/HARDWARE_V1.md (not the old 12 V / ZK-S4 / XL4015 design).
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
    "Transistor_FET:2N7000": ("Transistor_FET.kicad_sym", "2N7000"),
    "Timer:CD4541BE": ("Timer.kicad_sym", "CD4541BE"),
    "Timer:NE555D": ("Timer.kicad_sym", "NE555D"),
    "Switch:SW_Push": ("Switch.kicad_sym", "SW_Push"),
    "Connector_Generic:Conn_01x02": ("Connector_Generic.kicad_sym", "Conn_01x02"),
    "Connector_Generic:Conn_01x04": ("Connector_Generic.kicad_sym", "Conn_01x04"),
    "Connector_Generic:Conn_01x05": ("Connector_Generic.kicad_sym", "Conn_01x05"),
    "Connector_Generic:Conn_01x06": ("Connector_Generic.kicad_sym", "Conn_01x06"),
    "Transistor_Array:ULN2003": ("Transistor_Array.kicad_sym", "ULN2003"),
    "power:GND": ("power.kicad_sym", "GND"),
    "power:PWR_FLAG": ("power.kicad_sym", "PWR_FLAG"),
}


def g(n: int | float) -> float:
    """50 mil KiCad connection grid."""
    return round(float(n) * 1.27, 2)


ESCAPE = g(8)  # 10.16 mm past the pin, then the first bend


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
        [("1", "5V"), ("2", "GND"), ("3", "3V3"), ("4", "TXD0"), ("5", "RXD0"), ("6", "IO0")],
        [
            ("7", "IO2"),
            ("8", "IO4"),
            ("9", "IO5"),
            ("10", "IO14"),
            ("11", "IO15"),
            ("12", "IO17"),
            ("13", "IO32"),
            ("14", "IO33"),
            ("15", "IO35"),
            ("16", "IO36"),
            ("17", "IO39"),
        ],
    ),
    "Module:A7672": (
        "A7672SA-FASE",
        [("1", "VCC"), ("2", "GND"), ("3", "TXD"), ("4", "RXD"), ("5", "RESET"), ("6", "PWRKEY")],
        [("7", "ANT")],
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
    "Module:PCF8574": (
        "PCF8574 0x20",
        [("1", "VCC"), ("2", "GND"), ("3", "SDA"), ("4", "SCL"), ("5", "A0"), ("6", "A1"), ("7", "A2")],
        [
            ("8", "P0"),
            ("9", "P1"),
            ("10", "P2"),
            ("11", "P3"),
            ("12", "P4"),
            ("13", "P5"),
            ("14", "P6"),
            ("15", "P7"),
        ],
    ),
    "Module:ZMPT": (
        "ZMPT101B",
        [("1", "OUT"), ("2", "GND")],
        [("3", "VAC")],
    ),
    "Module:DS18B20": (
        "DS18B20 x4 bus",
        [("1", "VDD"), ("2", "GND"), ("3", "DATA")],
        [],
    ),
    "Module:RELAY": (
        "Relay 5V",
        [("1", "COIL+"), ("2", "COIL-")],
        [("3", "COM"), ("4", "NO")],
    ),
    "Module:BUZZER": (
        "Buzzer 5V activo",
        [("1", "+"), ("2", "-")],
        [],
    ),
    "Module:IDC10": (
        "IDC-10 A↔B",
        [],
        [
            ("1", "GND"),
            ("2", "5V_SYS"),
            ("3", "GND"),
            ("4", "5V_MODEM"),
            ("5", "GEL_ADC"),
            ("6", "IO15"),
            ("7", "MODEM_CUT"),
            ("8", "LED_PWR"),
            ("9", "BTN_RESET"),
            ("10", "GND"),
        ],
    ),
    "Module:AHCTBUF": (
        "74HCT125",
        [("1", "1OE"), ("2", "1A"), ("7", "GND"), ("4", "2A"), ("9", "3A"), ("12", "4A")],
        [("3", "1Y"), ("14", "VCC"), ("6", "2OE"), ("10", "3OE"), ("13", "4OE")],
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

    def use(self, lib_id: str) -> None:
        self.lib_ids.add(lib_id)

    def place(self, lib_id: str, ref: str, value: str, x: float, y: float, rot: int = 0) -> None:
        self.use(lib_id)
        self.parts[ref] = {"lib": lib_id, "value": value, "x": x, "y": y, "rot": rot}

    def is_ic(self, ref: str) -> bool:
        lib = self.parts[ref]["lib"]
        return lib.startswith(("Module:", "Timer:", "Transistor_Array:"))

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
            if not self.is_ic(ref):
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
            if not self.is_ic(ref):
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
            if not self.is_ic(ref):
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
            if not self.is_ic(ref):
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

    def route(self, x1: float, y1: float, x2: float, y2: float) -> None:
        """Short manhattan route that does not cross IC / module bodies."""
        if abs(x1 - x2) < 0.02 and abs(y1 - y2) < 0.02:
            return
        path = self._best_path(x1, y1, x2, y2)
        if path is not None:
            self._draw_path(path)
            return
        # Last resort: walk around the hull of every IC between the two points.
        xs = [x1, x2]
        ys = [y1, y2]
        for ref in self.parts:
            if not self.is_ic(ref):
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
            return
        bot = self._grid(max(ys) + g(3))
        hull = [(x1, y1), (x1, bot), (x2, bot), (x2, y2)]
        self._draw_path(hull)

    def wire(self, x1: float, y1: float, x2: float, y2: float) -> None:
        self.items.append(
            f"""	(wire
		(pts (xy {x1:.2f} {y1:.2f}) (xy {x2:.2f} {y2:.2f}))
		(stroke (width 0) (type default))
		(uuid "{uid()}")
	)"""
        )

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

    def join(self, ra: str, pa: str, rb: str, pb: str, mid: float | None = None) -> None:
        p1 = self.pin(ra, pa)
        p2 = self.pin(rb, pb)
        x1, y1 = self.leave_pin(ra, pa, p2)
        x2, y2 = self.leave_pin(rb, pb, p1)
        if mid is None:
            self.route(x1, y1, x2, y2)
            return
        path = [(x1, y1), (mid, y1), (mid, y2), (x2, y2)]
        if self._path_ok(path):
            self._draw_path(path)
        else:
            self.route(x1, y1, x2, y2)

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
            self.route(ex, ey, x, py)
            self.junc(x, py)
            return x, py
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
        x, y = self.leave_pin(ref, pin)
        if self.is_ic(ref):
            dx, dy = self.escape_dir(ref, pin)
            self.glabel(net, x, y, self._dir_rot(dx, dy))
            return
        sx, sy = self._away_vec(ref, pin, side)
        self.wire(x, y, x + sx * d, y + sy * d)
        self.glabel(net, x + sx * d, y + sy * d, self._dir_rot(sx, sy))

    def junc(self, x: float, y: float) -> None:
        self.items.append(
            f"""	(junction (at {x:.2f} {y:.2f}) (diameter 0) (color 0 0 0 0) (uuid "{uid()}"))"""
        )

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
        if side == "L":
            bx = min(x for x, _y in escaped)
        else:
            bx = max(x for x, _y in escaped)
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
        if side == "L":
            bx = min(x for x, _y in escaped)
        else:
            bx = max(x for x, _y in escaped)
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
        body = "\n".join(self.items)
        text = f"""(kicad_sch
	(version 20241209)
	(generator "eeschema")
	(generator_version "10.0")
	(uuid "{self.sheet_uuid}")
	(paper "{self.paper}")
	(title_block
		(title "{self.title}")
		(date "2026-09-10")
		(rev "1.2")
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


def build_power(lookups) -> Sch:
    s = Sch(
        PWR_UUID,
        "CallOnFail v1 — Alimentacion",
        "Gel 6V 7Ah + XY-SJVA 6,85V/0,6A + buck-boost 5,1V. No 12V, no ZK-S4, no XL4015.",
        f"/{ROOT_UUID}/{PWR_UUID}",
        paper="A1",
    )
    s.pin_lookup = lookups

    s.text("CallOnFail v1 — alimentacion (proto)", g(20), g(16), 2.54)
    s.text(
        "Bloques: carga | backup | high-side | WDT. Cable corto dentro del bloque; global al cruzar hoja.",
        g(20),
        g(22),
    )

    # --- Carga: 5V_PSU -> XY-SJVA -> fusible -> gel (izq -> der) ---
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

    # ADC gel pegado a la bateria (no al pie de la hoja)
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

    # --- Backup: U2 debajo/izq de la bateria, VIN corto ---
    s.place("Device:R", "R1", "10k", g(48), g(108), 0)
    s.place("Transistor_FET:2N7000", "Q5", "2N7000", g(70), g(124), 0)
    s.place("Device:R", "R18", "100k", g(48), g(140), 0)
    s.place("Module:BUCKBOOST", "U2", "XL6019", g(130), g(120), 0)
    s.place("Device:R", "R2", "10k", g(96), g(146), 0)
    s.place("Device:Q_PMOS", "Q10", "NDP6020P", g(154), g(112), 180)
    s.place("Transistor_FET:2N7000", "Q11", "2N7000", g(230), g(108), 0)
    s.place("Device:R", "R37", "100k", g(166), g(128), 0)
    s.place("Device:Q_PMOS", "Q8", "NDP6020P", g(178), g(96), 180)
    s.place("Transistor_FET:2N7000", "Q9", "2N7000", g(206), g(96), 0)
    s.place("Device:R", "R36", "100k", g(190), g(80), 0)
    s.place("Device:C_Polarized", "C1", "2200uF/16V", g(256), g(120), 0)
    s.place("Device:C", "C10", "100n", g(278), g(120), 0)
    s.tap_net("R1", "1", "5V_PSU", "U", g(6))
    s.join("R1", "2", "Q5", "2")
    s.join("R1", "2", "R18", "1")
    s.junc(*s.pin("R1", "2"))
    s.gnd_pin("R18", "2", "D")
    s.gnd_pin("Q5", "1", "D")
    s.join("Q5", "3", "U2", "3")
    s.tap_local("R1", "2", "PSU_DET", "R", g(8))
    s.join("BT1", "1", "U2", "1")
    s.join("BT1", "1", "R2", "1")
    s.join("R2", "2", "U2", "3")
    s.junc(*s.pin("U2", "3"))
    s.gnd_pin("U2", "2", "L")
    s.gnd_pin("U2", "5", "D")
    s.join("U2", "4", "Q10", "D")
    s.join("Q10", "S", "C1", "1")
    s.join("Q10", "G", "Q11", "3")
    s.join("Q10", "G", "R37", "1")
    s.junc(*s.pin("Q10", "G"))
    s.join("R37", "2", "C1", "1")
    s.gnd_pin("Q11", "1", "D")
    s.tap_local("U2", "3", "BB_EN", "R", g(6))
    s.tap_local("Q11", "2", "BB_EN", "L", g(6))
    s.tap_net("Q8", "D", "5V_PSU", "L", g(6))
    s.join("Q8", "S", "C1", "1")
    s.join("Q8", "G", "Q9", "3")
    s.join("Q8", "G", "R36", "1")
    s.junc(*s.pin("Q8", "G"))
    s.join("R36", "2", "C1", "1")
    s.gnd_pin("Q9", "1", "D")
    s.tap_local("Q9", "2", "PSU_DET", "L", g(6))
    s.gnd_pin("C1", "2", "D")
    s.join("C1", "1", "C10", "1")
    s.gnd_pin("C10", "2", "D")
    c1p = s.pin("C1", "1")
    s.junc(*c1p)
    s.tap_net("C1", "1", "5V_BUS", "R", g(10))
    s.place("power:PWR_FLAG", "#FLG_BUS", "PWR_FLAG", c1p[0] + g(16), c1p[1], 0)
    s.wire(c1p[0], c1p[1], c1p[0] + g(16), c1p[1])
    s.text("Hay PSU: Q5/Q9 ON -> backup EN=0, Q8 ON, Q10 OFF.", g(32), g(162))
    s.text("Sin PSU: Q8 OFF, EN=Gel+, Q11 ON, Q10 ON. Ambos NDP6020P S=BUS D=fuente.", g(32), g(166))
    s.text("Ajuste XL6019 5,1 V (ya no 5,4: no hay Schottky). No XL6009 / ZK-4KX.", g(32), g(170))

    # --- High-side: SYS lo maneja el 555 CMOS; modem sigue con inversor (GPIO 3V3) ---
    s.text("TLC555 HIGH=5V corta Q1. Modem: 74HCT125 DIP (3V3 in, 5V out). Todo THT.", g(32), g(176), 1.50)
    s.place("Device:Q_PMOS", "Q1", "NDP6020P", g(160), g(196), 180)
    s.place("Device:R", "R3", "100k", g(174), g(220), 0)
    s.place("Device:R", "R19", "1k", g(188), g(196), 0)
    s.join("Q1", "G", "R3", "1")
    s.join("Q1", "G", "R19", "2")
    s.junc(*s.pin("Q1", "G"))
    s.gnd_pin("R3", "2", "D")
    s.tap_local("R19", "1", "WDT_PULSE", "R", g(6))
    s.tap_net("Q1", "D", "5V_SYS", "D", g(6))
    s.text("Q1 5V_SYS. 555 idle LOW = ON. Pulso HIGH ~2 s = OFF.", g(32), g(244))

    s.place("Device:Q_PMOS", "Q3", "NDP6020P", g(256), g(196), 180)
    s.place("Module:AHCTBUF", "U13", "74HCT125", g(318), g(196), 0)
    s.place("Device:R", "R4", "100k", g(268), g(220), 0)
    s.place("Device:R", "R23", "1k", g(280), g(176), 0)
    s.place("Device:R", "R24", "100k", g(292), g(220), 0)
    s.place("Device:C", "C17", "100n", g(332), g(228), 0)
    s.gnd_rail("U13", ["1", "7", "4", "9", "12"], "L")
    s.rail("U13", ["14", "6", "10", "13"], "5V_BUS", "R")
    s.join("U13", "2", "R23", "2")
    s.join("U13", "2", "R24", "1")
    s.junc(*s.pin("U13", "2"))
    s.gnd_pin("R24", "2", "D")
    s.tap_net("R23", "1", "MODEM_CUT", "L", g(6))
    s.join("U13", "3", "Q3", "G")
    s.join("Q3", "G", "R4", "1")
    s.junc(*s.pin("Q3", "G"))
    s.gnd_pin("R4", "2", "D")
    s.tap_net("C17", "1", "5V_BUS", "U", g(4))
    s.gnd_pin("C17", "2", "D")
    s.tap_net("Q3", "D", "5V_MODEM", "D", g(6))
    s.text("Q3 5V_MODEM. 74HCT125 DIP-14: 1=OE 2=A 3=Y 7=GND 14=VCC. 6/10/13→VCC, 4/9/12→GND.", g(248), g(244))

    q1s = s.pin("Q1", "S")
    q3s = s.pin("Q3", "S")
    q8s = s.pin("Q8", "S")
    q10s = s.pin("Q10", "S")
    bus_y = g(168)
    s.wire(c1p[0], c1p[1], c1p[0], bus_y)
    s.wire(q1s[0], q1s[1], q1s[0], bus_y)
    s.wire(q3s[0], q3s[1], q3s[0], bus_y)
    s.wire(q8s[0], q8s[1], q8s[0], bus_y)
    s.wire(q10s[0], q10s[1], q10s[0], bus_y)
    xs = (c1p[0], q1s[0], q3s[0], q8s[0], q10s[0])
    s.wire(min(xs), bus_y, max(xs), bus_y)
    s.junc(c1p[0], bus_y)
    s.junc(q1s[0], bus_y)
    s.junc(q3s[0], bus_y)
    s.junc(q8s[0], bus_y)
    s.junc(q10s[0], bus_y)

    # LED PWR al lado de Q1 (5V_SYS), no cruzando Q3
    s.place("Device:R", "R5", "330R", g(128), g(196), 90)
    s.place("Device:LED", "D2", "LED GRN PWR", g(108), g(196), 180)
    s.join("Q1", "D", "R5", "1")
    s.join("R5", "2", "D2", "2")
    s.gnd_pin("D2", "1", "D")
    s.junc(*s.pin("Q1", "D"))
    s.tap_net("R5", "2", "LED_PWR", "U", g(6))
    s.text("LED PWR de frente, sin ESP.", g(4), g(214))

    # --- WDT a la derecha, bloque propio ---
    s.text("WDT CD4541 (~4-5 min) + TLC555 CMOS (~2 s) en 5V_BUS", g(300), g(20), 1.50)
    s.place("Timer:CD4541BE", "U3", "CD4541BE", g(340), g(76), 0)
    p_rtc = s.pin("U3", "1")
    p_ctc = s.pin("U3", "2")
    s.place("Device:R", "R8", "1M", p_rtc[0] - g(14), (p_rtc[1] + p_ctc[1]) / 2, 0)
    s.place("Device:C", "C4", "100n", p_ctc[0] - g(14), p_ctc[1] + g(16), 0)
    s.join("U3", "1", "R8", "1")
    s.join("U3", "2", "R8", "2")
    s.join("U3", "2", "C4", "1")
    s.junc(*s.pin("U3", "2"))
    s.gnd_pin("C4", "2", "D")
    s.place("Device:R", "R9", "10k", p_rtc[0] - g(14), p_rtc[1] - g(20), 0)
    s.tap_net("R9", "1", "5V_BUS", "U", g(6))
    s.join("R9", "2", "U3", "3")
    s.gnd_pin("U3", "5", "L")
    s.gnd_pin("U3", "7", "D")
    s.tap_net("U3", "14", "5V_BUS", "U", g(4))
    s.rail("U3", ["9", "10", "12", "13"], "5V_BUS", "L")
    s.nc(*s.pin("U3", "4"))
    s.nc(*s.pin("U3", "11"))
    s.place("Device:C", "C8", "100n", g(368), g(40), 0)
    s.tap_net("C8", "1", "5V_BUS", "U", g(4))
    s.gnd_pin("C8", "2", "D")
    s.text("R8/C4: tunear 4-5 min (2^16). Pin 9 HIGH => Q activo bajo. C8 junto al 4541.", g(278), g(108))

    s.place("Device:C", "C5", "100n", g(400), g(38), 90)
    s.place("Device:R", "R10", "100k", g(420), g(54), 0)
    s.place("Device:D", "D3", "1N4148", g(400), g(62), 180)
    s.tap_net("C5", "1", "IO15", "L", g(6))
    s.join("C5", "2", "R10", "1")
    s.join("C5", "2", "D3", "2")
    s.junc(*s.pin("C5", "2"))
    s.gnd_pin("R10", "2", "D")
    s.tap_local("D3", "1", "WDT_MR", "L", g(6))
    s.place("Device:R", "R20", "100k", g(356), g(96), 0)
    s.tap_local("R20", "1", "WDT_MR", "L", g(6))
    s.gnd_pin("R20", "2", "D")
    s.text("Patada IO15 acople. No DC a GND en IO15. R20 baja MR.", g(380), g(78))

    s.place("Timer:NE555D", "U4", "TLC555", g(410), g(136), 0)
    s.place("Device:C", "C6", "10n", g(370), g(120), 0)
    s.place("Device:R", "R11", "22k", g(370), g(154), 0)
    s.place("Device:C_Polarized", "C7", "100uF", g(350), g(170), 0)
    s.place("Device:D", "D4", "1N4148", g(444), g(136), 180)
    s.place("Device:D", "D5", "1N4148", g(370), g(136), 180)
    s.place("Device:R", "R12", "10k", g(340), g(162), 0)
    s.place("Switch:SW_Push", "SW1", "RESET gabinete", g(444), g(170), 0)
    s.tap_net("U4", "8", "5V_BUS", "U", g(4))
    s.place("Device:C", "C9", "100n", g(430), g(120), 0)
    s.tap_net("C9", "1", "5V_BUS", "U", g(4))
    s.gnd_pin("C9", "2", "D")
    s.gnd_pin("U4", "1", "D")
    s.tap_net("U4", "4", "5V_BUS", "L", g(10))
    s.join("U4", "5", "C6", "1")
    s.gnd_pin("C6", "2", "D")
    s.join("U4", "6", "U4", "7")
    s.join("U4", "6", "R11", "2")
    s.join("U4", "6", "C7", "1")
    s.junc(*s.pin("U4", "6"))
    s.tap_net("R11", "1", "5V_BUS", "U", g(6))
    s.gnd_pin("C7", "2", "D")
    s.join("U4", "3", "D4", "2")
    s.tap_local("D4", "1", "WDT_MR", "L", g(6))
    s.tap_local("U3", "6", "WDT_MR", "L", g(6))
    s.tap_local("U4", "3", "WDT_PULSE", "R", g(8))
    s.tap_local("U3", "8", "WDT_Q", "R", g(6))
    s.tap_local("D5", "1", "WDT_Q", "U", g(6))
    s.tap_net("D5", "2", "BTN_RESET", "D", g(6))
    u42 = s.pin("U4", "2")
    trig_x = u42[0] - g(12)
    s.wire(u42[0], u42[1], trig_x, u42[1])
    s.junc(trig_x, u42[1])
    r12b = s.pin("R12", "2")
    s.wire(r12b[0], r12b[1], trig_x, r12b[1])
    s.wire(trig_x, r12b[1], trig_x, u42[1])
    s.junc(trig_x, r12b[1])
    sw1 = s.pin("SW1", "1")
    s.wire(sw1[0], sw1[1], trig_x, sw1[1])
    s.wire(trig_x, sw1[1], trig_x, u42[1])
    s.junc(trig_x, sw1[1])
    s.tap_net("R12", "1", "5V_BUS", "U", g(6))
    s.gnd_pin("SW1", "2", "D")
    s.tap_net("SW1", "1", "BTN_RESET", "R", g(6))
    s.text("D5: timeout 4541 baja TRIG. SW1 NA a GND. TLC555 OUT a Q1 (no NE555 bipolar).", g(278), g(190))

    s.place("Module:IDC10", "J7", "IDC A→B", g(48), g(280), 0)
    s.gnd_pin("J7", "1", "R")
    s.tap_net("J7", "2", "5V_SYS", "R", g(8))
    s.gnd_pin("J7", "3", "R")
    s.tap_net("J7", "4", "5V_MODEM", "R", g(8))
    s.tap_net("J7", "5", "GEL_ADC", "R", g(8))
    s.tap_net("J7", "6", "IO15", "R", g(8))
    s.tap_net("J7", "7", "MODEM_CUT", "R", g(8))
    s.tap_net("J7", "8", "LED_PWR", "R", g(8))
    s.tap_net("J7", "9", "BTN_RESET", "R", g(8))
    s.gnd_pin("J7", "10", "R")
    s.text("Cinta 10: GND en 1/3/10. Polarizada. No enchufar con 5 V. 1000uF del modem va en B.", g(20), g(332))
    return s


def build_io(lookups) -> Sch:
    s = Sch(
        IO_UUID,
        "CallOnFail v1 — ESP32 / modem / sensores / campo",
        "WT32-ETH01 + A7672SA-FASE. I2C IO32/IO33. No GPIO21/22. No 5V y 3V3 a la vez en el WT32.",
        f"/{ROOT_UUID}/{IO_UUID}",
        paper="A1",
    )
    s.pin_lookup = lookups

    s.text("CallOnFail v1 — WT32, A7672, sensores, PCF, frente", g(20), g(16), 2.54)
    s.text(
        "USB | WT32 | modem  —  I2C y sensores debajo  —  PCF / campo / relés a la derecha.",
        g(20),
        g(22),
    )

    # MCU row
    s.place("Connector_Generic:Conn_01x04", "J2", "USB-Serial 3V3", g(32), g(80), 0)
    s.place("Module:WT32", "U5", "WT32-ETH01", g(90), g(80), 0)
    s.place("Module:A7672", "U6", "A7672SA-FASE", g(188), g(72), 0)
    s.place("Connector_Generic:Conn_01x02", "J3", "MODEM_CUT GPIO", g(188), g(118), 0)

    s.tap_net("U5", "1", "5V_SYS", "L", g(6))
    s.gnd_pin("U5", "2", "L")
    s.tap_net("U5", "3", "3V3", "L", g(6))
    tx, ty = s.escape_pt("U5", "3")
    s.junc(tx, ty)
    s.place("power:PWR_FLAG", "#FLG_3V3", "PWR_FLAG", tx, ty - g(6), 0)
    s.wire(tx, ty, tx, ty - g(6))

    s.gnd_pin("J2", "1", "L")
    s.join("J2", "2", "U5", "5")
    s.join("J2", "3", "U5", "4")
    s.join("J2", "4", "U5", "6")
    s.text("J2: USB TX->RXD0, USB RX->TXD0, IO0 a GND solo para flash. No 5V del USB.", g(20), g(112))

    s.place("Device:C", "C11", "100n", g(70), g(50), 0)
    s.place("Device:C_Polarized", "C15", "10uF", g(86), g(50), 0)
    s.tap_net("C11", "1", "5V_SYS", "U", g(4))
    s.gnd_pin("C11", "2", "D")
    s.tap_net("C15", "1", "5V_SYS", "U", g(4))
    s.gnd_pin("C15", "2", "D")

    s.tap_net("U6", "1", "5V_MODEM", "L", g(6))
    s.gnd_pin("U6", "2", "L")
    s.place("Device:C_Polarized", "C2", "1000uF/16V", g(230), g(56), 0)
    s.place("Device:C", "C12", "100n", g(248), g(56), 0)
    s.tap_net("C2", "1", "5V_MODEM", "U", g(4))
    s.gnd_pin("C2", "2", "D")
    s.tap_net("C12", "1", "5V_MODEM", "U", g(4))
    s.gnd_pin("C12", "2", "D")
    s.join("U5", "9", "U6", "3")
    s.join("U5", "12", "U6", "4")
    s.place("Device:R", "R32", "1k", g(148), g(56), 0)
    s.place("Device:R", "R33", "1k", g(148), g(68), 0)
    s.join("U5", "8", "R32", "1")
    s.join("R32", "2", "U6", "5")
    s.join("U5", "7", "R33", "1")
    s.join("R33", "2", "U6", "6")
    s.nc(*s.pin("U6", "7"))
    s.tap_net("J3", "1", "MODEM_CUT", "L", g(6))
    s.gnd_pin("J3", "2", "D")
    s.text("OC IO4/IO2 via 1k. No >100 nF. No bajar RESET y PWRKEY a la vez.", g(168), g(108))
    s.text("J3: GPIO salida TBD (no IO39, no IO2, no strapping). HIGH = corta modem.", g(148), g(132))

    s.tap_net("U5", "11", "IO15", "R", g(6))
    s.tap_net("U5", "15", "GEL_ADC", "R", g(6))

    sda_x, sda_y = s.leave_pin("U5", "13")
    scl_x, scl_y = s.leave_pin("U5", "14")
    # Past the WT32 right-pin stubs so the bus does not short IO14–IO39.
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

    # I2C slaves under WT32 / just right of the bus
    s.place("Module:OLED", "U7", "OLED 0x3C", g(90), g(150), 0)
    s.place("Module:SHT31", "U8", "SHT31 0x44", g(90), g(186), 0)
    s.place("Module:PCF8574", "U11", "PCF8574", g(200), g(150), 0)
    s.gnd_pin("U7", "2", "L")
    s.gnd_pin("U8", "2", "L")
    s.tap_net("U11", "1", "3V3", "L", g(6))
    s.gnd_pin("U11", "2", "L")
    s.gnd_rail("U11", ["5", "6", "7"], "L")
    s.place("Device:C", "C13", "100n", g(176), g(124), 0)
    s.tap_net("C13", "1", "3V3", "U", g(4))
    s.gnd_pin("C13", "2", "D")

    for ref, sda_pin, scl_pin in (("U7", "3", "4"), ("U8", "3", "4"), ("U11", "3", "4")):
        s.tap_x(ref, sda_pin, bus_sda)
        s.tap_x(ref, scl_pin, bus_scl)
    i2c_sda_ys = [sda_y, s.pin("U7", "3")[1], s.pin("U8", "3")[1], s.pin("U11", "3")[1]]
    i2c_scl_ys = [scl_y, s.pin("U7", "4")[1], s.pin("U8", "4")[1], s.pin("U11", "4")[1]]
    s.wire(bus_sda, min(i2c_sda_ys), bus_sda, max(i2c_sda_ys))
    s.wire(bus_scl, min(i2c_scl_ys), bus_scl, max(i2c_scl_ys))
    s.text("I2C IO32/IO33. Pull-up 4k7; omitir si el modulo ya las trae. PCF 0x20.", g(20), g(208))

    s.place("Module:DS18B20", "U9", "DS18B20 x4", g(90), g(222), 0)
    s.place("Device:R", "R15", "4k7", g(50), g(222), 0)
    s.gnd_pin("U9", "2", "L")
    s.tap_local("U5", "10", "OWIRE", "R", g(8))
    s.tap_local("U9", "3", "OWIRE", "L", g(8))
    s.tap_local("R15", "2", "OWIRE", "R", g(6))
    s.tap_net("R15", "1", "3V3", "U", g(4))
    v3_x = g(34)
    s.vspine(v3_x, [s.pin("U7", "1"), s.pin("U8", "1"), s.pin("U9", "1")])
    s.glabel("3V3", v3_x, s.pin("U7", "1")[1], 180)
    s.text("1-Wire cadena, no estrella. UTP DATA+GND.", g(20), g(244))

    s.place("Module:ZMPT", "U10", "ZMPT101B", g(148), g(222), 0)
    s.place("Device:R", "R34", "1k", g(128), g(214), 0)
    s.place("Device:C", "C14", "100n", g(168), g(236), 0)
    s.gnd_pin("U10", "2", "L")
    s.tap_local("U5", "16", "ZMPT_ADC", "R", g(8))
    s.join("U10", "1", "R34", "1")
    s.tap_local("R34", "2", "ZMPT_ADC", "L", g(6))
    s.join("R34", "2", "C14", "1")
    s.gnd_pin("C14", "2", "D")
    s.nc(*s.pin("U10", "3"))
    s.text("ZMPT OUT -> 1k -> IO36. No 220 hasta validar.", g(108), g(246))

    s.place("Switch:SW_Push", "SW2", "Servicio IO39", g(148), g(254), 0)
    s.place("Device:R", "R31", "10k", g(128), g(254), 0)
    s.tap_local("U5", "17", "BTN_SVC", "R", g(8))
    s.tap_local("SW2", "1", "BTN_SVC", "L", g(6))
    s.tap_local("R31", "2", "BTN_SVC", "R", g(6))
    s.tap_net("R31", "1", "3V3", "U", g(4))
    s.gnd_pin("SW2", "2", "D")
    s.text("IO39 sin pull interno. 10k a 3V3. Corta=llamada, larga=OTA.", g(108), g(268))

    # Campo: 2 IN + 2 OUT. P2 spare (pull-up). P3 buzzer. P6 NET, P7 ALARMA.
    s.place("Connector_Generic:Conn_01x04", "J4", "Campo IN/OUT", g(268), g(130), 0)
    s.place("Connector_Generic:Conn_01x02", "J5", "Campo GND/COM", g(268), g(168), 0)
    s.join("U11", "8", "J4", "1")
    s.join("U11", "9", "J4", "2")
    s.place("Device:R", "R27", "10k", g(236), g(108), 90)
    s.place("Device:R", "R28", "10k", g(244), g(108), 90)
    s.place("Device:R", "R29", "10k", g(252), g(108), 90)
    s.join("R27", "2", "J4", "1")
    s.join("R28", "2", "J4", "2")
    s.join("R29", "2", "U11", "10")
    s.tap_net("R27", "1", "3V3", "U", g(4))
    s.tap_net("R28", "1", "3V3", "U", g(4))
    s.tap_net("R29", "1", "3V3", "U", g(4))
    s.gnd_pin("J5", "1", "L")
    s.text("IN1 fuga, IN2 extra. P2 spare 10k. Contacto seco a GND.", g(210), g(188))

    s.place("Transistor_Array:ULN2003", "U12", "ULN2003", g(200), g(230), 0)
    s.place("Module:RELAY", "K1", "OUT1", g(268), g(214), 0)
    s.place("Module:BUZZER", "LS1", "Buzzer 5V activo", g(232), g(198), 0)
    s.place("Module:RELAY", "K2", "OUT2 aux", g(268), g(258), 0)
    s.tap_local("U11", "11", "BUZZ_DRV", "R", g(6))
    s.tap_local("U11", "12", "OUT1_DRV", "R", g(6))
    s.tap_local("U11", "13", "OUT2_DRV", "R", g(6))
    s.tap_local("U12", "1", "OUT1_DRV", "L", g(6))
    s.tap_local("U12", "2", "OUT2_DRV", "L", g(6))
    s.tap_local("U12", "3", "BUZZ_DRV", "L", g(6))
    s.gnd_rail("U12", ["4", "5", "6", "7"], "L")
    s.gnd_pin("U12", "8", "D")
    s.nc(*s.pin("U12", "10"))
    s.nc(*s.pin("U12", "11"))
    s.nc(*s.pin("U12", "12"))
    s.nc(*s.pin("U12", "13"))
    s.join("U12", "16", "K1", "2")
    s.join("U12", "15", "K2", "2")
    s.join("U12", "14", "LS1", "2")
    sys_x = g(244)
    s.vspine(sys_x, [s.pin("U12", "9"), s.pin("K1", "1"), s.pin("K2", "1"), s.pin("LS1", "1")], "5V_SYS", "R")
    s.flyback("D8", "K1")
    s.flyback("D9", "K2")
    s.join("K1", "3", "J5", "2")
    s.join("K2", "3", "J5", "2")
    s.junc(*s.pin("J5", "2"))
    s.join("K1", "4", "J4", "3")
    s.join("K2", "4", "J4", "4")
    s.text("P3=buzzer O3, P4=K1 O1, P5=K2 O2. P6=NET P7=ALARMA. 2 IN + 2 OUT.", g(188), g(284))

    s.place("Connector_Generic:Conn_01x05", "J6", "Panel 5 pin", g(304), g(130), 0)
    s.place("Device:R", "R16", "330R", g(328), g(120), 90)
    s.place("Device:LED", "D6", "LED NET", g(352), g(120), 180)
    s.place("Device:R", "R17", "330R", g(328), g(138), 90)
    s.place("Device:LED", "D7", "LED ALARMA", g(352), g(138), 180)
    s.gnd_pin("J6", "1", "L")
    s.tap_net("J6", "2", "LED_PWR", "L", g(6))
    s.tap_net("R16", "1", "5V_SYS", "U", g(4))
    s.join("R16", "2", "D6", "2")
    s.tap_local("D6", "1", "LED_NET", "L", g(6))
    s.tap_local("U11", "14", "LED_NET", "R", g(6))
    s.tap_net("J6", "3", "LED_NET", "L", g(6))
    s.tap_net("R17", "1", "5V_SYS", "U", g(4))
    s.join("R17", "2", "D7", "2")
    s.tap_local("D7", "1", "LED_ALM", "L", g(6))
    s.tap_local("U11", "15", "LED_ALM", "R", g(6))
    s.tap_net("J6", "4", "LED_ALM", "L", g(6))
    s.tap_net("J6", "5", "BTN_RESET", "L", g(6))
    s.text("Panel: GND, PWR, NET (P6), ALARMA (P7), RESET. Buzzer P3, no borne.", g(300), g(152))

    s.place("Module:IDC10", "J8", "IDC B←A", g(48), g(300), 0)
    s.gnd_pin("J8", "1", "R")
    s.tap_net("J8", "2", "5V_SYS", "R", g(8))
    s.gnd_pin("J8", "3", "R")
    s.tap_net("J8", "4", "5V_MODEM", "R", g(8))
    s.tap_net("J8", "5", "GEL_ADC", "R", g(8))
    s.tap_net("J8", "6", "IO15", "R", g(8))
    s.tap_net("J8", "7", "MODEM_CUT", "R", g(8))
    s.tap_net("J8", "8", "LED_PWR", "R", g(8))
    s.tap_net("J8", "9", "BTN_RESET", "R", g(8))
    s.gnd_pin("J8", "10", "R")
    s.text(
        "GPIO: 0 ETH CLK  2 PWRKEY  4 RESET  5 RX  14 1-Wire  15 WDT  16/18/23 ETH  17 TX  32 SDA  33 SCL  35 gel  36 ZMPT  39 servicio",
        g(20),
        g(348),
    )
    return s


def build_root() -> str:
    return f"""(kicad_sch
	(version 20241209)
	(generator "eeschema")
	(generator_version "10.0")
	(uuid "{ROOT_UUID}")
	(paper "A3")
	(title_block
		(title "CallOnFail v1")
		(date "2026-09-10")
		(rev "1.2")
		(company "CallOnFail")
		(comment 1 "WT32-ETH01 + A7672SA-FASE + gel 6V. Ver docs/HARDWARE_V1.md")
	)
	(lib_symbols
	)
	(text "CallOnFail hardware v1"
		(exclude_from_sim no)
		(at 50.80 30.48 0)
		(effects (font (size 3.81 3.81)) (justify left bottom))
		(uuid "{uid()}")
	)
	(text "Abrir las dos hojas jerarquicas. Nets globales unen alimentacion con I/O."
		(exclude_from_sim no)
		(at 50.80 40.64 0)
		(effects (font (size 1.27 1.27)) (justify left bottom))
		(uuid "{uid()}")
	)
	(text "5V_PSU  5V_BUS  5V_SYS  5V_MODEM  3V3  GEL_P  GEL_ADC  IO15  MODEM_CUT  LED_PWR  BTN_RESET"
		(exclude_from_sim no)
		(at 50.80 48.26 0)
		(effects (font (size 1.27 1.27)) (justify left bottom))
		(uuid "{uid()}")
	)
	(text "No 12 V. No ZK-S4. No XL4015. No IRF4905 a 5 V. Gel 6 V 7 Ah, flote 6,85 V."
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
