#!/usr/bin/env python3
"""Convert a raw X3 framebuffer dump (792x528, 1bpp, MSB-first, 1 = white) to PNG.

Usage: shot2png.py in.bin out.png [portrait|landscape-left|landscape-right]
The buffer is in native panel orientation; portrait UIs appear rotated unless the orientation is given."""
import sys

from PIL import Image

W, H = 792, 528
data = open(sys.argv[1], "rb").read()
if len(data) != W * H // 8:
    sys.exit(f"unexpected size {len(data)} (X3 buffers are {W * H // 8} bytes)")
im = Image.frombytes("1", (W, H), data)
mode = sys.argv[3] if len(sys.argv) > 3 else "landscape-left"
if mode == "portrait":
    im = im.rotate(-90, expand=True)
elif mode == "landscape-right":
    im = im.rotate(180)
im.save(sys.argv[2])
print("saved", sys.argv[2])
