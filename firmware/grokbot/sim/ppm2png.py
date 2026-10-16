#!/usr/bin/env python3
"""Convert sim/out/*.ppm into a single PNG contact sheet (needs PIL)."""
import glob
import os
from PIL import Image

out = os.path.join(os.path.dirname(__file__), "out")
paths = sorted(glob.glob(os.path.join(out, "*.ppm")))
imgs = [(os.path.basename(p).replace(".ppm", ""), Image.open(p).convert("RGB")) for p in paths]
if not imgs:
    raise SystemExit("no ppm files")
w, h = imgs[0][1].size
cols = 4
rows = (len(imgs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * h), (40, 40, 40))
for i, (name, im) in enumerate(imgs):
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    print(f"{i}: {name}")
sheet.save(os.path.join(out, "sheet.png"))
print("saved", os.path.join(out, "sheet.png"))
