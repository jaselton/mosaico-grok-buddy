#!/usr/bin/env python3
"""Render Grok Bot-style eye frames for the 480x480 ESP-Mosaico screen.

Each eye is a stadium pill (width = 0.47 x length) leaning 35.5 degrees from
vertical, top toward the left, drawn white on black. Each frame is a 160x160
RGBA PNG; the thinking and blink frames squash the pill vertically about its
centre. Fan-made approximation of the look; not an official xAI asset.
"""
import math, os, sys
from PIL import Image, ImageDraw

L = 149            # pill length in px at 480 (0.31 * screen, as in the approved mocks)
EYE_W = 0.47       # width / length
TILT = 35.5        # degrees from vertical, top toward the left
BOX = 160          # frame size
SS = 8             # supersampling
OPENNESS = {"open": 1.0, "think": 0.72, "b1": 0.45, "b2": 0.2, "b3": 0.07}

def frame(openness):
    big = BOX * SS
    im = Image.new("L", (big, big), 0)
    d = ImageDraw.Draw(im)
    w = L * EYE_W * SS
    h = L * SS
    c = big / 2
    d.rounded_rectangle((c - w / 2, c - h / 2, c + w / 2, c + h / 2), radius=w / 2, fill=255)
    im = im.rotate(TILT, resample=Image.BICUBIC)          # +ve = counter-clockwise: top goes left
    if openness < 1.0:
        sh = max(1, int(round(big * openness)))
        squashed = im.resize((big, sh), Image.LANCZOS)
        im = Image.new("L", (big, big), 0)
        im.paste(squashed, (0, (big - sh) // 2))
    a = im.resize((BOX, BOX), Image.LANCZOS)
    out = Image.new("RGBA", (BOX, BOX), (255, 255, 255, 0))
    out.putalpha(a)
    return out

if __name__ == "__main__":
    dst = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(dst, exist_ok=True)
    for name, o in OPENNESS.items():
        frame(o).save(os.path.join(dst, f"eye_{name}.png"), optimize=True)
        print("wrote", name)
