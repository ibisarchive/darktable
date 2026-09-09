#!/usr/bin/env python3
"""Resolve the Adobe Spectrum design tokens Ibis uses into files the theme can read.

Input: an unpacked @adobe/spectrum-tokens npm package (Apache-2.0),
src/*.json in the Spectrum design-data format (token sets per color theme
and platform scale, aliases in {braces}).

Output, in the --out directory:
  spectrum-tokens.json      the curated subset, resolved: every value is a
                            literal per set (light, dark; desktop scale)
  chunk-spectrum-dark.css   GTK CSS @define-color / comment table for a theme
  chunk-spectrum-light.css  to @import; names are spectrum_<token> with dashes
                            as underscores, e.g. @spectrum_gray_800
  LICENSE-spectrum-tokens.txt

    python tools/ibis/spectrum_tokens.py --package <unpacked pkg> --out data/ibis/design --css-out data/themes

The curated list is CURATED below; add a token name there when the theme
needs it, rerun, commit the three files. Dimensions are written as CSS
comments in the chunks (GTK CSS has no custom properties), and as numbers
in the JSON, which DESIGN.md quotes.
"""
import argparse
import json
import re
import shutil
import sys
from pathlib import Path

CURATED = {
    "color": [
        # the neutral ramp, both themes
        *[f"gray-{n}" for n in (25, 50, 75, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000)],
        # semantic surfaces and content
        "background-base-color", "background-layer-1-color", "background-layer-2-color",
        "background-pasteboard-color", "background-elevated-color",
        "neutral-content-color-default", "neutral-content-color-hover", "neutral-content-color-down",
        "neutral-subdued-content-color-default", "neutral-subdued-content-color-hover",
        "disabled-content-color", "disabled-background-color", "disabled-border-color",
        "neutral-background-color-default", "neutral-background-color-hover", "neutral-background-color-down",
        "neutral-subdued-background-color-default",
        "neutral-visual-color", "neutral-subdued-visual-color",
        "accent-color-100", "accent-color-500", "accent-color-800", "accent-color-900", "accent-color-1000",
        "accent-content-color-default", "accent-visual-color", "accent-background-color-default",
        "negative-color-900", "negative-visual-color", "notice-visual-color", "positive-visual-color",
        "focus-indicator-color", "disclosure-indicator-color", "menu-item-background-color-hover",
        "drop-shadow-color",
    ],
    "layout": [
        *[f"component-height-{n}" for n in (50, 75, 100, 200, 300)],
        *[f"corner-radius-{n}" for n in (75, 100, 200, 300)],
        "corner-radius-small-default", "corner-radius-medium-default", "corner-radius-large-default",
        *[f"spacing-{n}" for n in (50, 75, 100, 200, 300, 400, 500, 600, 800)],
        "text-to-visual-50", "text-to-visual-75", "text-to-visual-100", "text-to-visual-200", "text-to-visual-300",
        "text-to-control-75", "text-to-control-100", "text-to-control-200",
        "component-edge-to-text-75", "component-edge-to-text-100", "component-edge-to-text-200",
        "component-edge-to-visual-75", "component-edge-to-visual-100", "component-edge-to-visual-200",
        "component-pill-edge-to-text-100", "component-to-menu-medium",
        "border-width-100", "border-width-200", "focus-indicator-thickness", "focus-indicator-gap",
        "workflow-icon-size-50", "workflow-icon-size-75", "workflow-icon-size-100", "workflow-icon-size-200",
        "side-navigation-item-to-item", "side-navigation-item-to-header", "side-navigation-header-to-item",
        "side-navigation-second-level-edge-to-text", "side-navigation-third-level-edge-to-text",
        "side-navigation-minimum-height", "side-navigation-width", "side-navigation-bottom-to-text",
        "side-navigation-top-to-text",
    ],
    "typography": [
        *[f"font-size-{n}" for n in (25, 50, 75, 100, 200, 300, 400, 500)],
        "regular-font-weight", "medium-font-weight", "bold-font-weight", "extra-bold-font-weight",
        "line-height-100", "line-height-200", "cjk-line-height-100",
        "body-size-xs", "body-size-s", "body-size-m", "body-size-l", "body-size-xl",
        "heading-size-xxs", "heading-size-xs", "heading-size-s", "heading-size-m", "heading-size-l",
        "detail-size-s", "detail-size-m", "detail-size-l",
        "body-font-family", "heading-font-family", "detail-font-family", "sans-serif-font-family",
    ],
}

