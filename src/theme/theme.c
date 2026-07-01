#include "theme/theme.h"

#define RGB(rr, gg, bb) {.r = 0x##rr, .g = 0x##gg, .b = 0x##bb, .a = 0xff}

/* DankMaterialShell stock "green" palette, verbatim from the reference
 * implementation (quickshell/Common/StockThemes.js). Semantic error/warning/
 * success use DMS's fixed values (Theme.qml). */
static const dc_theme green_theme = {
    .primary = RGB(4c, af, 50),
    .primary_text = RGB(00, 00, 00),
    .primary_container = RGB(1b, 5e, 20),
    .secondary = RGB(81, c9, 95),
    .surface = RGB(10, 14, 0f),
    .surface_text = RGB(e0, e4, db),
    .surface_variant = RGB(42, 49, 40),
    .surface_variant_text = RGB(c2, c9, bd),
    .background = RGB(10, 14, 0f),
    .background_text = RGB(e0, e4, db),
    .outline = RGB(8c, 93, 88),
    .surface_container_lowest = RGB(0c, 10, 0b),
    .surface_container_low = RGB(19, 1d, 17),
    .surface_container = RGB(1d, 21, 1b),
    .surface_container_high = RGB(27, 2b, 25),
    .surface_container_highest = RGB(32, 36, 30),
    .error = RGB(f2, b8, b5),
    .warning = RGB(ff, 98, 00),
    .success = RGB(4c, af, 50),
};

const dc_theme *dc_theme_current = &green_theme;

void dc_theme_init(void)
{
    dc_theme_current = &green_theme;
}
