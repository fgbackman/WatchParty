#!/usr/bin/env python3
"""Renders the "ate" sticker: a hand making the ASL number 8 (index, ring and
pinky extended; thumb and middle finger pinched). Flat emoji style.

Usage: python3 icon/make_ate.py   -> Resources/ate.png (256px) + Resources/ate@2x.png (512px)
"""
import math
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: python3 -m pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "Resources")
SS = 4
N = 512 * SS

SKIN = (255, 207, 85, 255)        # emoji yellow
SKIN_DARK = (232, 170, 40, 255)   # outline / creases
NAIL = (255, 228, 150, 255)


def P(x, y):
    return (x * SS, y * SS)


def finger(d, x, top, bottom, w, tilt=0.0):
    """A straight finger: rounded capsule from (x, bottom) up to (x+tilt, top)."""
    r = w / 2
    dx = tilt
    pts = []
    # capsule as polygon + round caps
    d.line([P(x, bottom), P(x + dx, top)], fill=SKIN_DARK, width=int((w + 10) * SS))
    d.line([P(x, bottom), P(x + dx, top)], fill=SKIN, width=int(w * SS))
    d.ellipse([P(x + dx - r - 5, top - r - 5), P(x + dx + r + 5, top + r + 5)], fill=SKIN_DARK)
    d.ellipse([P(x + dx - r, top - r), P(x + dx + r, top + r)], fill=SKIN)
    # fingernail
    nr = r * 0.55
    d.ellipse([P(x + dx - nr, top - nr * 1.2), P(x + dx + nr, top + nr * 0.6)], fill=NAIL)


def thick_curve(d, pts, w, color):
    """Stamp circles along the path: no joint artifacts at any width."""
    r = w / 2
    dense = []
    for (a, b) in zip(pts, pts[1:]):
        steps = max(2, int(math.hypot(b[0] - a[0], b[1] - a[1]) / 2))
        for i in range(steps):
            t = i / steps
            dense.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
    dense.append(pts[-1])
    for (x, y) in dense:
        d.ellipse([P(x - r, y - r), P(x + r, y + r)], fill=color)


def bezier(p0, p1, p2, p3, n=40):
    out = []
    for i in range(n + 1):
        t = i / n
        x = (1 - t) ** 3 * p0[0] + 3 * (1 - t) ** 2 * t * p1[0] + 3 * (1 - t) * t ** 2 * p2[0] + t ** 3 * p3[0]
        y = (1 - t) ** 3 * p0[1] + 3 * (1 - t) ** 2 * t * p1[1] + 3 * (1 - t) * t ** 2 * p2[1] + t ** 3 * p3[1]
        out.append((x, y))
    return out


def render():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # right hand, palm facing the viewer: pinky left, thumb right
    # ---- extended fingers (behind the palm top edge) ----
    finger(d, 168, 150, 300, 56, tilt=-6)    # pinky (shortest)
    finger(d, 228, 96, 300, 60, tilt=-2)     # ring
    finger(d, 348, 118, 300, 60, tilt=4)     # index

    # ---- palm ----
    d.rounded_rectangle([P(133, 236), P(387, 450)], radius=70 * SS, fill=SKIN_DARK)
    d.rounded_rectangle([P(138, 241), P(382, 445)], radius=66 * SS, fill=SKIN)
    # wrist shading
    d.rounded_rectangle([P(165, 405), P(355, 445)], radius=30 * SS, fill=(255, 196, 70, 255))

    # ---- middle finger: up from the palm, then bent forward/down into the loop ----
    mid = bezier((290, 262), (288, 150), (335, 150), (336, 258))
    thick_curve(d, mid, 70, SKIN_DARK)
    thick_curve(d, mid, 58, SKIN)

    # ---- thumb: from the lower right up into the loop ----
    th = bezier((378, 428), (430, 340), (392, 318), (352, 318))
    thick_curve(d, th, 74, SKIN_DARK)
    thick_curve(d, th, 62, SKIN)

    # ---- the pinch: thumb tip and middle fingertip form a visible loop ----
    cx, cy = 336, 300
    ro, ri = 58, 20
    d.ellipse([P(cx - ro - 6, cy - ro - 6), P(cx + ro + 6, cy + ro + 6)], fill=SKIN_DARK)
    d.ellipse([P(cx - ro, cy - ro), P(cx + ro, cy + ro)], fill=SKIN)
    d.ellipse([P(cx - ri - 5, cy - ri - 5), P(cx + ri + 5, cy + ri + 5)], fill=SKIN_DARK)
    d.ellipse([P(cx - ri, cy - ri), P(cx + ri, cy + ri)], fill=(214, 150, 40, 255))   # the gap, palm in shadow
    # fingernail on the middle fingertip, top-left of the loop
    d.ellipse([P(cx - 42, cy - 44), P(cx - 10, cy - 14)], fill=NAIL)

    # a little "8" badge on the wrist corner, the pun made explicit
    bx, by, br = 160, 428, 44
    d.ellipse([P(bx - br, by - br), P(bx + br, by + br)], fill=(60, 60, 70, 255))
    d.ellipse([P(bx - br + 6, by - br + 6), P(bx + br - 6, by + br - 6)], fill=(255, 255, 255, 255))
    # draw an 8 with two rings
    for (cy8, rr) in ((by - 14, 15), (by + 13, 18)):
        d.ellipse([P(bx - rr, cy8 - rr), P(bx + rr, cy8 + rr)], fill=(60, 60, 70, 255))
        d.ellipse([P(bx - rr + 8, cy8 - rr + 8), P(bx + rr - 8, cy8 + rr - 8)], fill=(255, 255, 255, 255))

    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    master = render()
    master.resize((512, 512), Image.LANCZOS).save(os.path.join(OUT, "ate@2x.png"))
    master.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT, "ate.png"))
    master.resize((1024, 1024), Image.LANCZOS).save(os.path.join(HERE, "ate-preview.png"))
    print("wrote Resources/ate.png, Resources/ate@2x.png")


if __name__ == "__main__":
    main()
