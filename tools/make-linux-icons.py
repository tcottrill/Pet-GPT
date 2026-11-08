#!/usr/bin/env python3
"""Builds packaging/icons/hicolor from petemu/petemu.ico (the Windows icon).

usage: make-linux-icons.py [ICO] [OUT_DIR]   defaults: petemu/petemu.ico packaging/icons

Writes hicolor/<n>x<n>/apps/io.github.tcottrill.Pet-GPT.png for each icon theme size
up to the largest image in the .ico (256), using the .ico's own image where it has
that size and a downscale of the 256 px image otherwise. Needs Pillow. Run it again
whenever the Windows icon changes, then repackage.
"""
import pathlib
import shutil
import sys

from PIL import Image

APP_ID = 'io.github.tcottrill.Pet-GPT'
LINUX_SIZES = (16, 22, 24, 32, 48, 64, 96, 128, 192, 256)

root = pathlib.Path(__file__).resolve().parent.parent
ico = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'petemu' / 'petemu.ico'
out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else root / 'packaging' / 'icons'

source = Image.open(ico)
available = sorted(source.info['sizes'])
images = {}
for size in available:
    source.size = size
    images[size[0]] = source.copy().convert('RGBA')
largest = images[max(images)]

hicolor = out / 'hicolor'
if hicolor.exists():
    shutil.rmtree(hicolor)
for n in LINUX_SIZES:
    if n > largest.width:
        continue
    image = images.get(n) or largest.resize((n, n), Image.LANCZOS)
    target = hicolor / f'{n}x{n}' / 'apps' / f'{APP_ID}.png'
    target.parent.mkdir(parents=True, exist_ok=True)
    image.save(target)
    print(target.relative_to(root) if target.is_relative_to(root) else target)