SETS = ("light", "dark")
SCALE = "desktop"
ALIAS = re.compile(r"^\{([^}]+)\}$")


def load(package: Path) -> dict:
    tokens = {}
    for f in sorted((package / "src").glob("*.json")):
        d = json.loads(f.read_text(encoding="utf-8"))
        for k, v in d.items():
            tokens.setdefault(k, v)
    return tokens


def pick(node: dict, set_name: str) -> dict:
    """descend into color-theme and platform-scale sets until a value node"""
    while "sets" in node:
        sets = node["sets"]
        if set_name in sets:
            node = sets[set_name]
        elif SCALE in sets:
            node = sets[SCALE]
        else:
            node = next(iter(sets.values()))
    return node


def resolve(tokens: dict, name: str, set_name: str, depth: int = 0):
    if name not in tokens:
        return None
    if depth > 20:
        raise RecursionError(name)
    node = pick(tokens[name], set_name)
    value = node.get("value")
    if isinstance(value, str):
        m = ALIAS.match(value)
        if m:
            return resolve(tokens, m.group(1), set_name, depth + 1)
    return value


def rgb_to_hex(v: str) -> str:
    m = re.match(r"rgba?\((\d+),\s*(\d+),\s*(\d+)(?:,\s*([0-9.]+))?\)", v)
    if not m:
        return v
    r, g, b = (int(m.group(i)) for i in (1, 2, 3))
    a = m.group(4)
    if a is None or float(a) >= 1.0:
        return f"#{r:02x}{g:02x}{b:02x}"
    return f"rgba({r},{g},{b},{a})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path, help="for the JSON and the license")
    ap.add_argument("--css-out", type=Path, help="for the two CSS chunks (default: --out); the themes dir, so they install")
    args = ap.parse_args()
    css_out = args.css_out or args.out
    css_out.mkdir(parents=True, exist_ok=True)

    tokens = load(args.package)
    version = json.loads((args.package / "package.json").read_text(encoding="utf-8"))["version"]
    out = {"source": f"@adobe/spectrum-tokens {version} (Apache-2.0)", "scale": SCALE, "sets": {}}
    missing = []
    for set_name in SETS:
        resolved = {}
        for group, names in CURATED.items():
            resolved[group] = {}
            for n in names:
                v = resolve(tokens, n, set_name)
                if v is None:
                    missing.append(n)
                    continue
                resolved[group][n] = rgb_to_hex(v) if isinstance(v, str) else v
        out["sets"][set_name] = resolved
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "spectrum-tokens.json").write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8", newline="\n")

    for set_name in SETS:
        r = out["sets"][set_name]
        lines = [f"/* Adobe Spectrum design tokens, {set_name} theme, {SCALE} scale.",
                 f"   generated by tools/ibis/spectrum_tokens.py from {out['source']}; do not edit.",
                 "   colors are @define-color names, dimensions and type are the table in comments",
                 "   (GTK CSS has no custom properties): write the number where the theme needs it. */", ""]
        for n, v in r["color"].items():
            lines.append(f"@define-color spectrum_{n.replace('-', '_')} {v};")
        lines.append("")
        lines.append("/* layout")
        for n, v in r["layout"].items():
            lines.append(f"   {n:<48} {v}")
        lines.append("*/")
        lines.append("/* typography")
        for n, v in r["typography"].items():
            lines.append(f"   {n:<48} {v}")
        lines.append("*/")
        (css_out / f"chunk-spectrum-{set_name}.css").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    lic = args.package / "LICENSE"
    if lic.is_file():
        shutil.copyfile(lic, args.out / "LICENSE-spectrum-tokens.txt")

    n = sum(len(g) for g in out["sets"]["dark"].values())
    print(f"{n} tokens resolved per set into {args.out}")
    if missing:
        print("not in the package: " + ", ".join(sorted(set(missing))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
