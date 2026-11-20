#!/usr/bin/env python3
"""Erzeugt main/footer_icons.c/.h: Zahnrad- und QR-Icon der Fußzeile.

Die Icons sind ARGB8888-Bitmaps (LVGL) mit transparentem Hintergrund.
Das Zahnrad wird 16-fach überabgetastet und geglättet, das QR-Icon wird
pixelgenau gezeichnet. Nur Pillow wird benötigt:

    python tools/generate_footer_icons.py
"""

import math
from pathlib import Path

from PIL import Image, ImageDraw

SIZE = 16
SUPERSAMPLE = 16
WHITE = (255, 255, 255)
ORANGE = (255, 165, 0)  # COLOR_ORANGE in ui.c (RGB565 0xFD20)


def gear_mask() -> Image.Image:
    scale = SUPERSAMPLE
    big = SIZE * scale
    center = big / 2
    teeth = 8
    root, tip = 5.3 * scale, 7.6 * scale
    half_root = math.pi / teeth * 0.62  # Zahnbreite am Fuß
    half_tip = math.pi / teeth * 0.36   # Zahnbreite an der Spitze
    points = []
    for i in range(teeth):
        angle = 2 * math.pi * i / teeth - math.pi / 2
        for radius, offset in ((root, -half_root), (tip, -half_tip),
                               (tip, half_tip), (root, half_root)):
            a = angle + offset
            points.append((center + radius * math.cos(a),
                           center + radius * math.sin(a)))
    mask = Image.new("L", (big, big), 0)
    draw = ImageDraw.Draw(mask)
    draw.polygon(points, fill=255)
    draw.ellipse([center - 5.6 * scale, center - 5.6 * scale,
                  center + 5.6 * scale, center + 5.6 * scale], fill=255)
    hole = 2.6 * scale
    draw.ellipse([center - hole, center - hole, center + hole, center + hole],
                 fill=0)
    return mask.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def qr_mask() -> Image.Image:
    mask = Image.new("L", (SIZE, SIZE), 0)
    px = mask.load()

    def block(x, y, w, h):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                px[xx, yy] = 255

    for fx, fy in ((1, 1), (10, 1), (1, 10)):  # drei Suchmuster 5x5
        block(fx, fy, 5, 5)
        block(fx + 1, fy + 1, 3, 3)
        px_clear = (fx + 1, fy + 1, 3, 3)
        for yy in range(px_clear[1], px_clear[1] + 3):
            for xx in range(px_clear[0], px_clear[0] + 3):
                px[xx, yy] = 0
        block(fx + 2, fy + 2, 1, 1)
    for x, y in ((9, 9), (12, 9), (10, 11), (13, 11), (9, 13), (12, 13)):
        block(x, y, 2, 1 if y == 13 else 2)
    return mask


def to_c(name: str, mask: Image.Image, color) -> str:
    data = []
    for y in range(SIZE):
        for x in range(SIZE):
            alpha = mask.getpixel((x, y))
            r, g, b = color
            data.extend((b, g, r, alpha))  # ARGB8888 liegt als B,G,R,A im Speicher
    rows = []
    for i in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{v:02x}" for v in data[i:i + 16]) + ",")
    body = "\n".join(rows)
    return (
        f"static const uint8_t {name}_data[] = {{\n{body}\n}};\n\n"
        f"const lv_image_dsc_t {name} = {{\n"
        "    .header = {\n"
        "        .magic = LV_IMAGE_HEADER_MAGIC,\n"
        "        .cf = LV_COLOR_FORMAT_ARGB8888,\n"
        f"        .w = {SIZE},\n"
        f"        .h = {SIZE},\n"
        f"        .stride = {SIZE * 4},\n"
        "    },\n"
        f"    .data_size = sizeof({name}_data),\n"
        f"    .data = {name}_data,\n"
        "};\n"
    )


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "main"
    gear, qr = gear_mask(), qr_mask()
    (out / "footer_icons.h").write_text(
        "#pragma once\n\n"
        "/* Erzeugt von tools/generate_footer_icons.py - nicht von Hand ändern. */\n"
        '#include "lvgl.h"\n\n'
        "#define FOOTER_ICON_SIZE 16\n\n"
        "extern const lv_image_dsc_t footer_icon_settings_white;\n"
        "extern const lv_image_dsc_t footer_icon_settings_orange;\n"
        "extern const lv_image_dsc_t footer_icon_qr_white;\n",
        encoding="utf-8",
    )
    (out / "footer_icons.c").write_text(
        "/* Erzeugt von tools/generate_footer_icons.py - nicht von Hand ändern. */\n"
        '#include "footer_icons.h"\n\n'
        + to_c("footer_icon_settings_white", gear, WHITE) + "\n"
        + to_c("footer_icon_settings_orange", gear, ORANGE) + "\n"
        + to_c("footer_icon_qr_white", qr, WHITE),
        encoding="utf-8",
    )
    # Vorschau in der Konsole
    for label, mask in (("settings", gear), ("qr", qr)):
        print(label)
        for y in range(SIZE):
            print("".join(" .:#"[min(3, mask.getpixel((x, y)) // 64)] * 2
                          for x in range(SIZE)))


if __name__ == "__main__":
    main()
