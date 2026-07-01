/* theme.h — the active Material color palette + design tokens.
 *
 * Seeded with DankMaterialShell's stock "green" palette (the theme currently in
 * use) so DankC matches DMS exactly. The Material color engine (M2/M3) will
 * later populate this from the wallpaper; every widget reads from here rather
 * than hardcoding colors (see docs/10-DESIGN-SYSTEM.md).
 */
#ifndef DC_THEME_THEME_H
#define DC_THEME_THEME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct dc_color {
    uint8_t r, g, b, a;
} dc_color;

typedef struct dc_theme {
    dc_color primary;
    dc_color primary_text;
    dc_color primary_container;
    dc_color secondary;
    dc_color surface;
    dc_color surface_text;
    dc_color surface_variant;
    dc_color surface_variant_text;
    dc_color background;
    dc_color background_text;
    dc_color outline;
    dc_color surface_container_lowest;
    dc_color surface_container_low;
    dc_color surface_container;
    dc_color surface_container_high;
    dc_color surface_container_highest;
    dc_color error;
    dc_color warning;
    dc_color info;
    dc_color success;
} dc_theme;

/* The active theme. Read-only for widgets; owned by the theme module. */
extern const dc_theme *dc_theme_current;

/* Select the default palette ("green"). Safe to call before rendering starts. */
void dc_theme_init(void);

/* Switch to the built-in palette with the given id (e.g. "blue", "green",
 * "monochrome"). Falls back to the default if `id` is unknown. Returns true iff
 * the requested id matched a known theme. */
bool dc_theme_set(const char *id);

/* Enumerate the built-in palettes (for config/settings UI). */
int dc_theme_count(void);
const char *dc_theme_id_at(int index);
const char *dc_theme_name_at(int index);

#endif /* DC_THEME_THEME_H */
