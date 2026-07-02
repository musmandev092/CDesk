/* config.h — user configuration loaded from ~/.config/dankc/config.json.
 *
 * A small, forgiving JSON config (cJSON) applied at startup: theme selection,
 * clock format, and animation preferences. Missing file or keys fall back to
 * DMS-matching defaults. See docs/06-ROADMAP (config engine).
 */
#ifndef DC_CORE_CONFIG_H
#define DC_CORE_CONFIG_H

#include <stdbool.h>

#define DC_CONFIG_THEME_MAX 32
#define DC_CONFIG_PATH_MAX 512

typedef struct dc_config {
    char theme_id[DC_CONFIG_THEME_MAX];   /* built-in palette id, e.g. "green" */
    bool clock_24h;                       /* 24-hour vs 12-hour clock */
    bool show_date;                       /* show the date next to the clock */
    bool animations_enabled;              /* master switch for panel animations */
    float animation_speed;                /* duration multiplier (0.25..4.0; 1 = DMS) */
    bool dynamic_color;                   /* derive the palette from the wallpaper */
    char wallpaper[DC_CONFIG_PATH_MAX];   /* image for dynamic color */
} dc_config;

/* The active config. Read-only for the rest of the app. */
extern const dc_config *dc_config_current;

/* Load ~/.config/dankc/config.json (or defaults if absent/invalid) and apply
 * the theme selection. Call once at startup after dc_theme_init(). */
void dc_config_load(void);

#endif /* DC_CORE_CONFIG_H */
