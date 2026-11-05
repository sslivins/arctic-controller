#!/usr/bin/env python3
"""Print the Arctic Heat Pumps lockup as one compact SVG for the activity report.

Same mark as the boot animation (gen_boot_art.py): the iceberg icon beside the
ARCTIC wordmark over the HEAT PUMPS tagline. The lettering is converted to
Anton outlines, so the report needs no font. Paste the output into LOGO in
main/web/report.js.

Requires: fonttools.

    python tools/boot_art/gen_report_logo.py
    python tools/boot_art/gen_report_logo.py --icon  # the icon alone, as the favicon data URI
                                                     # for <link rel="icon"> in main/web/index.html
"""
import os
import sys
import urllib.parse

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = TTFont(os.path.join(HERE, "Anton-Regular.ttf"))
GLYPHS, CMAP = FONT.getGlyphSet(), FONT.getBestCmap()
CAP = FONT["OS/2"].sCapHeight

BLUE, CYAN, ORANGE = "#0f75bc", "#01aff0", "#f7941e"
S = 100  # icon size; everything is in these units
WL = 52  # waterline, as in gen_boot_art.py (0.52)


def n(v):
    return f"{v:.1f}".rstrip("0").rstrip(".")


def pts(p):
    return " ".join(f"{n(S / 2 + u * S)},{n(WL + v * S)}" for u, v in p)


def text_path(txt, x, baseline, cap, tracking=0.0):
    """Outline txt with its cap height scaled to cap; returns (path, width)."""
    k = cap / CAP
    pen = SVGPathPen(GLYPHS, ntos=n)
    pos = 0.0
    for i, ch in enumerate(txt):
        g = CMAP[ord(ch)]
        if ch != " ":
            GLYPHS[g].draw(TransformPen(pen, (k, 0, 0, -k, x + pos, baseline)))
        pos += GLYPHS[g].width * k + (tracking if i < len(txt) - 1 else 0)
    return pen.getCommands(), pos


def main():
    tx = S + 16
    arctic, w = text_path("ARCTIC", tx, 66, 62)
    tag = "HEAT PUMPS"
    _, w0 = text_path(tag, 0, 0, 20)
    pumps, _ = text_path(tag, tx, 98, 20, (w - w0) / (len(tag) - 1))  # spread to the wordmark's width
    berg = pts([(-0.42, 0), (-0.26, -0.17), (-0.15, -0.24), (-0.03, -0.40), (0.04, -0.33),
                (0.13, -0.27), (0.24, -0.15), (0.42, 0)])
    under = pts([(-0.42, 0), (0.42, 0), (0.34, 0.16), (0.22, 0.24), (0.14, 0.40), (0.02, 0.31),
                 (-0.12, 0.42), (-0.22, 0.26), (-0.33, 0.14)])
    r = n(S * 18 / 260)
    icon = (f'<clipPath id="arcticIcon"><rect width="{S}" height="{S}" rx="{r}"/></clipPath>'
            f'<g clip-path="url(#arcticIcon)"><rect width="{S}" height="{S}" fill="{ORANGE}"/><rect y="{WL}" width="{S}" height="{S - WL}" fill="{BLUE}"/>'
            f'<polygon points="{berg}" fill="#fff"/><polygon points="{under}" fill="{CYAN}"/></g>')
    if "--icon" in sys.argv:
        svg = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {S} {S}">{icon}</svg>'
        print("data:image/svg+xml," + urllib.parse.quote(svg, safe=" =:/,.'()"))
        return
    print(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {n(tx + w)} {S}" role="img" aria-label="Arctic Heat Pumps">{icon}'
          f'<path d="{arctic}" fill="{BLUE}"/><path d="{pumps}" fill="{ORANGE}"/></svg>')


if __name__ == "__main__":
    main()
