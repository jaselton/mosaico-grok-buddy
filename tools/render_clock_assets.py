#!/usr/bin/env python3
"""Render the world-timer clock face assets (original artwork).

Outputs (480x480 unless noted) into components/app_clock/assets/:
  worldtimer_dial.png  static dial: sunburst, city disc, minute track, numerals,
                       sub-dial faces, date frame, wordmark (no moving parts)
  worldtimer_ring.png  RGBA day/night 24 h disc with 0 h at the top; rotated
                       on the device every minute
  digits.png           10 date-wheel digits, 28x30 each, side by side

The moving parts (hands, ring rotation, date digits, gauges, the alarm ON/OFF
window) are drawn by components/app_clock at runtime.

Usage: python3 tools/render_clock_assets.py [--wordmark GROK] [--subtitle Worldtimer]
Needs Pillow. Fonts come from components/app_buddy/ui/fonts (DejaVu).
"""
import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "components/app_buddy/ui/fonts"
OUT = ROOT / "components/app_clock/assets"

W = 480
S = 4                      # supersampling
N = W * S
C = N / 2

# Geometry shared with app_clock.c (native pixels at 480).
R_DIAL = 234
R_CITY_OUT, R_CITY_IN = 232, 204
R_RING_OUT, R_RING_IN = 201, 173
R_TRACK = 170
SUB3 = (240 + 80, 240 + 12, 44)      # 30-minute counter: centre x, y, radius
SUB7 = (240 - 62, 240 + 90, 44)      # alarm reserve (battery)
ONOFF = (0, 0, 16)                   # filled below from the numeral radius
DATE = (0, 0)                        # date window centre, filled below

CITIES = ["LONDON", "PARIS", "CAIRO", "MOSCOW", "DUBAI", "KARACHI", "DHAKA", "BANGKOK", "HONG KONG",
          "TOKYO", "SYDNEY", "NOUMEA", "AUCKLAND", "MIDWAY", "HAWAII", "ANCHORAGE", "L.ANGELES", "DENVER",
          "CHICAGO", "NEW YORK", "CARACAS", "RIO", "S.GEORGIA", "AZORES"]


