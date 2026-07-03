#!/usr/bin/env python3
"""Generate src/theme/stock_themes.inc from DankMaterialShell's StockThemes.js.

Emits BOTH the dark and light variant of every stock theme (dc_stock_theme now
carries a .dark and .light dc_theme). Semantic error/warning/info/success are
fixed (DMS keeps them constant across themes/modes). Run from the repo root:

    scripts/gen_stock_themes.py \
        ~/Downloads/DankMaterialShell-master/quickshell/Common/StockThemes.js \
        > src/theme/stock_themes.inc
"""
import re
import sys

# JS key -> dc_theme C field. surfaceTint is DMS-only (unused here) and skipped.
FIELD_MAP = {
    "primary": "primary",
    "primaryText": "primary_text",
    "primaryContainer": "primary_container",
    "secondary": "secondary",
    "surface": "surface",
    "surfaceText": "surface_text",
    "surfaceVariant": "surface_variant",
    "surfaceVariantText": "surface_variant_text",
    "background": "background",
    "backgroundText": "background_text",
    "outline": "outline",
    "surfaceContainerLowest": "surface_container_lowest",
    "surfaceContainerLow": "surface_container_low",
    "surfaceContainer": "surface_container",
    "surfaceContainerHigh": "surface_container_high",
    "surfaceContainerHighest": "surface_container_highest",
}
# Field emission order (matches theme.h) with the fixed semantic tail.
ORDER = list(FIELD_MAP.values())

# Stable id order (matches the previous stock_themes.inc / settings swatch grid).
ID_ORDER = ["blue", "purple", "green", "orange", "red", "cyan", "pink", "amber",
            "coral", "monochrome"]


def hexc(s):
    s = s.lstrip("#")
    return int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)


def parse_block(js, section):
    """Parse one of DARK/LIGHT into {id: {jskey: '#hex'}}."""
    m = re.search(section + r":\s*\{", js)
    assert m, "no %s section" % section
    i = m.end()
    depth = 1
    start = i
    while depth:
        if js[i] == "{":
            depth += 1
        elif js[i] == "}":
            depth -= 1
        i += 1
    body = js[start:i - 1]
    themes = {}
    for tm in re.finditer(r"(\w+):\s*\{([^}]*)\}", body):
        tid = tm.group(1)
        fields = {}
        for fm in re.finditer(r'(\w+):\s*"(#[0-9a-fA-F]{6})"', tm.group(2)):
            fields[fm.group(1)] = fm.group(2)
        themes[tid] = fields
    return themes


def emit_theme(fields):
    out = []
    for jskey, cfield in FIELD_MAP.items():
        r, g, b = hexc(fields[jskey])
        out.append("                .%s = {0x%02x,0x%02x,0x%02x,255}," % (cfield, r, g, b))
    out.append("                .error = {0xf4,0x43,0x36,255}, .warning = {0xff,0x98,0x00,255},")
    out.append("                .info = {0x21,0x96,0xf3,255}, .success = {0x4c,0xaf,0x50,255},")
    return "\n".join(out)


def main():
    js = open(sys.argv[1]).read()
    dark = parse_block(js, "DARK")
    light = parse_block(js, "LIGHT")

    names = {"blue": "Blue", "purple": "Purple", "green": "Green", "orange": "Orange",
             "red": "Red", "cyan": "Cyan", "pink": "Pink", "amber": "Amber",
             "coral": "Coral", "monochrome": "Monochrome"}

    print("/* Generated from DankMaterialShell quickshell/Common/StockThemes.js by")
    print(" * scripts/gen_stock_themes.py -- DARK and LIGHT variants of every stock")
    print(" * theme. Do not edit by hand; semantic error/warning/info/success are fixed. */")
    print("")
    print("static const dc_stock_theme dc_stock_themes[] = {")
    for tid in ID_ORDER:
        if tid not in dark:
            continue
        print("    {")
        print('        .id = "%s", .name = "%s",' % (tid, names.get(tid, tid.title())))
        print("        .dark = {")
        print(emit_theme(dark[tid]))
        print("        },")
        print("        .light = {")
        print(emit_theme(light[tid]))
        print("        },")
        print("    },")
    print("};")


if __name__ == "__main__":
    main()
