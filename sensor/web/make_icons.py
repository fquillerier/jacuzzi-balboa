"""Draw the home-screen icons (web/icon-180.png, icon-192.png, icon-512.png).

Only needed when the icon design changes; the PNGs are committed.
Requires Pillow:  pip install pillow
"""
import math
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024  # draw large, then downscale for smooth edges
BG = (14, 116, 144)  # #0e7490
FG = (255, 255, 255)

img = Image.new("RGB", (S, S), BG)
d = ImageDraw.Draw(img)

# three water waves
for i, y0 in enumerate((560, 680, 800)):
    pts = []
    for x in range(150, S - 150 + 1, 4):
        pts.append((x, y0 + 38 * math.sin((x - 150) / (S - 300) * 2 * math.pi * 2 + i * 0.9)))
    d.line(pts, fill=FG, width=46, joint="curve")
    for p in (pts[0], pts[-1]):
        d.ellipse([p[0] - 23, p[1] - 23, p[0] + 23, p[1] + 23], fill=FG)

# bubbles
for cx, cy, r in ((380, 360, 52), (540, 250, 72), (680, 390, 44)):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=FG, width=30)

for size in (180, 192, 512):
    small = img.resize((size, size), Image.LANCZOS).quantize(colors=16)
    small.save(os.path.join(HERE, f"icon-{size}.png"), optimize=True)
print("icons written")