def font(sz, bold=True):
    return ImageFont.truetype(str(FONTS / ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")), int(sz))


def pol(r, deg, cx=C, cy=C):
    a = math.radians(deg - 90)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def arc_text(im, text, r, center_deg, sz, col, outward=True):
    f = font(sz * S)
    widths = [f.getlength(ch) for ch in text]
    total = sum(widths)
    ang = math.degrees(total / r)
    a = center_deg - ang / 2 if outward else center_deg + ang / 2
    for ch, wd in zip(text, widths):
        step = math.degrees(wd / r)
        mid = a + step / 2 if outward else a - step / 2
        t = Image.new("RGBA", (int(wd) + 6 * S, int(sz * S * 1.6)), (0, 0, 0, 0))
        ImageDraw.Draw(t).text((3 * S, 0), ch, font=f, fill=col)
        t = t.rotate(-mid if outward else 180 - mid, resample=Image.BICUBIC, expand=True)
        x, y = pol(r, mid)
        im.paste(t, (int(x - t.size[0] / 2), int(y - t.size[1] / 2)), t)
        a = a + step if outward else a - step


def annulus_mask(r_out, r_in):
    m = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(m)
    d.ellipse((C - r_out, C - r_out, C + r_out, C + r_out), fill=255)
    d.ellipse((C - r_in, C - r_in, C + r_in, C + r_in), fill=0)
    return m


def dial(wordmark, subtitle):
    im = Image.new("RGB", (N, N), (0, 0, 0))
    R = R_DIAL * S
    sun = Image.new("L", (N, N), 0)
    sd = ImageDraw.Draw(sun)
    for k in range(720):
        a = k * 0.5
        v = 110 + int(26 * math.sin(math.radians(a * 8)) + 16 * math.sin(math.radians(a * 3)))
        sd.polygon([(C, C), pol(R * 1.5, a), pol(R * 1.5, a + 0.6)], fill=v)
    sun = sun.filter(ImageFilter.GaussianBlur(2 * S))
    sunburst = Image.composite(Image.new("RGB", (N, N), (38, 74, 156)), Image.new("RGB", (N, N), (16, 36, 90)), sun)
    m = Image.new("L", (N, N), 0)
    ImageDraw.Draw(m).ellipse((C - R, C - R, C + R, C + R), fill=255)
    im.paste(sunburst, (0, 0), m)
    d = ImageDraw.Draw(im)

    # City disc
    im.paste((14, 30, 76), (0, 0), annulus_mask(R_CITY_OUT * S, R_CITY_IN * S))
    for i, cn in enumerate(CITIES):
        deg = (i * 15 + 90) % 360           # LONDON at 3 o'clock; i*15 degrees per zone
        top = deg >= 272 or deg <= 88
        size = 8.6 if len(cn) <= 7 else 7.4
        rmid = (R_CITY_OUT + R_CITY_IN) / 2 * S + (3.5 * S if top else -3.5 * S)
        arc_text(im, cn, rmid, deg, size, (238, 240, 246), outward=top)
        sep = deg + 7.5
        d.line((pol(R_CITY_OUT * S - 4 * S, sep), pol(R_CITY_IN * S + 4 * S, sep)), fill=(62, 88, 152), width=S)
    # Recessed track for the rotating 24 h disc (drawn at runtime)
    im.paste((10, 22, 56), (0, 0), annulus_mask(R_RING_OUT * S, R_RING_IN * S))

    # Minute track
    rt = R_TRACK * S
    for i in range(60):
        d.line((pol(rt, i * 6), pol(rt - (7 if i % 5 else 12) * S, i * 6)), fill=(220, 226, 242), width=(2 if i % 5 else 3) * S)
    # Lume numerals (2, 3, 7, 8 make room for the date, counter and alarm gauges)
    for h in (1, 4, 5, 6, 9, 10, 11, 12):
        x, y = pol(rt - 36 * S, h * 30)
        d.text((x, y), str(h), font=font(30 * S), fill=(244, 240, 226), anchor="mm",
               stroke_width=S, stroke_fill=(20, 30, 60))

    # Date frame above the 30-minute counter (digits drawn at runtime)
    x, y = pol(rt - 50 * S, 54)
    d.rounded_rectangle((x - 31 * S, y - 17 * S, x + 31 * S, y + 17 * S), radius=3 * S, fill=(12, 16, 30))
    global DATE
    DATE = (round(x / S), round(y / S))

    # 30-minute counter face
    sx, sy, rr = (v * S for v in SUB3)
    d.ellipse((sx - rr, sy - rr, sx + rr, sy + rr), fill=(12, 26, 64), outline=(100, 120, 175), width=S)
    for k in range(60):
        d.line((pol(rr - 2 * S, k * 6, sx, sy), pol(rr - (5 if k % 2 else 9) * S, k * 6, sx, sy)),
               fill=(210, 215, 232), width=(1 if k % 2 else 2) * S)
    for v, dg in [("30", 0), ("10", 120), ("20", 240)]:
        x2, y2 = pol(rr - 20 * S, dg, sx, sy)
        d.text((x2, y2), v, font=font(10 * S), fill=(240, 240, 245), anchor="mm")

    # Alarm reserve (battery) face: arc track 150..250 degrees, hand drawn at runtime
    ax, ay, ar = (v * S for v in SUB7)
    d.ellipse((ax - ar, ay - ar, ax + ar, ay + ar), fill=(10, 22, 54), outline=(100, 120, 175), width=S)
    box = (ax - ar + 8 * S, ay - ar + 8 * S, ax + ar - 8 * S, ay + ar - 8 * S)
    d.arc(box, 150, 250, fill=(40, 60, 90), width=7 * S)
    d.arc(box, 228, 250, fill=(70, 200, 120), width=7 * S)
    d.text((ax + 8 * S, ay - 12 * S), "ALARM", font=font(7.5 * S), fill=(240, 240, 245), anchor="mm")
    d.text((ax + 8 * S, ay - 1 * S), "RESERVE", font=font(6.5 * S), fill=(200, 205, 220), anchor="mm")
    # PIL arcs start at 3 o'clock; 250 there is 340 clockwise-from-12, the full end.
    for lbl, deg, col in (("F", 336, (70, 200, 120)), ("E", 246, (200, 205, 220))):
        lx, ly = pol(ar - 21 * S, deg, ax, ay)
        d.text((lx, ly), lbl, font=font(7.5 * S), fill=col, anchor="mm")

    # Alarm ON/OFF window frame (fill drawn at runtime)
    ox, oy = pol(rt - 90 * S, -74)
    d.ellipse((ox - 16 * S, oy - 16 * S, ox + 16 * S, oy + 16 * S), fill=(10, 22, 54), outline=(100, 120, 175), width=S)
    d.text((ox, oy - 25 * S), "ON/OFF", font=font(6.5 * S), fill=(220, 225, 240), anchor="mm")
    global ONOFF
    ONOFF = (round(ox / S), round(oy / S), 11)

    # Wordmark
    d.polygon([(C, C - 122 * S), (C - 6 * S, C - 109 * S), (C + 6 * S, C - 109 * S)], fill=(244, 240, 226))
    d.text((C, C - 93 * S), wordmark, font=font(18 * S), fill=(244, 240, 226), anchor="mm")
    d.text((C, C - 74 * S), subtitle, font=font(11 * S, False), fill=(225, 228, 238), anchor="mm")
    return im.resize((W, W), Image.LANCZOS)


def ring():
    """Day/night disc, 0 h at the top, hours clockwise (15 degrees each)."""
    lay = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    r1, r2 = (R_RING_OUT - 1) * S, (R_RING_IN + 1) * S
    def hd(h):
        return (h * 15) % 360
    ld.pieslice((C - r1, C - r1, C + r1, C + r1), hd(6) - 90, hd(18) - 90, fill=(236, 229, 202, 255))
    ld.pieslice((C - r1, C - r1, C + r1, C + r1), hd(18) - 90, hd(6) - 90 + 360, fill=(18, 40, 96, 255))
    hole = Image.new("L", (N, N), 255)
    ImageDraw.Draw(hole).ellipse((C - r2, C - r2, C + r2, C + r2), fill=0)
    lay.putalpha(Image.composite(lay.getchannel("A"), Image.new("L", (N, N), 0), hole))
    for h in range(24):
        deg = hd(h)
        day = 6 <= h < 18
        top = deg >= 272 or deg <= 88
        rmid = (r1 + r2) / 2 + (3 * S if top else -3 * S)
        arc_text(lay, str(h), rmid, deg, 10.5, (24, 34, 72, 255) if day else (242, 242, 242, 255), outward=top)
    return lay.resize((W, W), Image.LANCZOS)


def digits():
    w, h = 28, 30
    im = Image.new("RGB", (w * 10 * S, h * S), (248, 248, 248))
    d = ImageDraw.Draw(im)
    for k in range(10):
        d.text(((k * w + w / 2) * S, h / 2 * S), str(k), font=font(24 * S), fill=(20, 20, 28), anchor="mm")
    return im.resize((w * 10, h), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wordmark", default="GROK")
    ap.add_argument("--subtitle", default="Worldtimer")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    dial(args.wordmark, args.subtitle).save(OUT / "worldtimer_dial.png", optimize=True)
    ring().save(OUT / "worldtimer_ring.png", optimize=True)
    digits().save(OUT / "digits.png", optimize=True)
    # Positions computed while drawing; app_clock.c must match these.
    print(f"DATE_CENTRE {DATE}  ONOFF {ONOFF}  SUB3 {SUB3}  SUB7 {SUB7}")


if __name__ == "__main__":
    main()
