#!/usr/bin/env python3
"""Generate icon bitmaps for Material Symbols Outlined icons (settings, qr_code).

Outputs a C header file with two 24x24 monochrome bitmaps.
Uses svglib + reportlab.renderPM for SVG rasterization.
"""

import io
import sys
from svglib.svglib import svg2rlg
from reportlab.graphics import renderPM
from PIL import Image

SETTINGS_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" height="24" viewBox="0 -960 960 960" width="24">'
    '<path d="m370-80-16-128q-13-5-24.5-12T307-235l-119 50L78-375l103-78q-1-7-1-13.5v-27'
    'q0-6.5 1-13.5L78-585l110-190 119 50q11-8 23-15t24-12l16-128h220l16 128q13 5 24.5 12'
    't22.5 15l119-50 110 190-103 78q1 7 1 13.5v27q0 6.5-2 13.5l103 78-110 190-118-50q-11 8'
    '-23 15t-24 12L590-80H370Zm70-80h79l14-106q31-8 57.5-23.5T639-327l99 41 39-68-86-65q5-14'
    '7-29.5t2-31.5q0-16-2-31.5t-7-29.5l86-65-39-68-99 42q-22-23-48.5-38.5T533-694l-13-106h-79'
    'l-14 106q-31 8-57.5 23.5T321-633l-99-41-39 68 86 64q-5 15-7 30t-2 32q0 16 2 31t7 30l-86 65'
    '39 68 99-42q22 23 48.5 38.5T427-266l13 106Zm42-180q58 0 99-41t41-99q0-58-41-99t-99-41'
    'q-59 0-99.5 41T342-480q0 58 40.5 99t99.5 41Zm-2-140Z"/></svg>'
)

QR_CODE_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" height="24" viewBox="0 -960 960 960" width="24">'
    '<path d="M120-520v-320h320v320H120Zm80-80h160v-160H200v160Zm-80 480v-320h320v320H120Z'
    'm80-80h160v-160H200v160Zm320-320v-320h320v320H520Zm80-80h160v-160H600v160Zm160 480v-80h80v80'
    'h-80ZM520-360v-80h80v80h-80Zm80 80v-80h80v80h-80Zm-80 80v-80h80v80h-80Zm80 80v-80h80v80h-80Z'
    'm80-80v-80h80v80h-80Zm0-160v-80h80v80h-80Zm80 80v-80h80v80h-80Z"/></svg>'
)

ICON_SIZE = 24


def rasterize_svg(svg_string, size=(ICON_SIZE, ICON_SIZE)):
    drawing = svg2rlg(io.BytesIO(svg_string.encode("utf-8")))
    if drawing is None:
        raise RuntimeError("svg2rlg returned None")
    # renderPM expects drawing; output PNG bytes
    png_data = renderPM.drawToString(drawing, fmt="PNG", dpi=96)
    img = Image.open(io.BytesIO(png_data)).convert("L")
    img = img.resize(size, Image.Resampling.NEAREST)
    bitmap = []
    for y in range(size[1]):
        row = []
        for x in range(size[0]):
            # Background is white (255); icon pixels are black (0) after render.
            # We want filled pixels = 1, background = 0.
            pixel = img.getpixel((x, y))
            row.append(1 if pixel < 128 else 0)
        bitmap.append(row)
    return bitmap


def bitmap_to_c(bitmap, name):
    lines = []
    lines.append(f"static const uint8_t {name}[{ICON_SIZE}][{ICON_SIZE}] = {{")
    for row in bitmap:
        lines.append("    { " + ", ".join(str(b) for b in row) + " },")
    lines.append("};")
    return "\n".join(lines)


def main():
    settings_bitmap = rasterize_svg(SETTINGS_SVG)
    qr_bitmap = rasterize_svg(QR_CODE_SVG)

    out = []
    out.append("/* Auto-generated from Material Symbols Outlined icons (Apache 2.0). */")
    out.append("#ifndef UI_ICON_BITMAPS_H")
    out.append("#define UI_ICON_BITMAPS_H")
    out.append("")
    out.append(bitmap_to_c(settings_bitmap, "icon_settings"))
    out.append("")
    out.append(bitmap_to_c(qr_bitmap, "icon_qr_code"))
    out.append("")
    out.append("#endif /* UI_ICON_BITMAPS_H */")
    print("\n".join(out))


if __name__ == "__main__":
    main()
