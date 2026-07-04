/* config.h — user configuration loaded from ~/.config/dankc/config.json.
 *
 * A small, forgiving JSON config (cJSON) applied at startup: theme selection,
 * clock format, and animation preferences. Missing file or keys fall back to
 * DMS-matching defaults. See docs/06-ROADMAP (config engine).
 */
#ifndef DC_CORE_CONFIG_H
#define DC_CORE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define DC_CONFIG_THEME_MAX 32
#define DC_CONFIG_PATH_MAX 512

/* Per-app notification rules (docs/26-DND-SCHEDULING-PLAN.md rule-engine
 * section): "match" is compared case-insensitively against either the
 * notification's app_name or its desktop-entry hint (T2, notifications.c).
 * action/urgency are plain ints -- not the notifications.h enum -- so
 * config.c doesn't need to depend on that header (same trick as
 * nightlight_schedule_mode below); notifications.c casts them back. */
#define DC_CONFIG_NOTIF_RULES_MAX 32
#define DC_CONFIG_NOTIF_RULE_MATCH 64

/* action: 0=mute (recorded, no popup/sound) / 1=ignore (dropped entirely) /
 * 2=popup-only (toast then deleted, never kept in history) / 3=no-history
 * (toast+Current, deleted once it would move to History).
 * urgency: -1=keep the notification's own urgency, else 0=low/1=normal/
 * 2=critical override. */
typedef struct dc_notif_rule {
    char match[DC_CONFIG_NOTIF_RULE_MATCH];
    int action;
    int urgency;
} dc_notif_rule;

/* Bar widget host (docs/12-BAR-SPEC.md sec.0/7 stage S2): each section is a
 * config-driven list of widget ids, e.g. "launcherButton", "clock". */
#define DC_CONFIG_WIDGETS_MAX 12
#define DC_CONFIG_WIDGET_ID_MAX 32

/* App dock pinned-apps list (see dock_pinned below). */
#define DC_CONFIG_DOCK_PINNED_MAX 16

/* Audio per-device config (docs/25-AUDIO-PERDEVICE-PLAN.md sec.3 "Config
 * surface", T2): keyed by pipewire node.name, which runs longer (~45 chars
 * for a typical USB/HDMI sink) than DC_CONFIG_WIDGET_ID_MAX (32) so these get
 * their own wider name buffer. Capped at 16 devices each; a full table
 * evicts the oldest entry to make room for a new one (stale entries from a
 * USB device that moved ports are harmless, just wasted slots). */
#define DC_CONFIG_AUDIO_DEVICES_MAX 16
#define DC_CONFIG_AUDIO_NAME_MAX 96
#define DC_CONFIG_AUDIO_ALIAS_MAX 48

typedef enum {
    DC_BAR_POSITION_TOP = 0,
    DC_BAR_POSITION_BOTTOM,
} dc_bar_position;

/* Per-device max-volume clamp entry ("audioDeviceMaxVolumes" JSON object,
 * node.name -> percent). max_percent is stored clamped to 100-200; a device
 * with no entry uses the plain 100 default (dc_config_audio_max()). */
typedef struct dc_audio_max_entry {
    char name[DC_CONFIG_AUDIO_NAME_MAX];
    int max_percent;
} dc_audio_max_entry;

/* Per-device display-name override ("audioDeviceAliases" JSON object,
 * node.name -> alias string). dankc-config-only (D3 in the plan doc): no
 * wireplumber file is written, so applying an alias never audibly
 * interrupts audio. */
