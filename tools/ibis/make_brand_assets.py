#!/usr/bin/env python3
"""Render the Ibis Archive brand assets into data/pixmaps.

Source: the Ibis mark (white bird on a black square, Group 5.png) and the
program name set in Times New Roman. Everything darktable shows as its own
logo is replaced by a data file, so the C code stays untouched:

    idbutton.png, idbutton-N.png      top-left panel logo (40 px) and seasonal variants
    idbutton.svg, idbutton-N.svg      same, SVG (splash, welcome, panel)
    darktable.svg                     splash program name (wordmark)
    dt_text.svg                       panel / about program name (wordmark)
    dt_logo_128x128.png / .ico        Windows executable and installer icon
    <size>/darktable.png, scalable/*  application icons

darktable draws several of these at the SVG's own width/height (it passes
no size), so the intrinsic sizes below match the stock files exactly:
idbutton.svg 40 x 40, dt_text.svg 103.03 x 18.222, darktable.svg
132.44 x 21.47. The embedded PNGs are rendered larger and scaled down.

Usage: python make_brand_assets.py --mark "Group 5.png" --pixmaps data/pixmaps
"""
import argparse
import base64
import io
import os

from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 22, 24, 32, 48, 64, 256]
WORDMARK = "Ibis Archive"
FONT_FILES = [
    r"C:\Windows\Fonts\times.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "/Library/Fonts/Times New Roman.ttf",
]

# stock intrinsic sizes (px), read from the darktable files being replaced
PANEL_LOGO = 40.0
TEXT_H = 18.222      # dt_text.svg
SPLASH_TEXT_H = 21.47  # darktable.svg (mm in the original, unitless here)


def rounded(mark, size, radius_frac=0.18):
    """The mark scaled to size with rounded corners, so it sits well as an icon."""
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


def svg_with_png(im, width, height):
    """An SVG that is just the PNG. width/height are the display size darktable
    will use when it asks for none; the PNG may be larger and is scaled."""
    data = base64.b64encode(png_bytes(im)).decode("ascii")
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            f'width="{width:g}" height="{height:g}" viewBox="0 0 {width:g} {height:g}">\n'
            f'  <image width="{width:g}" height="{height:g}" preserveAspectRatio="xMidYMid meet" '
            f'xlink:href="data:image/png;base64,{data}"/>\n'
            f'</svg>\n')


def wordmark_png(fill, px=256):
    """The program name in Times New Roman as a tight PNG, so the result does
    not depend on which fonts the viewer's rsvg can see."""
    font = None
    for cand in FONT_FILES:
        if os.path.exists(cand):
            font = ImageFont.truetype(cand, px)
            break
    if font is None:
        raise SystemExit("no Times New Roman / Liberation Serif font file found")
    d = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    x0, y0, x1, y1 = d.textbbox((0, 0), WORDMARK, font=font)
    pad = px // 20
    im = Image.new("RGBA", (x1 - x0 + 2 * pad, y1 - y0 + 2 * pad), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((pad - x0, pad - y0), WORDMARK, font=font, fill=fill)
    return im


def wordmark_svg(fill, height):
    im = wordmark_png(fill)
    return svg_with_png(im, round(height * im.width / im.height, 3), height)


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mark", required=True, help="square PNG of the Ibis mark")
    ap.add_argument("--pixmaps", required=True, help="data/pixmaps directory")
    args = ap.parse_args()

    mark = Image.open(args.mark).convert("RGBA")
    px = args.pixmaps

    # panel logo and the seasonal variants darktable swaps in by date: one mark, always
    small = rounded(mark, int(PANEL_LOGO))
    for name in ["idbutton.png", "idbutton-1.png", "idbutton-2.png", "idbutton-3.png"]:
        small.save(os.path.join(px, name), "PNG", optimize=True)
    panel_svg = svg_with_png(rounded(mark, 512), PANEL_LOGO, PANEL_LOGO)
    for name in ["idbutton.svg", "idbutton-1.svg", "idbutton-2.svg", "idbutton-3.svg"]:
        write(os.path.join(px, name), panel_svg)

    # wordmarks: the splash is dark; the panel mark is a mid grey that reads on both themes
    write(os.path.join(px, "darktable.svg"), wordmark_svg("#ffffff", SPLASH_TEXT_H))
    write(os.path.join(px, "dt_text.svg"), wordmark_svg("#8f8f8f", TEXT_H))

    # application icons
    for s in SIZES:
        d = os.path.join(px, f"{s}x{s}")
        os.makedirs(d, exist_ok=True)
        rounded(mark, s).save(os.path.join(d, "darktable.png"), "PNG", optimize=True)
    icon_svg = svg_with_png(rounded(mark, 512), 512, 512)
    for name in ["darktable.svg", "darktable-1.svg", "darktable-2.svg", "darktable-3.svg", "darktable_macos_icon.svg"]:
        write(os.path.join(px, "scalable", name), icon_svg)

    rounded(mark, 128).save(os.path.join(px, "dt_logo_128x128.png"), "PNG", optimize=True)
    rounded(mark, 256).save(os.path.join(px, "dt_logo_128x128.ico"), format="ICO",
                            sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("brand assets written to", px)


if __name__ == "__main__":
    main()
