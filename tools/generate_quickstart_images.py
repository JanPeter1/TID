#!/usr/bin/env python3
"""Draw the pictures used by docs/kurzanleitung.md (requires Pillow).

The drawings follow the 320 x 240 layout implemented in main/ui.c; colors are
lightened a little so they stay readable in print. Rerun the script after
changing the layout or the footer icons:

    python tools/generate_quickstart_images.py [--output docs/img]
"""

import argparse
import math
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

K = 6        # render pixels per logical pixel
OUT_DIV = 2  # final size = render size / OUT_DIV (3 px per logical pixel)

BG = (12, 16, 24)
PANEL = (34, 42, 58)
PANEL_LIGHT = (62, 74, 98)
ORANGE = (255, 165, 0)
WHITE = (255, 255, 255)
MUTED = (165, 170, 180)
GREEN = (40, 220, 100)
RED = (215, 45, 45)
INK = (30, 30, 30)
PAPER = (255, 255, 255)

FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")


@lru_cache(maxsize=None)
def font(size, bold=False):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    px = max(1, int(size * K))
    try:
        return ImageFont.truetype(str(FONT_DIR / name), px)
    except OSError:
        return ImageFont.load_default(px)


class Canvas:
    """Drawing surface addressed in logical pixels (plus an optional offset)."""

    def __init__(self, width, height, background, ox=0, oy=0):
        self.img = Image.new("RGB", (round(width * K), round(height * K)),
                             background)
        self.d = ImageDraw.Draw(self.img)
        self.ox, self.oy = ox, oy

    def X(self, x):
        return round((x + self.ox) * K)

    def Y(self, y):
        return round((y + self.oy) * K)

    @staticmethod
    def W(w):
        return max(1, round(w * K))

    def rect(self, x, y, w, h, fill=None, radius=0, outline=None, width=1):
        box = [self.X(x), self.Y(y), self.X(x + w) - 1, self.Y(y + h) - 1]
        if radius:
            self.d.rounded_rectangle(box, round(radius * K), fill=fill,
                                     outline=outline, width=self.W(width))
        else:
            self.d.rectangle(box, fill=fill, outline=outline,
                             width=self.W(width))

    def circle(self, cx, cy, r, fill=None, outline=None, width=1):
        self.d.ellipse([self.X(cx - r), self.Y(cy - r),
                        self.X(cx + r), self.Y(cy + r)],
                       fill=fill, outline=outline, width=self.W(width))

    def arc(self, cx, cy, r, start, end, color, width=1):
        self.d.arc([self.X(cx - r), self.Y(cy - r),
                    self.X(cx + r), self.Y(cy + r)],
                   start, end, fill=color, width=self.W(width))

    def line(self, x0, y0, x1, y1, color, width=1):
        self.d.line([self.X(x0), self.Y(y0), self.X(x1), self.Y(y1)],
                    fill=color, width=self.W(width))

    def polyline(self, points, color, width=1):
        self.d.line([(self.X(x), self.Y(y)) for x, y in points],
                    fill=color, width=self.W(width), joint="curve")

    def text(self, x, y, s, color, size, bold=False, anchor="la"):
        self.d.text((self.X(x), self.Y(y)), s, font=font(size, bold),
                    fill=color, anchor=anchor)

    def save(self, path):
        size = (self.img.width // OUT_DIV, self.img.height // OUT_DIV)
        self.img.resize(size, Image.LANCZOS).save(path, optimize=True)


# --- icons (same shapes as draw_icon() and the footer icons in ui.c) --------

def tab_icon(c, index, cx, cy, color=WHITE, w=1.3):
    if index == 0:  # Aktuell: clock
        c.circle(cx, cy, 9, outline=color, width=w)
        c.line(cx, cy, cx, cy - 6, color, w)
        c.line(cx, cy, cx + 5, cy + 2, color, w)
    elif index == 1:  # Kalender
        c.rect(cx - 9, cy - 7, 18, 15, outline=color, width=w)
        c.line(cx - 9, cy - 2, cx + 9, cy - 2, color, w)
        c.line(cx - 4, cy - 10, cx - 4, cy - 5, color, w)
        c.line(cx + 4, cy - 10, cx + 4, cy - 5, color, w)
        c.rect(cx - 5, cy + 1, 2, 2, fill=color)
        c.rect(cx + 1, cy + 1, 2, 2, fill=color)
    elif index == 2:  # Fotos
        c.rect(cx - 10, cy - 7, 20, 14, outline=color, width=w)
        c.circle(cx + 5, cy - 3, 2, fill=color)
        c.polyline([(cx - 8, cy + 5), (cx - 2, cy - 1), (cx + 2, cy + 3),
                    (cx + 5, cy + 1)], color, w)
    elif index == 3:  # Wetter: sun
        c.circle(cx, cy, 4, outline=color, width=w)
        for k in range(8):
            a = k * math.pi / 4
            c.line(cx + 7 * math.cos(a), cy + 7 * math.sin(a),
                   cx + 10 * math.cos(a), cy + 10 * math.sin(a), color, w)
    else:  # Info
        c.circle(cx, cy, 9, outline=color, width=w)
        c.rect(cx - 1, cy - 1, 2, 7, fill=color)
        c.circle(cx, cy - 5, 1.2, fill=color)


def qr_icon(c, cx, cy, z=1.0, color=WHITE):
    x0, y0 = cx - 7 * z, cy - 7 * z

    def finder(x, y):
        c.rect(x, y, 6 * z, 6 * z, outline=color, width=1.1 * z)
        c.rect(x + 2 * z, y + 2 * z, 2 * z, 2 * z, fill=color)

    finder(x0, y0)
    finder(x0 + 9 * z, y0)
    finder(x0, y0 + 9 * z)
    for dx, dy in ((9, 9), (12, 12), (9, 13), (13, 9)):
        c.rect(x0 + dx * z, y0 + dy * z, 2 * z, 2 * z, fill=color)


def moon_icon(c, cx, cy, z=1.0, bg=PANEL):
    c.circle(cx, cy, 6 * z, fill=WHITE)
    # Bite circle kept inside the 16 px footer so it does not stick out.
    c.circle(cx + 3 * z, cy - 2 * z, 5.5 * z, fill=bg)


def gear_icon(c, cx, cy, z=1.0, color=WHITE, bg=PANEL):
    for k in range(8):
        a = k * math.pi / 4
        c.line(cx + 4 * z * math.cos(a), cy + 4 * z * math.sin(a),
               cx + 7.5 * z * math.cos(a), cy + 7.5 * z * math.sin(a),
               color, 2.4 * z)
    c.circle(cx, cy, 5.5 * z, fill=color)
    c.circle(cx, cy, 2.4 * z, fill=bg)


def wifi_icon(c, cx, cy, color=GREEN, bars=4):
    for i in range(4):
        h = 3 + i * 2
        c.rect(cx - 6 + i * 3, cy + 5 - h, 2, h,
               fill=color if i < bars else MUTED)


# --- the display ------------------------------------------------------------

def draw_ota_panel(c, text, percent=None):
    """Status panel as drawn by draw_ota_progress() in ui.c."""
    bx, by, bw, bh = 40, 138, 240, 12
    c.rect(bx - 8, by - 20, bw + 16, 52, BG)
    if percent is not None:
        c.rect(bx - 2, by - 2, bw + 4, bh + 4, ORANGE)
        c.rect(bx, by, bw, bh, BG)
        c.rect(bx, by, bw * percent // 100, bh, WHITE)
        c.text(bx + bw / 2, by - 10, f"{percent}%", ORANGE, 10, True, "mm")
    c.text(bx, by + bh + 4, text, ORANGE, 11, True)


def draw_screen(c, active=0, ota=None):
    """One 320 x 240 screen with an example page ("Aktuell")."""
    c.rect(0, 0, 320, 240, BG)
    for i in range(5):  # tab bar
        x = i * 64
        c.rect(x + 2, 2, 60, 29, ORANGE if i == active else PANEL, radius=3)
        tab_icon(c, i, x + 32, 16)

    c.text(34, 40, "HEUTE AUF TEXEL", ORANGE, 11, True)  # example content
    c.text(34, 60, "Beispiel-Veranstaltung", WHITE, 15, True)
    c.text(34, 86, "10:00   14:00", ORANGE, 11)
    c.text(34, 104, "De Cocksdorp", MUTED, 11)
    for i, w in enumerate((220, 180, 200)):
        c.rect(34, 130 + i * 16, w, 6, PANEL, radius=3)

    c.rect(2, 96, 24, 64, PANEL, radius=6)  # side buttons
    c.rect(294, 96, 24, 64, PANEL, radius=6)
    c.text(14, 128, "<", ORANGE, 20, True, "mm")
    c.text(306, 128, ">", ORANGE, 20, True, "mm")

    c.rect(0, 224, 320, 16, PANEL)  # footer
    c.text(4, 232, "14:32", ORANGE, 10, False, "lm")
    c.text(48, 232, "21.11.2026", WHITE, 10, False, "lm")
    c.text(112, 232, "1/3", ORANGE, 10, False, "lm")
    wifi_icon(c, 186, 231)
    c.arc(214, 232, 6.5, 0, 360, MUTED, 1.6)  # page timer ring
    c.arc(214, 232, 6.5, -90, 0, ORANGE, 1.6)
    qr_icon(c, 242, 232)
    moon_icon(c, 270, 232)
    gear_icon(c, 298, 232)
    if ota:
        draw_ota_panel(c, *ota)


def overview(path):
    c = Canvas(368, 300, PAPER, ox=4, oy=26)
    draw_screen(c)
    c.rect(0, 0, 320, 240, outline=INK, width=1.2)

    def callout(n, bx, by, tx, ty):
        c.line(bx, by, tx, ty, RED, 1.4)
        c.circle(tx, ty, 2.2, fill=RED)
        c.circle(bx, by, 9, fill=RED, outline=WHITE, width=1.2)
        c.text(bx, by + 0.5, str(n), WHITE, 11, True, "mm")

    callout(1, 160, -16, 160, 6)
    callout(2, 338, 70, 240, 70)
    callout(3, 338, 128, 312, 128)
    for n, x, tx in ((4, 52, 55), (5, 124, 121), (6, 186, 186),
                     (7, 214, 214), (8, 242, 242), (9, 270, 270),
                     (10, 298, 298)):
        callout(n, x, 258, tx, 236)
    c.save(path)


def footer_functions(path):
    c = Canvas(560, 135, PAPER)

    def tap(cx, cy):
        c.circle(cx, cy, 14, outline=ORANGE, width=0.8)
        c.circle(cx, cy, 9, outline=ORANGE, width=1.0)
        c.circle(cx, cy, 4, fill=ORANGE)

    def hold(cx, cy):
        c.circle(cx, cy, 13, outline=MUTED, width=1.5)
        c.arc(cx, cy, 13, -90, 200, ORANGE, 2.2)
        c.text(cx, cy, "2 s", WHITE, 9, True, "mm")

    def tabs_icon(cx, cy):
        for i in range(5):
            c.rect(cx - 40 + i * 16.5, cy - 5, 14, 9,
                   ORANGE if i == 1 else PANEL_LIGHT, radius=2)

    cards = [
        (0, 120, lambda cx: qr_icon(c, cx, 30, 2.2),
         [(tap, "Tippen", "QR-Code zeigen")]),
        (130, 120, lambda cx: moon_icon(c, cx, 30, 2.2),
         [(tap, "Tippen", "Ruhemodus")]),
        (260, 170, lambda cx: gear_icon(c, cx, 30, 2.2),
         [(tap, "Tippen", "Setup-WLAN"), (hold, "Halten", "Update")]),
        (440, 120, lambda cx: tabs_icon(cx, 30),
         [(hold, "Halten", "Aktualisieren")]),
    ]
    for x, w, icon, items in cards:
        c.rect(x, 0, w, 135, PANEL, radius=8)
        icon(x + w / 2)
        for k, (picto, gesture, label) in enumerate(items):
            cx = x + w / 2 if len(items) == 1 else x + w * (0.28 + 0.44 * k)
            picto(cx, 82)
            c.text(cx, 103, gesture, MUTED, 8, False, "mm")
            c.text(cx, 118, label, WHITE, 9.5, True, "mm")
    c.save(path)


def update_states(path):
    states = [("OTA: PRUEFE", None, "Update wird gesucht"),
              ("OTA: DOWNLOAD", 42, "Update wird geladen"),
              ("OTA: AKTUELL", None, "Firmware ist aktuell")]
    cw, ch, gap = 256, 52, 10  # crop = the status panel (y 118..170)
    out = Canvas(3 * cw + 2 * gap, ch + 20, PAPER)
    for i, (text, percent, caption) in enumerate(states):
        screen = Canvas(320, 240, BG)
        draw_screen(screen, ota=(text, percent))
        crop = screen.img.crop((32 * K, 118 * K, 288 * K, 170 * K))
        x = i * (cw + gap)
        out.img.paste(crop, (round(x * K), 0))
        out.rect(x, 0, cw, ch, outline=INK, width=0.8)
        out.text(x + cw / 2, ch + 10, caption, INK, 10, True, "mm")
    out.save(path)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--output", type=Path, default=root / "docs" / "img",
                        help="target directory (default: docs/img)")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    overview(args.output / "kurzanleitung_display.png")
    footer_functions(args.output / "kurzanleitung_fusszeile.png")
    update_states(args.output / "kurzanleitung_update.png")
    print(f"Images written to {args.output}")


if __name__ == "__main__":
    main()
