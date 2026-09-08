#!/usr/bin/env python3
"""Render the Ibis Archive brand assets into data/pixmaps.

Source: the Ibis mark (white bird on a black square, Group 5.png) and the
wordmark text. Everything darktable shows as its own logo is replaced by
a data file, so the C code stays untouched:

    idbutton.png, idbutton-N.png      top-left panel logo (40 px) and seasonal variants
    idbutton.svg, idbutton-N.svg      splash and welcome screens (PNG embedded)
    darktable.svg                     splash program name (wordmark)
    dt_text.svg                       about dialog program name (wordmark)
    dt_logo_128x128.png / .ico        Windows executable and installer icon
    <size>/darktable.png, scalable/*  application icons

Usage: python make_brand_assets.py --mark "Group 5.png" --pixmaps data/pixmaps
"""
import argparse
import base64
import io
import os
import shutil

from PIL import Image, ImageDraw

SIZES = [16, 22, 24, 32, 48, 64, 256]
WORDMARK = "Ibis Archive"


def rounded(mark, size, radius_frac=0.18):
    """The mark scaled to size, with rounded corners so it sits well as an icon."""
    im = mark.convert("RGBA").resize((size, size), Image.LANCZOS)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=int(size * radius_frac), fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(im, (0, 0), mask)
    return out


def png_bytes(im):
    b = io.BytesIO()
    im.save(b, "PNG", optimize=True)
    return b.getvalue()


def svg_with_png(im, size):
    """An SVG that is just the PNG, for the places darktable insists on SVG."""
    data = base64.b64encode(png_bytes(im)).decode("ascii")
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            f'width="{size}" height="{size}" viewBox="0 0 {size} {size}">\n'
            f'  <image width="{size}" height="{size}" xlink:href="data:image/png;base64,{data}"/>\n'
            f'</svg>\n')


def wordmark_svg(fill):
    """The program name in the brand face. rsvg renders the text with the
    system's Times New Roman (or its metric twin)."""
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<svg xmlns="http://www.w3.org/2000/svg" width="480" height="78" viewBox="0 0 480 78">\n'
            f'  <text x="0" y="60" font-family="Times New Roman, Liberation Serif, Tinos, serif" '
            f'font-size="64" fill="{fill}">{WORDMARK}</text>\n'
            '</svg>\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mark", required=True, help="square PNG of the Ibis mark")
    ap.add_argument("--pixmaps", required=True, help="data/pixmaps directory")
    args = ap.parse_args()

    mark = Image.open(args.mark).convert("RGBA")
    px = args.pixmaps

    # panel logo and seasonal variants (darktable swaps these by date; one mark, always)
    small = rounded(mark, 40)
    for name in ["idbutton.png", "idbutton-1.png", "idbutton-2.png", "idbutton-3.png"]:
        small.save(os.path.join(px, name), "PNG", optimize=True)
    big_svg = svg_with_png(rounded(mark, 512), 512)
    for name in ["idbutton.svg", "idbutton-1.svg", "idbutton-2.svg", "idbutton-3.svg"]:
        with open(os.path.join(px, name), "w", encoding="utf-8") as f:
            f.write(big_svg)

    # wordmarks: the splash is dark, the about dialog follows the theme's text
    with open(os.path.join(px, "darktable.svg"), "w", encoding="utf-8") as f:
        f.write(wordmark_svg("#ffffff"))
    with open(os.path.join(px, "dt_text.svg"), "w", encoding="utf-8") as f:
        f.write(wordmark_svg("#c4c4c4"))

    # application icons
    for s in SIZES:
        d = os.path.join(px, f"{s}x{s}")
        os.makedirs(d, exist_ok=True)
        rounded(mark, s).save(os.path.join(d, "darktable.png"), "PNG", optimize=True)
    scal = os.path.join(px, "scalable")
    for name in ["darktable.svg", "darktable-1.svg", "darktable-2.svg", "darktable-3.svg", "darktable_macos_icon.svg"]:
        with open(os.path.join(scal, name), "w", encoding="utf-8") as f:
            f.write(big_svg)

    logo128 = rounded(mark, 128)
    logo128.save(os.path.join(px, "dt_logo_128x128.png"), "PNG", optimize=True)
    rounded(mark, 256).save(os.path.join(px, "dt_logo_128x128.ico"), format="ICO",
                            sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("brand assets written to", px)


if __name__ == "__main__":
    main()
