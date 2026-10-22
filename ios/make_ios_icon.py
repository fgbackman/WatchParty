#!/usr/bin/env python3
"""The iPhone app icon: the Mac icon's artwork, full-bleed and square (iOS rounds the corners itself).

Usage: python3 ios/make_ios_icon.py  -> ios/Assets.xcassets/AppIcon.appiconset/icon-1024.png
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "icon"))
import make_icon as mi
from PIL import Image, ImageDraw, ImageFilter

N, SS = mi.N, mi.SS
bg = mi.gradient(N, (62, 44, 196), (156, 48, 210))
light = Image.new("RGBA", (N, N), (0, 0, 0, 0))
ImageDraw.Draw(light).ellipse([-N * 0.2, -N * 0.45, N * 0.75, N * 0.5], fill=(255, 255, 255, 46))
bg = Image.alpha_composite(bg, light.filter(ImageFilter.GaussianBlur(N * 0.12)))

fg = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(fg)
cx, cy = N / 2, N / 2
sw, sh = N * 0.56, N * 0.37
sx0, sy0 = cx - sw / 2, cy - sh / 2 - N * 0.075
d.rounded_rectangle([sx0, sy0, sx0 + sw, sy0 + sh], radius=N * 0.045, fill=(255, 255, 255, 54),
                    outline=(255, 255, 255, 235), width=int(N * 0.016))
tri_h = sh * 0.46
tri_w = tri_h * 0.92
tcx, tcy = cx + tri_w * 0.08, sy0 + sh / 2
d.polygon([(tcx - tri_w / 2, tcy - tri_h / 2), (tcx - tri_w / 2, tcy + tri_h / 2), (tcx + tri_w / 2, tcy)], fill=(255, 255, 255, 245))
ps = N * 0.13
py = sy0 + sh + N * 0.045 + ps * 0.5
for dx in (-1, 0, 1):
    mi.person(d, cx + dx * ps * 1.05, py + (0 if dx == 0 else ps * 0.06), ps * (1.0 if dx == 0 else 0.9),
              (255, 255, 255, 240 if dx == 0 else 205))
art = Image.alpha_composite(bg, fg)
m = int(70 * SS)                                   # a little tighter than the Mac grid
art = art.crop((m, m, N - m, N - m)).convert("RGB").resize((1024, 1024), Image.LANCZOS)
out = os.path.join(HERE, "Assets.xcassets", "AppIcon.appiconset", "icon-1024.png")
art.save(out)
print("wrote", out)
