#!/usr/bin/env bash
# subset-fonts.sh — regenerate assets/fonts/MaterialSymbolsRounded.subset.ttf
# from the full vendored variable font (docs/POLISH.md P7 item 3).
#
# dankc only ever draws the fixed set of codepoints in
# src/render/icons.h's DC_ICON_* defines (see render/nvg.c's
# ICON_FONT_CANDIDATES comment for why this is the font that's actually
# loaded/installed). Re-run this whenever a new DC_ICON_ is added so the
# subset stays in sync — it derives the codepoint list straight from
# icons.h, so there's nothing else to keep in sync by hand.
#
# Requires: fonttools (pyftsubset). `pip install fonttools` or your distro's
# python-fonttools package.
set -euo pipefail
cd "$(dirname "$0")/.."

src=assets/fonts/MaterialSymbolsRounded.ttf
out=assets/fonts/MaterialSymbolsRounded.subset.ttf

if [ ! -f "$src" ]; then
    echo "subset-fonts.sh: $src not found (full font is repo-only, not installed" >&2
    echo "  by meson.build -- if you deleted it, restore it from git history first)." >&2
    exit 1
fi
command -v pyftsubset >/dev/null || {
    echo "subset-fonts.sh: pyftsubset not found (install fonttools)" >&2
    exit 1
}

codes=$(grep -oP '#define DC_ICON_\w+\s+\K0x[0-9a-fA-F]+' src/render/icons.h \
    | sed 's/0x/U+/' | sort -u | paste -sd, -)
n=$(echo "$codes" | tr ',' '\n' | wc -l)
echo "subset-fonts.sh: $n codepoints from src/render/icons.h"

pyftsubset "$src" \
    --unicodes="$codes" \
    --output-file="$out" \
    --layout-features='*' \
    --glyph-names \
    --symbol-cmap \
    --legacy-cmap \
    --notdef-glyph \
    --notdef-outline \
    --recommended-glyphs \
    --name-IDs='*' \
    --name-legacy \
    --name-languages='*'

before=$(stat -c%s "$src")
after=$(stat -c%s "$out")
echo "subset-fonts.sh: $out written ($((before / 1024))KB -> $((after / 1024))KB)"
