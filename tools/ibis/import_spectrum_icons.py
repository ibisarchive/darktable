#!/usr/bin/env python3
"""Copy the Spectrum 2 Workflow icons an Ibis icon set refers to.

Reads themes/icons/<set>/map.txt, finds every SVG name on its right-hand
side and copies S2_Icon_<Name>_20_N.svg from an unpacked
@adobe/spectrum-css-workflow-icons npm package (Apache-2.0) into the set
directory as <Name>.svg. The fill is normalised to black: darktable paints
the file as a mask with the CSS color, so the fill only needs to be opaque.

    npm pack @adobe/spectrum-css-workflow-icons   # or curl the tarball
    tar xzf spectrum-css-workflow-icons-*.tgz
    python tools/ibis/import_spectrum_icons.py --package package \\
        --set data/themes/icons/spectrum

Names in map.txt that the package does not have are listed at the end so
the map can be corrected.
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

FILL_RE = re.compile(r'fill="var\(--iconPrimary,\s*#[0-9a-fA-F]{3,6}\)"')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", required=True, type=Path, help="unpacked npm package directory")
    ap.add_argument("--set", required=True, type=Path, help="icon set directory holding map.txt")
    ap.add_argument("--prune", action="store_true", help="delete SVGs the map no longer uses")
    args = ap.parse_args()

    svg_dir = args.package / "icons" / "assets" / "svg"
    if not svg_dir.is_dir():
        print(f"no icons in {svg_dir}", file=sys.stderr)
        return 1

    wanted = set()
    for line in (args.set / "map.txt").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("@"):
            continue
        if "=" in line:
            wanted.add(line.split("=", 1)[1].strip())

    missing = []
    for name in sorted(wanted):
        src = svg_dir / f"S2_Icon_{name}_20_N.svg"
        if not src.is_file():
            missing.append(name)
            continue
        text = src.read_text(encoding="utf-8")
        text = FILL_RE.sub('fill="#000"', text)
        (args.set / f"{name}.svg").write_text(text, encoding="utf-8", newline="\n")

    for legal in ("LICENSE", "COPYRIGHT"):
        src = args.package / legal
        if src.is_file():
            shutil.copyfile(src, args.set / f"{legal}-spectrum.txt")

    if args.prune:
        for svg in args.set.glob("*.svg"):
            if svg.stem not in wanted:
                svg.unlink()

    print(f"{len(wanted) - len(missing)} icons copied to {args.set}")
    if missing:
        print("not in the package: " + ", ".join(missing))
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