typedef struct dc_audio_alias_entry {
    char name[DC_CONFIG_AUDIO_NAME_MAX];
    char alias[DC_CONFIG_AUDIO_ALIAS_MAX];
} dc_audio_alias_entry;

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
    /* Optional per-mode overrides (docs/29-SMALL-FEATURES-PLAN.md sec.3,
     * wallpaper T2). Config keys "wallpaperLight"/"wallpaperDark", default
     * empty (unset -> `wallpaper` above is used regardless of mode). Resolved
     * by dc_wallpaper_effective() (services/wallpaper.h), which is the only
     * place that should branch on these + `wallpaper`. */
    char wallpaper_light[DC_CONFIG_PATH_MAX];
    char wallpaper_dark[DC_CONFIG_PATH_MAX];

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

    /* Night Light (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.4,
     * services/nightlight.c): replaces the old hardcoded 4000K on/off
     * toggle. nightlight_schedule_mode is a dc_nightlight_schedule value
     * (0=manual/fixed, 1=sunset-to-sunrise via weather_lat/weather_lon,
     * 2=fixed HH:MM-to-HH:MM window) -- stored as a plain int here so
     * config.c doesn't need to depend on services/nightlight.h; nightlight.c
     * casts it back. nightlight_temp is the night-side color temperature in
     * Kelvin (2500-6500); nightlight_from/to are "HH:MM" strings, only used
     * when schedule mode == 2. */
    bool nightlight_enabled;
    int nightlight_temp;
    int nightlight_schedule_mode;
    char nightlight_from[8];
    char nightlight_to[8];

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

    /* DND scheduling (docs/26-DND-SCHEDULING-PLAN.md): dnd_enabled above
     * remains the sole runtime gate (back-compat: an old config with just
     * dndEnabled:true keeps meaning "on indefinitely"). dnd_until_epoch is a
     * CLOCK_REALTIME wall-clock second count: 0 means "on indefinitely" while
     * dnd_enabled is true, >0 is an auto-resume deadline (a stale/past value
     * self-clears on the next 1Hz tick, T2). dnd_until_hour is the resume
     * hour (0-23, localtime) used by the "until HH:MM" preset. */
    int64_t dnd_until_epoch;
    int dnd_until_hour;

    /* Notification privacy mode (docs/26-DND-SCHEDULING-PLAN.md): redacts
     * toast summary/body at render time (T4, toasts.c); the notification
     * center itself still shows full content. */
    bool notif_privacy_mode;

    /* Per-app notification rules; first match wins (T2, method_notify). */
    dc_notif_rule notif_rules[DC_CONFIG_NOTIF_RULES_MAX];
    int notif_rules_n;

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

    /* Lock Screen (docs/14-COMPLETION-PLAN.md W3.2, ui/lock.c): a pragmatic
     * subset of DMS's 23-setting LockScreenTab.qml -- only the options
     * ui/lock.c can actually honor (no fingerprint/U2F/video-screensaver/
     * multi-monitor backend exists). Defaults match lock.c's prior
     * hardcoded behavior (clock+date always shown, password field always
     * visible, flat themed background). */
    bool lock_show_clock;         /* big HH:MM clock */
    bool lock_show_date;          /* weekday/date line under the clock */
    bool lock_show_password_field; /* if false, the password pill only appears once typing starts */
    bool lock_use_wallpaper_bg;   /* blurred wallpaper background (reuses `wallpaper` + material_blur's
                                    * blur pipeline, ui/material_bg.c) instead of the flat surface fill */

    /* Mouse/Touchpad/Keyboard (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.7,
     * services/niri_input.c): mirrors niri's `input {}` KDL block, which uses
     * bare-keyword presence (not `key true/false`) for its boolean toggles --
     * so each bool here is "emit this keyword in the managed fragment", not a
     * tri-state on/off/default. All default false/0 (matches niri's own
     * built-in defaults, i.e. a fresh install changes nothing until the user
     * opts in). accel-speed is only written when its own *_enabled flag is
     * set, since 0 is indistinguishable from niri's default. */
    bool input_touchpad_tap;
    bool input_touchpad_natural_scroll;
    bool input_touchpad_dwt;
    bool input_touchpad_disabled_on_external_mouse;
    bool input_touchpad_accel_enabled;
    float input_touchpad_accel_speed; /* -1..1 */
    bool input_mouse_natural_scroll;
    bool input_mouse_accel_enabled;
    float input_mouse_accel_speed; /* -1..1 */
    bool input_keyboard_numlock;
    char input_keyboard_layout[32]; /* xkb layout string e.g. "us"; empty = unset */

    /* System-wide theming (services/systheme.c): dankc writes each app's own
     * native theme file directly from dc_theme_current + dc_config_light_mode()
     * (no external `matugen` invocation). systheme_enabled is the master
     * switch, off by default so a fresh install never touches any app's
     * config until the user opts in via Settings > Theme & Colors. Each
     * per-app toggle defaults on (matching "opt into the feature once, get
     * every supported app") but is only ever acted on when BOTH
     * systheme_enabled is true AND dc_systheme_app_detected() finds the app
     * actually installed -- see systheme.c's file header for the full
     * safety contract. Task 1 only implements the "gtk" tier; the rest are
     * wired here so later tasks don't need another config.h/config.c
     * change. */
    bool systheme_enabled;
    bool systheme_gtk;
    bool systheme_qt;
    bool systheme_alacritty;
    bool systheme_vscode;
    bool systheme_kitty;
    bool systheme_foot;

    /* System-wide theming, wave 2 (docs, Task 0 "foundation" of the
     * comprehensive expansion): toggles for the emitters landing in Tasks
     * 1-7. Same contract as the wave-1 toggles above -- each is only acted
     * on when systheme_enabled AND the per-app toggle AND
     * dc_systheme_app_detected() are all true. Default true (opt into the
     * feature once, get every detected app) except the Tier-3 "hard
     * caveat" apps below, which default false because touching them is
     * either disruptive to a running app (Firefox/Discord need an external
     * userChrome/Vencord mod to actually read dankc's output) or not
     * something a fresh install should silently rewrite (Konsole's
     * colorscheme, ~/.Xresources). */
    bool systheme_kvantum;
    bool systheme_kde;
    bool systheme_ghostty;
    bool systheme_wezterm;
    bool systheme_konsole;    /* default false: touches KDE's global colorscheme */
    bool systheme_xresources; /* default false: touches ~/.Xresources directly */
    bool systheme_zed;
    bool systheme_helix;
    bool systheme_neovim;
    bool systheme_vim;
    bool systheme_sublime;
    bool systheme_emacs;
    bool systheme_rofi;
    bool systheme_wofi;
    bool systheme_fuzzel;
    bool systheme_tofi;
    bool systheme_mako;
    bool systheme_dunst;
    bool systheme_swaync;
    bool systheme_btop;
    bool systheme_cava;
    bool systheme_zathura;
    bool systheme_qutebrowser;
    bool systheme_firefox;   /* default false: needs userChrome.css / a theme mod to take effect */
    bool systheme_discord;   /* default false: needs Vesktop/Vencord, not stock Discord */
    bool systheme_spicetify; /* default false: rewrites a Spotify client mod's theme files */
    bool systheme_gtk2;

    /* Battery protection + power/sleep depth (docs/24-BATTERY-POWER-PLAN.md).
     * charge_limit is the *desired* charge_control_end_threshold percentage
     * (50-100); battery.c's dc_battery_set_charge_limit() is the only thing
     * that ever writes it to sysfs (via pkexec), and only when the user hits
     * "Apply" -- sysfs resets on reboot, so dankc deliberately does NOT
     * auto-reapply this at startup (see plan doc "Key decisions"). Default
     * 100 == "no limit configured" (matches an unconfigured/absent sysfs
     * threshold). battery_notifications is the master switch for the
     * low/critical/charge-limit-reached toasts (services/battery_auto.c);
     * low/critical thresholds are plain battery percent (raw, not the
     * charge-limit-rescaled UI percent -- see plan doc). auto_power_saver
     * flips the power profile to power-saver once raw%% <= low threshold
     * while on battery. auto_profile_switch + profile_on_ac/profile_on_battery
     * drive an automatic dc_power_set_mode() call on each observed AC-edge
     * (never at startup); the profile ints are plain 0=power-saver/
     * 1=balanced/2=performance -- not power.h's enum, same trick as
     * nightlight_schedule_mode above, so config.c doesn't need to depend on
     * power.h. */
    int charge_limit;
    bool battery_notifications;
    int low_battery_threshold;
    int critical_battery_threshold;
    bool auto_power_saver;
    bool auto_profile_switch;
    int profile_on_ac;
    int profile_on_battery;

    /* Idle timeouts (stretch T7, src/services/idle.c -- an ext-idle-notify-v1
     * client): per-AC-source minute counts for 4 escalating stages (lock ->
     * monitor-off -> suspend -> hibernate), separately configurable for AC
     * and battery since logind's own IdleAction/Sec is single-valued and
     * can't do that split (plan doc "Key decisions"). idle_timeouts_enabled
     * is the master switch (off by default: no idle mechanism exists yet
     * without T7, and a stage value of 0 always means "disabled" regardless
     * of the master switch). All default 0 (disabled) so a fresh install/
     * upgrade changes nothing until the user opts in. */
    bool idle_timeouts_enabled;
    int idle_lock_ac_min;
    int idle_lock_batt_min;
    int idle_monitor_off_ac_min;
    int idle_monitor_off_batt_min;
    int idle_suspend_ac_min;
    int idle_suspend_batt_min;
    int idle_hibernate_ac_min;
    int idle_hibernate_batt_min;

    /* Audio per-device config (docs/25-AUDIO-PERDEVICE-PLAN.md sec.3, T2):
     * flat parallel tables keyed by pipewire node.name (config.c has no map
     * type). Unused until T3 (audio.c enforces the max clamp + resolves
     * aliases) and T4/T5 (settings.c/controlcenter.c UI) land. */
    dc_audio_max_entry audio_max_volumes[DC_CONFIG_AUDIO_DEVICES_MAX];
    int audio_max_volumes_n;
    dc_audio_alias_entry audio_aliases[DC_CONFIG_AUDIO_DEVICES_MAX];
    int audio_aliases_n;
    char audio_hidden[DC_CONFIG_AUDIO_DEVICES_MAX][DC_CONFIG_AUDIO_NAME_MAX];
    int audio_hidden_n;

    /* System updater (docs/29-SMALL-FEATURES-PLAN.md sec.4, services/
     * updates.c): update_terminal_cmd overrides the terminal dc_updates_
     * run_upgrade() spawns to run the interactive upgrade command -- a
     * printf-style template containing exactly one literal "%s" (substituted
     * with the upgrade command), e.g. foot -e sh -c '%s; read -p done'.
     * Default "" (empty) auto-probes foot/alacritty/kitty/wezterm/ghostty/
     * xterm on PATH instead (see updates.c). updates_check_interval_min is
     * minutes between automatic background checks (dc_updates_check_async());
     * default 0 = manual only (never auto-checks), matching the existing
     * settings.c updater tab's click-to-check behavior -- wiring the
     * periodic timer into main.c's clock_tick is a later task. */
    char update_terminal_cmd[256];
    int updates_check_interval_min;
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

/* Audio per-device config accessors (docs/25-AUDIO-PERDEVICE-PLAN.md sec.3,
 * T2). All keyed by pipewire node.name; `name` may be NULL/empty, in which
 * case the accessors return the "no entry" default and the setters are a
 * no-op. Setters remove the table entry when reset to the default value
 * (max_percent<=100 / alias empty-or-NULL / hidden=false) so config.json
 * doesn't accumulate no-op entries; a full table evicts its oldest entry to
 * make room for a new one. */
int dc_config_audio_max(const char *name);          /* default 100 */
const char *dc_config_audio_alias(const char *name); /* NULL if none set */
bool dc_config_audio_hidden(const char *name);

void dc_config_audio_set_max(const char *name, int max_percent);
void dc_config_audio_set_alias(const char *name, const char *alias);
void dc_config_audio_set_hidden(const char *name, bool hidden);

#endif /* DC_CORE_CONFIG_H */
