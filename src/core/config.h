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

/* Bar widget host (docs/12-BAR-SPEC.md sec.0/7 stage S2): each section is a
 * config-driven list of widget ids, e.g. "launcherButton", "clock". */
#define DC_CONFIG_WIDGETS_MAX 12
#define DC_CONFIG_WIDGET_ID_MAX 32

typedef enum {
    DC_BAR_POSITION_TOP = 0,
    DC_BAR_POSITION_BOTTOM,
} dc_bar_position;

typedef struct dc_config {
    char theme_id[DC_CONFIG_THEME_MAX];   /* built-in palette id, e.g. "green" */
    bool clock_24h;                       /* 24-hour vs 12-hour clock */
    bool show_date;                       /* show the date next to the clock */
    bool show_seconds;                    /* show seconds in the clock */
    bool animations_enabled;              /* master switch for panel animations */
    float animation_speed;                /* duration multiplier (0.25..4.0; 1 = DMS) */
    bool dynamic_color;                   /* derive the palette from the wallpaper */
    char wallpaper[DC_CONFIG_PATH_MAX];   /* image for dynamic color */

    dc_bar_position bar_position;         /* top or bottom edge */
    int bar_spacing;                      /* gap between the bar rect and the outer edge/screen sides */
    int bar_inner_padding;                /* drives the pill/bar-thickness formulas (docs/12-BAR-SPEC.md) */
    int bar_widget_padding;               /* horizontal padding inside each widget pill */
    float bar_transparency;               /* 0..1 alpha multiplier for the bar background */
    float bar_widget_transparency;        /* 0..1 alpha multiplier for widget pill backgrounds */

    /* weather widget (docs/12-BAR-SPEC.md sec.4/7 S4b): fixed lat/lon, no
     * geolocation yet. dc_weather_init() is only called at startup when
     * weather_enabled is true. */
    bool weather_enabled;
    double weather_lat;
    double weather_lon;
    bool weather_fahrenheit;

    /* Left/center/right widget ids, in display order (left-to-right within
     * each section; see dc_bar's layout pass for how each section anchors). */
    char bar_left_widgets[DC_CONFIG_WIDGETS_MAX][DC_CONFIG_WIDGET_ID_MAX];
    int bar_left_widgets_n;
    char bar_center_widgets[DC_CONFIG_WIDGETS_MAX][DC_CONFIG_WIDGET_ID_MAX];
    int bar_center_widgets_n;
    char bar_right_widgets[DC_CONFIG_WIDGETS_MAX][DC_CONFIG_WIDGET_ID_MAX];
    int bar_right_widgets_n;
} dc_config;

/* The active config. Read-only for the rest of the app. */
extern const dc_config *dc_config_current;

/* Load ~/.config/dankc/config.json (or defaults if absent/invalid) and apply
 * the theme selection. Call once at startup after dc_theme_init(). */
void dc_config_load(void);

/* Mutable access for the settings UI. After editing, call dc_config_reapply()
 * (re-selects theme + dynamic color) and dc_config_save() (persist to disk). */
dc_config *dc_config_mut(void);
void dc_config_reapply(void);
void dc_config_save(void);

/* Register a callback invoked by dc_config_notify_changed(). The settings UI
 * calls dc_config_notify_changed() after mutating fields that affect other
 * live surfaces (bar geometry, widget lists); main.c registers a callback that
 * reconfigures + redraws the bars. */
void dc_config_set_change_cb(void (*cb)(void *ud), void *ud);
void dc_config_notify_changed(void);

#endif /* DC_CORE_CONFIG_H */
