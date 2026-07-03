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

/* App dock pinned-apps list (see dock_pinned below). */
#define DC_CONFIG_DOCK_PINNED_MAX 16

typedef enum {
    DC_BAR_POSITION_TOP = 0,
    DC_BAR_POSITION_BOTTOM,
} dc_bar_position;

typedef struct dc_config {
    char theme_id[DC_CONFIG_THEME_MAX];   /* built-in palette id, e.g. "green" */
    /* Dark/light selection. Config key "themeMode": "dark" | "light" | "auto".
     * "auto" follows the wall clock (light 06:00-18:00, dark otherwise). The
     * env var DANKC_THEME_MODE (dark|light|auto) overrides this at startup for
     * testing. Applies to BOTH stock themes and wallpaper-derived dynamic
     * color. Default "dark" (matches DMS's dark-first stock themes). */
    char theme_mode[8];
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
    char weather_location[64]; /* display name for the dashboard Weather tab */

    /* Left/center/right widget ids, in display order (left-to-right within
     * each section; see dc_bar's layout pass for how each section anchors). */
    char bar_left_widgets[DC_CONFIG_WIDGETS_MAX][DC_CONFIG_WIDGET_ID_MAX];
    int bar_left_widgets_n;
    char bar_center_widgets[DC_CONFIG_WIDGETS_MAX][DC_CONFIG_WIDGET_ID_MAX];
    int bar_center_widgets_n;
    char bar_right_widgets[DC_CONFIG_WIDGETS_MAX][DC_CONFIG_WIDGET_ID_MAX];
    int bar_right_widgets_n;

    /* notifications (docs/08-SETTINGS-UI.md PANELS > Notifications): popup
     * lifetime per urgency in whole seconds (0 = never auto-dismiss), and a
     * do-not-disturb master switch that suppresses new toast popups while
     * still recording them in the notification center. */
    int notif_timeout_low_sec;
    int notif_timeout_normal_sec;
    int notif_timeout_critical_sec;
    bool dnd_enabled;

    /* Notification sounds (docs/14-COMPLETION-PLAN.md W1.3, services/sound.c):
     * master switch + per-event toggle + linear volume, matching DMS's
     * SettingsData.soundsEnabled/soundNewNotification (SoundsTab.qml). Both
     * default on, matching DMS. dnd_enabled above also suppresses sound
     * (docs/14 explicitly asks for this; DMS itself only gates the toast, not
     * the sound -- see sound.h's dc_sound_notify() comment). */
    bool sounds_enabled;
    bool notif_sound_enabled;
    float sound_volume;

    /* launcher (docs/08-SETTINGS-UI.md DOCK & LAUNCHER): default view mode
     * used when the launcher (re)opens -- list (false) or grid (true). */
    bool launcher_grid_view;

    /* App dock (docs/POLISH.md P5, docs/11-UX-FLOW.md sec.5): off by default
     * so existing users aren't surprised by a new persistent surface -- see
     * ui/dock.c. Position always mirrors bar_position (same edge, stacked
     * just past the bar's outer edge -- matches DMS's default dockPosition
     * == barPosition behavior, docs/11 sec.5 "Dock" + Modules/Dock/Dock.qml
     * barSpacing computation). */
    bool dock_enabled;    /* master switch; ui/dock.c only creates a surface when true */
    bool dock_auto_hide;  /* hide unless hovered/revealed (DMS dockAutoHide, default off) */
    int dock_icon_size;   /* px, DMS dockIconSize default 40 */
    /* Pinned app ids (desktop-entry basenames), in display order. Reuses
     * DC_CONFIG_WIDGET_ID_MAX-sized slots (get_string_array/add_string_array
     * are already keyed to that width) -- comfortably fits real desktop ids.
     * Default empty, matching DMS's own SessionData.pinnedApps: []. */
    char dock_pinned[DC_CONFIG_DOCK_PINNED_MAX][DC_CONFIG_WIDGET_ID_MAX];
    int dock_pinned_n;
    /* Frame: rounded screen corners overlay (docs/POLISH.md P2, ui/frame.c).
     * DMS's frameEnabled defaults to false (SettingsData.qml); frame_radius
     * defaults to the base cornerRadius design token (docs/10-DESIGN-SYSTEM.md
     * sec.1 = 12), not DMS's separate frameRounding=23 (that belongs to the
     * full connected-chrome Frame system, which dankc doesn't implement). */
    bool frame_enabled;
    float frame_radius;

    /* Material background: blurred+dimmed wallpaper behind panel cards
     * instead of a flat surfaceContainer fill (docs/POLISH.md P2,
     * ui/material_bg.c). Falls back to the flat fill when disabled or when
     * no wallpaper is configured/readable. */
    bool material_blur;

    /* XDG autostart (docs/14-COMPLETION-PLAN.md W1.2, services/autostart.c):
     * master switch for launching ~/.config/autostart + /etc/xdg/autostart
     * entries at session start. DMS has no equivalent setting (it never
     * spawns autostart entries itself, only manages the directory via
     * AutoStartTab.qml) -- default true here since dankc is acting as the
     * whole session/DE and a fresh install should behave like any other DE
     * out of the box. */
    bool autostart_enabled;

    /* OSD (docs/14-COMPLETION-PLAN.md W2.3, ui/osd.c): auto-hide timeout and
     * screen position for the volume/brightness overlay. DMS's osdPosition
     * enumerates 8 corners/edges (docs/09 "OSD Position"); dankc's layer-
     * shell anchoring only implements the 4 that don't require horizontal
     * centering math beyond what dc_popout_bar_adjacent-style anchors give
     * for free -- see osd.c's dc_osd_position enum for the mapping. */
    int osd_position;    /* dc_osd_position */
    int osd_timeout_ms;  /* auto-hide delay, ms */

    /* Typography (docs/14-COMPLETION-PLAN.md W2.4): scales UI text sizes.
     * NOTE: only wired into ui/settings.c's own text so the slider has a
     * visible live effect without touching every nvgFontSize() call site
     * across bar/panels (see settings.c's ui_font_size() comment) -- full
     * shell-wide propagation is deferred, matching the task's own escape
     * hatch for this item. */
    float font_scale;

    /* Locale (docs/14-COMPLETION-PLAN.md W2 "Locale"): first day of week for
     * the dashboard calendar grid (0=Sunday .. 6=Saturday, matching struct
     * tm's tm_wday numbering used by ui/dashboard.c's draw_calendar_card()). */
    int first_day_of_week;

    /* Theme & Colors (docs/14-COMPLETION-PLAN.md W2 "Theme & Colors deep
     * tab"): light/dark mode preference. UI-only for now -- dankc's theme
     * engine (under src/theme) is dark-only; a separate agent owns adding
     * real light-theme variants and will consume this key. Defaults "dark"
     * so nothing changes visually until that lands. */
    char theme_mode[8];
} dc_config;

/* The active config. Read-only for the rest of the app. */
extern const dc_config *dc_config_current;

/* Resolve theme_mode ("dark"|"light"|"auto" + the DANKC_THEME_MODE env
 * override) to a concrete light/dark bool (true = light). "auto" -> clock. */
bool dc_config_light_mode(void);

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
