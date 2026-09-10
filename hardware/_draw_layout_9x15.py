#!/usr/bin/env python3
"""To-scale layout of two 9x15 cm protoboards (4 px/mm)."""
from PIL import Image, ImageDraw, ImageFont

PX = 4  # px per mm
W, H = 90 * PX, 150 * PX  # 360 x 600

# XY-SJVA ~65x32; XL6019 auto buck-boost típico ~50x30
XY = (32 * PX, 65 * PX)
BB = (30 * PX, 50 * PX)


def font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for name in ("segoeui.ttf", "arial.ttf", "calibri.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def rounded(draw, box, fill, outline, r=8):
    draw.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=2)


def label(draw, xy, text, size=14, fill="#1a1a1a", anchor="mm"):
    draw.text(xy, text, font=font(size), fill=fill, anchor=anchor)


def board(draw, ox, oy, title, fill="#f3e2c4"):
    rounded(draw, (ox - 6, oy - 36, ox + W + 6, oy + H + 6), fill, "#5a4632", 14)
    # hole grid
    for x in range(ox + 8, ox + W - 4, 10):
        for y in range(oy + 8, oy + H - 4, 10):
            draw.ellipse((x - 1, y - 1, x + 1, y + 1), fill="#d9c4a0")
    draw.rectangle((ox, oy - 32, ox + W, oy), fill="#1f4e79")
    label(draw, (ox + W / 2, oy - 16), title, 16, "white")


def zone(draw, box, fill, outline, title, lines):
    rounded(draw, box, fill, outline, 8)
    x0, y0, x1, y1 = box
    label(draw, ((x0 + x1) / 2, y0 + 12), title, 13, "#111")
    for i, line in enumerate(lines):
        label(draw, ((x0 + x1) / 2, y0 + 28 + i * 14), line, 11, "#333")


