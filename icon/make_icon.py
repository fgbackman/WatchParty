#!/usr/bin/env python3
"""Renders the WatchParty app icon as a macOS .iconset (PNG set).

Usage: python3 icon/make_icon.py            -> icon/WatchParty.iconset/*.png
Then:  iconutil -c icns icon/WatchParty.iconset -o Resources/WatchParty.icns
"""
import math
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageOps
except ImportError:
    sys.exit("Pillow is required: python3 -m pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "WatchParty.iconset")
SS = 4                      # supersampling factor
N = 1024 * SS               # master canvas


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def gradient(size, top, bottom):
    g = Image.linear_gradient("L").resize((size, size))
    return ImageOps.colorize(g, black=top, white=bottom).convert("RGBA")


def rounded_mask(size, box, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m


def person(draw, cx, cy, s, fill):
    """A simple person glyph: head + rounded shoulders, centered at (cx, cy), height ~s."""
    head_r = s * 0.19
    draw.ellipse([cx - head_r, cy - s * 0.5, cx + head_r, cy - s * 0.5 + 2 * head_r], fill=fill)
    body_w = s * 0.72
    body_top = cy - s * 0.5 + 2 * head_r + s * 0.06
    body_bottom = cy + s * 0.5
    draw.rounded_rectangle([cx - body_w / 2, body_top, cx + body_w / 2, body_bottom + body_w],
                           radius=body_w / 2, fill=fill)


def render_master():
    # macOS icon grid: the shape fills ~824/1024 of the canvas, corner radius ~22.4%.
    margin = int(100 * SS)
    box = [margin, margin, N - margin, N - margin]
    radius = int(185 * SS)

    # background gradient (indigo -> violet) with a soft diagonal light
    bg = gradient(N, (62, 44, 196), (156, 48, 210))
    light = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ld = ImageDraw.Draw(light)
    ld.ellipse([-N * 0.2, -N * 0.45, N * 0.75, N * 0.5], fill=(255, 255, 255, 46))
    light = light.filter(ImageFilter.GaussianBlur(N * 0.12))
    bg = Image.alpha_composite(bg, light)

    canvas = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    shape = rounded_mask(N, box, radius)

    # drop shadow (subtle, like Apple's)
    shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 90), box=None, mask=shape)
    shadow = shadow.filter(ImageFilter.GaussianBlur(14 * SS))
    canvas = Image.alpha_composite(canvas, Image.new("RGBA", (N, N), (0, 0, 0, 0)))
    offset = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    offset.paste(shadow, (0, int(8 * SS)))
    canvas = Image.alpha_composite(canvas, offset)

    body = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    body.paste(bg, mask=shape)
    canvas = Image.alpha_composite(canvas, body)

    fg = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(fg)
    cx, cy = N / 2, N / 2

    # screen: a rounded rectangle with a translucent fill and a crisp border
    sw, sh = N * 0.56, N * 0.37
    sx0, sy0 = cx - sw / 2, cy - sh / 2 - N * 0.075
    d.rounded_rectangle([sx0, sy0, sx0 + sw, sy0 + sh], radius=N * 0.045, fill=(255, 255, 255, 54),
                        outline=(255, 255, 255, 235), width=int(N * 0.016))

    # play triangle inside the screen
    tri_h = sh * 0.46
    tri_w = tri_h * 0.92
    tcx, tcy = cx + tri_w * 0.08, sy0 + sh / 2
    d.polygon([(tcx - tri_w / 2, tcy - tri_h / 2), (tcx - tri_w / 2, tcy + tri_h / 2), (tcx + tri_w / 2, tcy)],
              fill=(255, 255, 255, 245))

    # the audience: three people under the screen
    ps = N * 0.13
    py = sy0 + sh + N * 0.045 + ps * 0.5
    for i, dx in enumerate((-1, 0, 1)):
        person(d, cx + dx * ps * 1.05, py + (0 if dx == 0 else ps * 0.06), ps * (1.0 if dx == 0 else 0.9),
               (255, 255, 255, 240 if dx == 0 else 205))

    # clip the foreground to the icon shape (shoulders may overrun the bottom edge)
    clipped = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    clipped.paste(fg, mask=shape)
    canvas = Image.alpha_composite(canvas, clipped)
    return canvas


def main():
    os.makedirs(OUT, exist_ok=True)
    master = render_master().resize((1024, 1024), Image.LANCZOS)
    master.save(os.path.join(HERE, "WatchParty-1024.png"))
    sizes = [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)]
    for pt, scale in sizes:
        px = pt * scale
        name = f"icon_{pt}x{pt}.png" if scale == 1 else f"icon_{pt}x{pt}@2x.png"
        master.resize((px, px), Image.LANCZOS).save(os.path.join(OUT, name))
    print(f"wrote {len(sizes)} PNGs to {OUT}")


if __name__ == "__main__":
    main()