def main() -> None:
    img = Image.new("RGB", (1280, 780), "#fafafa")
    d = ImageDraw.Draw(img)
    label(d, (640, 28), "CallOnFail v1 — dos placas 9×15 cm", 28, "#111")
    label(
        d,
        (640, 54),
        "XY-SJVA y XL6019 van SOBRE la placa A (standoff). Fuente 5 V y gel al chasis.",
        14,
        "#444",
    )

    ax, ay = 80, 100
    board(d, ax, ay, "PLACA A — ALIMENTACIÓN", "#efe0c8")

    # modules on A, top: side by side, long axis vertical (along 15 cm)
    gap = 12
    mx = ax + (W - XY[0] - BB[0] - gap) // 2
    my = ay + 10
    rounded(d, (mx, my, mx + XY[0], my + XY[1]), "#cfe8ff", "#1d4ed8", 6)
    label(d, (mx + XY[0] / 2, my + XY[1] / 2 - 10), "XY-SJVA", 13, "#1e3a8a")
    label(d, (mx + XY[0] / 2, my + XY[1] / 2 + 8), "65×32 mm", 11, "#1e40af")
    label(d, (mx + XY[0] / 2, my + XY[1] / 2 + 22), "6,85 V / 0,6 A", 10, "#1e40af")

    bx = mx + XY[0] + gap
    rounded(d, (bx, my, bx + BB[0], my + BB[1]), "#ddd6fe", "#6d28d9", 6)
    label(d, (bx + BB[0] / 2, my + BB[1] / 2 - 10), "XL6019", 13, "#4c1d95")
    label(d, (bx + BB[0] / 2, my + BB[1] / 2 + 8), "50×30 mm", 11, "#5b21b6")
    label(d, (bx + BB[0] / 2, my + BB[1] / 2 + 22), "buck-boost 5,4 V", 10, "#5b21b6")

    y = my + XY[1] + 10  # XY is the taller of the two modules
    zone(
        d,
        (ax + 8, y, ax + W - 8, y + 52),
        "#dbeafe",
        "#2563eb",
        "Bornes  ·  F1 5 A",
        ["PSU 5 V    GEL+ / GEL−    fusible 5×20"],
    )
    y += 60
    zone(
        d,
        (ax + 8, y, ax + W - 8, y + 88),
        "#fef3c7",
        "#d97706",
        "WDT  CD4541 + NE555",
        ["pasivos, D3/D4/D5, SW RESET vía cinta"],
    )
    y += 96
    zone(
        d,
        (ax + 8, y, ax + W / 2 - 4, ay + H - 40),
        "#ffedd5",
        "#ea580c",
        "High-side",
        ["2× AO3401", "2N7000  caps"],
    )
    zone(
        d,
        (ax + W / 2 + 4, y, ax + W - 8, ay + H - 40),
        "#dcfce7",
        "#16a34a",
        "GEL_ADC",
        ["47k / 22k", "SB560"],
    )
    rounded(d, (ax + 8, ay + H - 32, ax + W - 8, ay + H - 8), "#14532d", "#14532d", 6)
    label(d, (ax + W / 2, ay + H - 20), "cinta 10 pines → B", 13, "white")

    # dimension A
    d.line((ax - 28, ay, ax - 28, ay + H), fill="#333", width=2)
    label(d, (ax - 42, ay + H / 2), "15 cm", 13, "#111", anchor="mm")
    d.line((ax, ay + H + 22, ax + W, ay + H + 22), fill="#333", width=2)
    label(d, (ax + W / 2, ay + H + 36), "9 cm", 13)

    # board B
    cx, cy = 620, 100
    board(d, cx, cy, "PLACA B — I/O", "#efe0c8")
    zone(d, (cx + 8, cy + 10, cx + W / 2 - 4, cy + 130), "#e9d5ff", "#7c3aed", "PCF8574", ["I2C"])
    zone(d, (cx + W / 2 + 4, cy + 10, cx + W - 8, cy + 130), "#ddd6fe", "#6d28d9", "ULN2003", ["sink"])
    zone(d, (cx + 8, cy + 140, cx + W / 2 - 4, cy + 280), "#fecaca", "#dc2626", "K1 sirena", ["relé 5 V"])
    zone(d, (cx + W / 2 + 4, cy + 140, cx + W - 8, cy + 280), "#fecaca", "#dc2626", "K2 aux", ["relé 5 V"])
    zone(d, (cx + 8, cy + 290, cx + W / 2 - 4, cy + 430), "#bfdbfe", "#2563eb", "Campo", ["IN1–4  OUT1–2"])
    zone(d, (cx + W / 2 + 4, cy + 290, cx + W - 8, cy + 430), "#fde68a", "#ca8a04", "ZMPT / 1-Wire", ["borde"])
    zone(
        d,
        (cx + 8, cy + 440, cx + W - 8, cy + H - 40),
        "#e2e8f0",
        "#475569",
        "Headers WT32 + A7672  (hembra al borde)",
        ["los módulos enchufan / cuelgan fuera"],
    )
    rounded(d, (cx + 8, cy + H - 32, cx + W - 8, cy + H - 8), "#14532d", "#14532d", 6)
    label(d, (cx + W / 2, cy + H - 20), "cinta 10 pines ← A", 13, "white")
    d.line((cx, cy + H + 22, cx + W, cy + H + 22), fill="#333", width=2)
    label(d, (cx + W / 2, cy + H + 36), "9 cm", 13)

    # off-board B
    label(d, (1080, 130), "Fuera de B / tapa", 14, "#111")
    for i, t in enumerate(
        ["WT32-ETH01 + RJ45", "A7672SA-FASE", "OLED 1.3\"", "SHT31", "Frente: PWR NET ALARMA RESET"]
    ):
        rounded(d, (980, 150 + i * 44, 1255, 186 + i * 44), "#fff", "#64748b", 8)
        label(d, (1118, 168 + i * 44), t, 12)

    # outside A still: PSU and gel chassis
    label(d, (260, 740), "Chasis (no van en la placa): fuente 5 V 5 A  ·  gel 6 V 7 Ah", 13, "#444")
    label(
        d,
        (640, 762),
        "Standoff 8–10 mm. Potes del XY-SJVA accesibles. Potencia en cable 0,75 mm².",
        12,
        "#7c2d12",
    )

    out = __file__.replace("_draw_layout_9x15.py", "layout-9x15.png")
    img.save(out, "PNG")
    print("wrote", out)


if __name__ == "__main__":
    main()
