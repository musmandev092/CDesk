#include "core/config.h"

#include "core/dms_import.h"
#include "core/log.h"
#include "services/systheme.h"
#include "theme/dynamic.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"

/* DMS-matching defaults. */
static dc_config config = {
    .theme_id = "green",
    .theme_mode = "dark",
    .clock_24h = true,
    .show_date = true,
    .show_seconds = false,
    .animations_enabled = true,
    .animation_speed = 1.0f,

    .bar_position = DC_BAR_POSITION_TOP,
    .bar_spacing = 4,
    .bar_inner_padding = 4,
    .bar_widget_padding = 8,
    .bar_transparency = 1.0f,
    .bar_widget_transparency = 1.0f,

    /* User's live DMS weather config (docs/12-BAR-SPEC.md sec.0): fixed
     * "New York, NY" location, Celsius. */
    .weather_enabled = true,
    .weather_lat = 40.7128,
    .weather_lon = -74.0060,
    .weather_fahrenheit = false,
    .weather_location = "New York",

    /* Night Light (services/nightlight.c): off by default, matching the old
     * toggle's off-at-startup behavior; 4000K matches the old hardcoded
     * one-shot value so upgrading doesn't change anyone's expectation of
     * what "on" looks like. Schedule 0 = manual/fixed. */
    .nightlight_enabled = false,
    .nightlight_temp = 4000,
    .nightlight_schedule_mode = 0,

    /* User's live DMS layout (docs/12-BAR-SPEC.md sec.0). */
    .bar_left_widgets = {"launcherButton", "workspaceSwitcher", "focusedWindow"},
    .bar_left_widgets_n = 3,
    .bar_center_widgets = {"music", "clock", "weather"},
    .bar_center_widgets_n = 3,
    .bar_right_widgets = {"systemTray", "clipboard", "cpuUsage", "memUsage", "notificationButton",
                          "battery", "controlCenterButton"},
    .bar_right_widgets_n = 7,

    /* Matches the previously-hardcoded DC_NOTIF_LOW_MS/DC_NOTIF_DEFAULT_MS
     * and "critical never auto-expires" behavior (services/notifications.c). */
    .notif_timeout_low_sec = 5,
    .notif_timeout_normal_sec = 5,
    .notif_timeout_critical_sec = 0,
    .dnd_enabled = false,
    .dnd_until_epoch = 0,
    .dnd_until_hour = 8,
    .notif_privacy_mode = false,
    .notif_rules_n = 0,

    /* docs/14-COMPLETION-PLAN.md W1.3: matches DMS SettingsData.soundsEnabled/
     * soundNewNotification defaults (both on). */
    .sounds_enabled = true,
    .notif_sound_enabled = true,
    .sound_volume = 1.0f,

    .launcher_grid_view = false,

    .dock_enabled = false,
    .dock_auto_hide = false,
    .dock_icon_size = 40,
    .dock_pinned_n = 0,
    /* docs/POLISH.md P2: DMS's frameEnabled defaults off; frame_radius
     * defaults to the base cornerRadius token (docs/10-DESIGN-SYSTEM.md
     * sec.1). materialBlur defaults on per docs/POLISH.md P2. */
    .frame_enabled = false,
    .frame_radius = 12.0f,
    .material_blur = true,

    .autostart_enabled = true,

    .osd_position = 0, /* DC_OSD_POS_BOTTOM_CENTER, matches osd.c's prior hardcoded behavior */
    .osd_timeout_ms = 2000,

    .font_scale = 1.0f,

    .first_day_of_week = 0, /* Sunday, matches dashboard.c's prior hardcoded dow[] order */

    /* docs/14-COMPLETION-PLAN.md W3.2: matches lock.c's prior hardcoded
     * behavior (clock+date always on, password pill always visible, flat
     * background) so existing users see no change until they opt in. */
    .lock_show_clock = true,
    .lock_show_date = true,
    .lock_show_password_field = true,
    .lock_use_wallpaper_bg = false,

    /* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.7: all off/0 -- a fresh
     * install writes no dankc-input.kdl until the user opts into something,
     * matching niri's own out-of-the-box behavior. */
    .input_touchpad_tap = false,
    .input_touchpad_natural_scroll = false,
    .input_touchpad_dwt = false,
    .input_touchpad_disabled_on_external_mouse = false,
    .input_touchpad_accel_enabled = false,
    .input_touchpad_accel_speed = 0.0f,
    .input_mouse_natural_scroll = false,
    .input_mouse_accel_enabled = false,
    .input_mouse_accel_speed = 0.0f,
    .input_keyboard_numlock = false,

    /* services/systheme.c: master switch off (opt-in), per-app toggles on
     * (matches the master switch's own "opt in once, get everything
     * detected" intent -- see config.h's field comment). */
    .systheme_enabled = false,
    .systheme_gtk = true,
    .systheme_qt = true,
    .systheme_alacritty = true,
    .systheme_vscode = true,
    .systheme_kitty = true,
    .systheme_foot = true,

    /* Wave 2 (config.h's field comments): default true except the Tier-3
     * hard-caveat apps, which default false. */
    .systheme_kvantum = true,
    .systheme_kde = true,
    .systheme_ghostty = true,
    .systheme_wezterm = true,
    .systheme_konsole = false,
    .systheme_xresources = false,
    .systheme_zed = true,
    .systheme_helix = true,
    .systheme_neovim = true,
    .systheme_vim = true,
    .systheme_sublime = true,
    .systheme_emacs = true,
    .systheme_rofi = true,
    .systheme_wofi = true,
    .systheme_fuzzel = true,
    .systheme_tofi = true,
    .systheme_mako = true,
    .systheme_dunst = true,
    .systheme_swaync = true,
    .systheme_btop = true,
    .systheme_cava = true,
    .systheme_zathura = true,
    .systheme_qutebrowser = true,
    .systheme_firefox = false,
    .systheme_discord = false,
    .systheme_spicetify = false,
    .systheme_gtk2 = true,

    /* docs/24-BATTERY-POWER-PLAN.md: 100 == no charge limit configured;
     * notifications on by default; low/critical thresholds match common DE
     * defaults (20%/10%); auto power-saver/profile-switch off until the user
     * opts in; profileOnAC/Battery default to balanced-on-AC,
     * power-saver-on-battery (only consulted once autoProfileSwitch is on). */
    .charge_limit = 100,
    .battery_notifications = true,
    .low_battery_threshold = 20,
    .critical_battery_threshold = 10,
    .auto_power_saver = false,
    .auto_profile_switch = false,
    .profile_on_ac = 1,
    .profile_on_battery = 0,

    /* Idle timeouts (stretch T7): all disabled until the user opts in. */
    .idle_timeouts_enabled = false,
    .idle_lock_ac_min = 0,
    .idle_lock_batt_min = 0,
    .idle_monitor_off_ac_min = 0,
    .idle_monitor_off_batt_min = 0,
    .idle_suspend_ac_min = 0,
    .idle_suspend_batt_min = 0,
    .idle_hibernate_ac_min = 0,
    .idle_hibernate_batt_min = 0,

    /* docs/25-AUDIO-PERDEVICE-PLAN.md sec.3: no per-device overrides until
     * the user sets one via settings.c/controlcenter.c (T4/T5). */
    .audio_max_volumes_n = 0,
    .audio_aliases_n = 0,
    .audio_hidden_n = 0,
};

const dc_config *dc_config_current = &config;

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1 << 20) { /* sanity cap: 1 MiB */
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static void get_bool(const cJSON *root, const char *key, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item))
        *out = cJSON_IsTrue(item);
}

static void get_string(const cJSON *root, const char *key, char *out, size_t n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring)
        snprintf(out, n, "%s", item->valuestring);
}

static void get_float(const cJSON *root, const char *key, float *out, float lo, float hi)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        float v = (float)item->valuedouble;
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
        *out = v;
    }
}

static void get_int(const cJSON *root, const char *key, int *out, int lo, int hi)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
        *out = v;
    }
}

/* Like get_int() but 64-bit, for wall-clock epoch seconds (dnd_until_epoch)
 * which don't fit get_int()'s int clamp. cJSON stores numbers as a double
 * internally, which is lossless for epoch-second values well past the
 * heat-death of any config file. */
static void get_int64(const cJSON *root, const char *key, int64_t *out, int64_t lo, int64_t hi)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        int64_t v = (int64_t)item->valuedouble;
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
        *out = v;
    }
}

/* Like get_float() but double-precision, for the weather widget's lat/lon
 * (docs/12-BAR-SPEC.md sec.4 weather). */
static void get_double(const cJSON *root, const char *key, double *out, double lo, double hi)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        double v = item->valuedouble;
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
        *out = v;
    }
}

/* Parse a JSON array of strings into a fixed id[][] table; non-string entries
 * are skipped, extra entries beyond `cap` are dropped. Leaves `arr`/`out_n`
 * untouched if `key` is absent or not an array (defaults survive). */
static void get_string_array(const cJSON *root, const char *key,
                             char arr[][DC_CONFIG_WIDGET_ID_MAX], int cap, int *out_n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsArray(item))
        return;

    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item)
    {
        if (n >= cap)
            break;
        if (!cJSON_IsString(entry) || !entry->valuestring)
            continue;
        snprintf(arr[n], DC_CONFIG_WIDGET_ID_MAX, "%s", entry->valuestring);
        n++;
    }
    *out_n = n;
}

static void add_string_array(cJSON *root, const char *key, char arr[][DC_CONFIG_WIDGET_ID_MAX],
                             int n)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(a, cJSON_CreateString(arr[i]));
    cJSON_AddItemToObject(root, key, a);
}

/* Parse a JSON array of per-app notification rule objects
 * ({"match":"discord","action":"mute","urgency":"critical"}) into a fixed
 * dc_notif_rule[] table (docs/26-DND-SCHEDULING-PLAN.md rule-engine
 * section). Forgiving like get_string_array(): a non-array/absent key leaves
 * `arr`/`out_n` untouched (defaults survive); a non-object entry or one
 * missing "match" is skipped; an unrecognized action/urgency string falls
 * back to its default (mute / keep) rather than erroring. Entries beyond
 * `cap` are dropped. */
static void get_rule_array(const cJSON *root, const char *key, dc_notif_rule *arr, int cap,
                           int *out_n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsArray(item))
        return;

    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item)
    {
        if (n >= cap)
            break;
        if (!cJSON_IsObject(entry))
            continue;
        const cJSON *match = cJSON_GetObjectItemCaseSensitive(entry, "match");
        if (!cJSON_IsString(match) || !match->valuestring || !match->valuestring[0])
            continue; /* a rule with no match string can never fire; skip it */

        dc_notif_rule r = {0};
        r.urgency = -1; /* default: keep the notification's own urgency */
        snprintf(r.match, sizeof(r.match), "%s", match->valuestring);

        const cJSON *action = cJSON_GetObjectItemCaseSensitive(entry, "action");
        if (cJSON_IsString(action) && action->valuestring) {
            if (strcmp(action->valuestring, "ignore") == 0)
                r.action = 1;
            else if (strcmp(action->valuestring, "popup_only") == 0)
                r.action = 2;
            else if (strcmp(action->valuestring, "no_history") == 0)
                r.action = 3;
            /* "mute" or anything unrecognized -> 0 (mute, r.action's zero-init) */
        }

        const cJSON *urgency = cJSON_GetObjectItemCaseSensitive(entry, "urgency");
        if (cJSON_IsString(urgency) && urgency->valuestring) {
            if (strcmp(urgency->valuestring, "low") == 0)
                r.urgency = 0;
            else if (strcmp(urgency->valuestring, "normal") == 0)
                r.urgency = 1;
            else if (strcmp(urgency->valuestring, "critical") == 0)
                r.urgency = 2;
            /* "keep" or anything unrecognized -> -1 (keep, r.urgency's default above) */
        }

        arr[n] = r;
        n++;
    }
    *out_n = n;
}

static void add_rule_array(cJSON *root, const char *key, const dc_notif_rule *arr, int n)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "match", arr[i].match);

        const char *action_str = "mute";
        switch (arr[i].action) {
        case 1:
            action_str = "ignore";
            break;
        case 2:
            action_str = "popup_only";
            break;
        case 3:
            action_str = "no_history";
            break;
        default:
            action_str = "mute";
            break;
        }
        cJSON_AddStringToObject(o, "action", action_str);

        const char *urgency_str = "keep";
        switch (arr[i].urgency) {
        case 0:
            urgency_str = "low";
            break;
        case 1:
            urgency_str = "normal";
            break;
        case 2:
            urgency_str = "critical";
            break;
        default:
            urgency_str = "keep";
            break;
        }
        cJSON_AddStringToObject(o, "urgency", urgency_str);

        cJSON_AddItemToArray(a, o);
    }
    cJSON_AddItemToObject(root, key, a);
}

/* WIDE variant of get_string_array()/add_string_array(): same forgiving
 * semantics (non-array/absent key leaves arr/out_n untouched; non-string
 * entries skipped; entries beyond cap dropped), but sized for
 * DC_CONFIG_AUDIO_NAME_MAX (pipewire node.name runs longer than the 32-char
 * widget-id width the original helper is hardwired to). Used for
 * "audioHiddenDevices" (docs/25-AUDIO-PERDEVICE-PLAN.md sec.3, T2). */
static void get_string_array_wide(const cJSON *root, const char *key,
                                  char arr[][DC_CONFIG_AUDIO_NAME_MAX], int cap, int *out_n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsArray(item))
        return;

    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item)
    {
        if (n >= cap)
            break;
        if (!cJSON_IsString(entry) || !entry->valuestring)
            continue;
        snprintf(arr[n], DC_CONFIG_AUDIO_NAME_MAX, "%s", entry->valuestring);
        n++;
    }
    *out_n = n;
}

static void add_string_array_wide(cJSON *root, const char *key,
                                  char arr[][DC_CONFIG_AUDIO_NAME_MAX], int n)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(a, cJSON_CreateString(arr[i]));
    cJSON_AddItemToObject(root, key, a);
}

/* Parse a JSON object ({"node.name": 150, ...}) into a fixed name/percent
 * table ("audioDeviceMaxVolumes", docs/25-AUDIO-PERDEVICE-PLAN.md sec.3,
 * T2). Forgiving like get_string_array(): absent/non-object key leaves
 * arr/out_n untouched; a non-number value or an empty key name is skipped;
 * entries beyond cap are dropped. Values are clamped to 100-200 (100 is the
 * "no override" default, so a value of 100 is kept as an explicit entry
 * only because the caller asked for it -- dc_config_audio_set_max() is the
 * one that actually collapses 100 back to "no entry"). */
static void get_audio_max_map(const cJSON *root, const char *key, dc_audio_max_entry *arr, int cap,
                              int *out_n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(item))
        return;

    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item)
    {
        if (n >= cap)
            break;
        if (!cJSON_IsNumber(entry) || !entry->string || !entry->string[0])
            continue;
        int v = (int)entry->valuedouble;
        if (v < 100)
            v = 100;
        if (v > 200)
            v = 200;
        snprintf(arr[n].name, DC_CONFIG_AUDIO_NAME_MAX, "%s", entry->string);
        arr[n].max_percent = v;
        n++;
    }
    *out_n = n;
}

static void add_audio_max_map(cJSON *root, const char *key, const dc_audio_max_entry *arr, int n)
{
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < n; i++)
        cJSON_AddNumberToObject(o, arr[i].name, arr[i].max_percent);
    cJSON_AddItemToObject(root, key, o);
}

/* Parse a JSON object ({"node.name": "Alias"}) into a fixed name/alias table
 * ("audioDeviceAliases", docs/25-AUDIO-PERDEVICE-PLAN.md sec.3, T2). Same
 * forgiving semantics as get_audio_max_map(): a non-string value or empty
 * key name is skipped. */
static void get_audio_alias_map(const cJSON *root, const char *key, dc_audio_alias_entry *arr,
                                int cap, int *out_n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(item))
        return;

    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item)
    {
        if (n >= cap)
            break;
        if (!cJSON_IsString(entry) || !entry->valuestring || !entry->string || !entry->string[0])
            continue;
        snprintf(arr[n].name, DC_CONFIG_AUDIO_NAME_MAX, "%s", entry->string);
        snprintf(arr[n].alias, DC_CONFIG_AUDIO_ALIAS_MAX, "%s", entry->valuestring);
        n++;
    }
    *out_n = n;
}

static void add_audio_alias_map(cJSON *root, const char *key, const dc_audio_alias_entry *arr,
                                int n)
{
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < n; i++)
        cJSON_AddStringToObject(o, arr[i].name, arr[i].alias);
    cJSON_AddItemToObject(root, key, o);
}

static void get_bar_position(const cJSON *root, const char *key, dc_bar_position *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring)
        return;
    if (strcmp(item->valuestring, "bottom") == 0)
        *out = DC_BAR_POSITION_BOTTOM;
    else if (strcmp(item->valuestring, "top") == 0)
        *out = DC_BAR_POSITION_TOP;
}

/* Resolve themeMode ("dark"|"light"|"auto") + the DANKC_THEME_MODE env
 * override to a concrete light bool. "auto" follows the wall clock: light
 * during the day (06:00-18:00), dark at night. (Chosen over a fixed default so
 * "auto" is actually useful without a geoclue/sun-position dependency; can be
 * upgraded to real sunrise/sunset later.) */
bool dc_config_light_mode(void)
{
    const char *mode = config.theme_mode;
    const char *env = getenv("DANKC_THEME_MODE");
    if (env && *env)
        mode = env;
    if (strcmp(mode, "light") == 0)
        return true;
    if (strcmp(mode, "dark") == 0)
        return false;
    /* "auto" (or anything unrecognized) -> time of day. */
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    return lt.tm_hour >= 6 && lt.tm_hour < 18;
}

/* Apply the selected stock palette in the resolved mode, then overlay a
 * wallpaper-derived palette (same mode) when dynamic color is enabled. */
static void apply_theme(void)
{
    bool light = dc_config_light_mode();
    dc_theme_set_light(light);
    dc_theme_set(config.theme_id);
    if (config.dynamic_color && config.wallpaper[0]) {
        dc_theme generated;
        if (dc_dynamic_from_image(config.wallpaper, light, &generated)) {
            dc_theme_set_custom(&generated);
            dc_info("dynamic color from %s (%s)", config.wallpaper, light ? "light" : "dark");
        } else {
            dc_warn("dynamic color: could not read %s", config.wallpaper);
        }
    }
    dc_systheme_apply(&config);
}

/* Resolve the config.json path. Returns false if no HOME/XDG. */
static bool config_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%.480s/dankc/config.json", xdg);
    else if (home)
        snprintf(out, n, "%.480s/.config/dankc/config.json", home);
    else
        return false;
    return true;
}

void dc_config_load(void)
{
    /* defaults (struct initializer above) -> DMS import -> dankc's own
     * config.json. Import runs first so any key dankc's config.json actually
     * sets still wins (docs/12-BAR-SPEC.md sec.7, stage S5): the get_*()
     * helpers below only touch a field when its JSON key is present, so
     * layering config.json on top of the DMS-imported values is safe. */
    dc_dms_import(&config);

    char path[512];
    if (!config_path(path, sizeof(path))) {
        dc_info("no HOME; using default/DMS-imported config");
        apply_theme();
        return;
    }

    char *text = read_file(path);
    if (!text) {
        dc_info("no config at %s; using default/DMS-imported config (theme=%s)", path,
                config.theme_id);
        apply_theme();
        return;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        dc_warn("config.json parse error; using defaults");
        apply_theme();
        return;
    }

    get_string(root, "theme", config.theme_id, sizeof(config.theme_id));
    get_string(root, "themeMode", config.theme_mode, sizeof(config.theme_mode));
    get_bool(root, "clock24h", &config.clock_24h);
    get_bool(root, "showDate", &config.show_date);
    get_bool(root, "showSeconds", &config.show_seconds);
    get_bool(root, "animationsEnabled", &config.animations_enabled);
    get_float(root, "animationSpeed", &config.animation_speed, 0.25f, 4.0f);
    get_bool(root, "dynamicColor", &config.dynamic_color);
    get_string(root, "wallpaper", config.wallpaper, sizeof(config.wallpaper));

    get_bar_position(root, "barPosition", &config.bar_position);
    get_int(root, "barSpacing", &config.bar_spacing, 0, 64);
    get_int(root, "barInnerPadding", &config.bar_inner_padding, 0, 64);
    get_int(root, "barWidgetPadding", &config.bar_widget_padding, 0, 64);
    get_float(root, "barTransparency", &config.bar_transparency, 0.0f, 1.0f);
    get_float(root, "barWidgetTransparency", &config.bar_widget_transparency, 0.0f, 1.0f);

    get_bool(root, "weatherEnabled", &config.weather_enabled);
    get_double(root, "weatherLat", &config.weather_lat, -90.0, 90.0);
    get_double(root, "weatherLon", &config.weather_lon, -180.0, 180.0);
    get_bool(root, "weatherFahrenheit", &config.weather_fahrenheit);
    get_string(root, "weatherLocation", config.weather_location, sizeof(config.weather_location));

    get_bool(root, "nightlightEnabled", &config.nightlight_enabled);
    get_int(root, "nightlightTemp", &config.nightlight_temp, 2500, 6500);
    get_int(root, "nightlightScheduleMode", &config.nightlight_schedule_mode, 0, 2);
    get_string(root, "nightlightFrom", config.nightlight_from, sizeof(config.nightlight_from));
    get_string(root, "nightlightTo", config.nightlight_to, sizeof(config.nightlight_to));

    get_string_array(root, "barLeftWidgets", config.bar_left_widgets, DC_CONFIG_WIDGETS_MAX,
                     &config.bar_left_widgets_n);
    get_string_array(root, "barCenterWidgets", config.bar_center_widgets, DC_CONFIG_WIDGETS_MAX,
                     &config.bar_center_widgets_n);
    get_string_array(root, "barRightWidgets", config.bar_right_widgets, DC_CONFIG_WIDGETS_MAX,
                     &config.bar_right_widgets_n);

    get_int(root, "notifTimeoutLow", &config.notif_timeout_low_sec, 0, 120);
    get_int(root, "notifTimeoutNormal", &config.notif_timeout_normal_sec, 0, 120);
    get_int(root, "notifTimeoutCritical", &config.notif_timeout_critical_sec, 0, 120);
    get_bool(root, "dndEnabled", &config.dnd_enabled);
    get_int64(root, "dndUntilEpoch", &config.dnd_until_epoch, 0, INT64_MAX);
    get_int(root, "dndUntilHour", &config.dnd_until_hour, 0, 23);
    get_bool(root, "notifPrivacyMode", &config.notif_privacy_mode);
    get_rule_array(root, "notifAppRules", config.notif_rules, DC_CONFIG_NOTIF_RULES_MAX,
                   &config.notif_rules_n);

    get_bool(root, "soundsEnabled", &config.sounds_enabled);
    get_bool(root, "soundNewNotification", &config.notif_sound_enabled);
    get_float(root, "soundVolume", &config.sound_volume, 0.0f, 1.0f);

    get_bool(root, "launcherGridView", &config.launcher_grid_view);

    get_bool(root, "dockEnabled", &config.dock_enabled);
    get_bool(root, "dockAutoHide", &config.dock_auto_hide);
    get_int(root, "dockIconSize", &config.dock_icon_size, 16, 96);
    get_string_array(root, "dockPinned", config.dock_pinned, DC_CONFIG_DOCK_PINNED_MAX,
                     &config.dock_pinned_n);
    get_bool(root, "frameEnabled", &config.frame_enabled);
    get_float(root, "frameRadius", &config.frame_radius, 0.0f, 200.0f);
    get_bool(root, "materialBlur", &config.material_blur);
    get_bool(root, "autostartEnabled", &config.autostart_enabled);

    get_int(root, "osdPosition", &config.osd_position, 0, 3);
    get_int(root, "osdTimeoutMs", &config.osd_timeout_ms, 500, 10000);
    get_float(root, "fontScale", &config.font_scale, 0.8f, 1.5f);
    get_int(root, "firstDayOfWeek", &config.first_day_of_week, 0, 6);

    get_bool(root, "lockShowClock", &config.lock_show_clock);
    get_bool(root, "lockShowDate", &config.lock_show_date);
    get_bool(root, "lockShowPasswordField", &config.lock_show_password_field);
    get_bool(root, "lockUseWallpaperBg", &config.lock_use_wallpaper_bg);

    get_bool(root, "inputTouchpadTap", &config.input_touchpad_tap);
    get_bool(root, "inputTouchpadNaturalScroll", &config.input_touchpad_natural_scroll);
    get_bool(root, "inputTouchpadDwt", &config.input_touchpad_dwt);
    get_bool(root, "inputTouchpadDisabledOnExternalMouse",
             &config.input_touchpad_disabled_on_external_mouse);
    get_bool(root, "inputTouchpadAccelEnabled", &config.input_touchpad_accel_enabled);
    get_float(root, "inputTouchpadAccelSpeed", &config.input_touchpad_accel_speed, -1.0f, 1.0f);
    get_bool(root, "inputMouseNaturalScroll", &config.input_mouse_natural_scroll);
    get_bool(root, "inputMouseAccelEnabled", &config.input_mouse_accel_enabled);
    get_float(root, "inputMouseAccelSpeed", &config.input_mouse_accel_speed, -1.0f, 1.0f);
    get_bool(root, "inputKeyboardNumlock", &config.input_keyboard_numlock);
    get_string(root, "inputKeyboardLayout", config.input_keyboard_layout,
               sizeof(config.input_keyboard_layout));

    get_bool(root, "systemThemingEnabled", &config.systheme_enabled);
    get_bool(root, "systemThemeGtk", &config.systheme_gtk);
    get_bool(root, "systemThemeQt", &config.systheme_qt);
    get_bool(root, "systemThemeAlacritty", &config.systheme_alacritty);
    get_bool(root, "systemThemeVscode", &config.systheme_vscode);
    get_bool(root, "systemThemeKitty", &config.systheme_kitty);
    get_bool(root, "systemThemeFoot", &config.systheme_foot);

    get_bool(root, "systemThemeKvantum", &config.systheme_kvantum);
    get_bool(root, "systemThemeKde", &config.systheme_kde);
    get_bool(root, "systemThemeGhostty", &config.systheme_ghostty);
    get_bool(root, "systemThemeWezterm", &config.systheme_wezterm);
    get_bool(root, "systemThemeKonsole", &config.systheme_konsole);
    get_bool(root, "systemThemeXresources", &config.systheme_xresources);
    get_bool(root, "systemThemeZed", &config.systheme_zed);
    get_bool(root, "systemThemeHelix", &config.systheme_helix);
    get_bool(root, "systemThemeNeovim", &config.systheme_neovim);
    get_bool(root, "systemThemeVim", &config.systheme_vim);
    get_bool(root, "systemThemeSublime", &config.systheme_sublime);
    get_bool(root, "systemThemeEmacs", &config.systheme_emacs);
    get_bool(root, "systemThemeRofi", &config.systheme_rofi);
    get_bool(root, "systemThemeWofi", &config.systheme_wofi);
    get_bool(root, "systemThemeFuzzel", &config.systheme_fuzzel);
    get_bool(root, "systemThemeTofi", &config.systheme_tofi);
    get_bool(root, "systemThemeMako", &config.systheme_mako);
    get_bool(root, "systemThemeDunst", &config.systheme_dunst);
    get_bool(root, "systemThemeSwaync", &config.systheme_swaync);
    get_bool(root, "systemThemeBtop", &config.systheme_btop);
    get_bool(root, "systemThemeCava", &config.systheme_cava);
    get_bool(root, "systemThemeZathura", &config.systheme_zathura);
    get_bool(root, "systemThemeQutebrowser", &config.systheme_qutebrowser);
    get_bool(root, "systemThemeFirefox", &config.systheme_firefox);
    get_bool(root, "systemThemeDiscord", &config.systheme_discord);
    get_bool(root, "systemThemeSpicetify", &config.systheme_spicetify);
    get_bool(root, "systemThemeGtk2", &config.systheme_gtk2);

    get_int(root, "chargeLimit", &config.charge_limit, 50, 100);
    get_bool(root, "batteryNotifications", &config.battery_notifications);
    get_int(root, "lowBatteryThreshold", &config.low_battery_threshold, 5, 50);
    get_int(root, "criticalBatteryThreshold", &config.critical_battery_threshold, 2, 25);
    get_bool(root, "autoPowerSaver", &config.auto_power_saver);
    get_bool(root, "autoProfileSwitch", &config.auto_profile_switch);
    get_int(root, "profileOnAC", &config.profile_on_ac, 0, 2);
    get_int(root, "profileOnBattery", &config.profile_on_battery, 0, 2);

    get_bool(root, "idleTimeoutsEnabled", &config.idle_timeouts_enabled);
    get_int(root, "idleLockAcMin", &config.idle_lock_ac_min, 0, 240);
    get_int(root, "idleLockBatteryMin", &config.idle_lock_batt_min, 0, 240);
    get_int(root, "idleMonitorOffAcMin", &config.idle_monitor_off_ac_min, 0, 240);
    get_int(root, "idleMonitorOffBatteryMin", &config.idle_monitor_off_batt_min, 0, 240);
    get_int(root, "idleSuspendAcMin", &config.idle_suspend_ac_min, 0, 240);
    get_int(root, "idleSuspendBatteryMin", &config.idle_suspend_batt_min, 0, 240);
    get_int(root, "idleHibernateAcMin", &config.idle_hibernate_ac_min, 0, 240);
    get_int(root, "idleHibernateBatteryMin", &config.idle_hibernate_batt_min, 0, 240);

    get_audio_max_map(root, "audioDeviceMaxVolumes", config.audio_max_volumes,
                      DC_CONFIG_AUDIO_DEVICES_MAX, &config.audio_max_volumes_n);
    get_audio_alias_map(root, "audioDeviceAliases", config.audio_aliases,
                        DC_CONFIG_AUDIO_DEVICES_MAX, &config.audio_aliases_n);
    get_string_array_wide(root, "audioHiddenDevices", config.audio_hidden,
                          DC_CONFIG_AUDIO_DEVICES_MAX, &config.audio_hidden_n);
    cJSON_Delete(root);

    apply_theme();
    dc_info("config loaded: theme=%s clock24h=%d anim=%d/%.2fx dynamic=%d barPosition=%s",
            config.theme_id, config.clock_24h, config.animations_enabled,
            (double)config.animation_speed, config.dynamic_color,
            config.bar_position == DC_BAR_POSITION_BOTTOM ? "bottom" : "top");
}

dc_config *dc_config_mut(void)
{
    return &config;
}

void dc_config_reapply(void)
{
    apply_theme();
}

static void (*change_cb)(void *);
static void *change_ud;

void dc_config_set_change_cb(void (*cb)(void *ud), void *ud)
{
    change_cb = cb;
    change_ud = ud;
}

void dc_config_notify_changed(void)
{
    if (change_cb)
        change_cb(change_ud);
}

/* Create the parent directory of `path` (one level: ~/.config/dankc). */
static void ensure_parent_dir(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';
    /* Make grandparent then parent (mkdir is a no-op if they already exist). */
    char *slash2 = strrchr(dir, '/');
    if (slash2) {
        *slash2 = '\0';
        mkdir(dir, 0755);
        *slash2 = '/';
    }
    mkdir(dir, 0755);
}

void dc_config_save(void)
{
    char path[512];
    if (!config_path(path, sizeof(path)))
        return;
    ensure_parent_dir(path);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "theme", config.theme_id);
    cJSON_AddStringToObject(root, "themeMode", config.theme_mode);
    cJSON_AddBoolToObject(root, "clock24h", config.clock_24h);
    cJSON_AddBoolToObject(root, "showDate", config.show_date);
    cJSON_AddBoolToObject(root, "showSeconds", config.show_seconds);
    cJSON_AddBoolToObject(root, "animationsEnabled", config.animations_enabled);
    cJSON_AddNumberToObject(root, "animationSpeed", config.animation_speed);
    cJSON_AddBoolToObject(root, "dynamicColor", config.dynamic_color);
    if (config.wallpaper[0])
        cJSON_AddStringToObject(root, "wallpaper", config.wallpaper);

    cJSON_AddStringToObject(root, "barPosition",
                            config.bar_position == DC_BAR_POSITION_BOTTOM ? "bottom" : "top");
    cJSON_AddNumberToObject(root, "barSpacing", config.bar_spacing);
    cJSON_AddNumberToObject(root, "barInnerPadding", config.bar_inner_padding);
    cJSON_AddNumberToObject(root, "barWidgetPadding", config.bar_widget_padding);
    cJSON_AddNumberToObject(root, "barTransparency", (double)config.bar_transparency);
    cJSON_AddNumberToObject(root, "barWidgetTransparency", (double)config.bar_widget_transparency);

    cJSON_AddBoolToObject(root, "weatherEnabled", config.weather_enabled);
    cJSON_AddNumberToObject(root, "weatherLat", config.weather_lat);
    cJSON_AddNumberToObject(root, "weatherLon", config.weather_lon);
    cJSON_AddBoolToObject(root, "weatherFahrenheit", config.weather_fahrenheit);
    if (config.weather_location[0])
        cJSON_AddStringToObject(root, "weatherLocation", config.weather_location);

    cJSON_AddBoolToObject(root, "nightlightEnabled", config.nightlight_enabled);
    cJSON_AddNumberToObject(root, "nightlightTemp", config.nightlight_temp);
    cJSON_AddNumberToObject(root, "nightlightScheduleMode", config.nightlight_schedule_mode);
    if (config.nightlight_from[0])
        cJSON_AddStringToObject(root, "nightlightFrom", config.nightlight_from);
    if (config.nightlight_to[0])
        cJSON_AddStringToObject(root, "nightlightTo", config.nightlight_to);

    add_string_array(root, "barLeftWidgets", config.bar_left_widgets, config.bar_left_widgets_n);
    add_string_array(root, "barCenterWidgets", config.bar_center_widgets,
                     config.bar_center_widgets_n);
    add_string_array(root, "barRightWidgets", config.bar_right_widgets,
                     config.bar_right_widgets_n);

    cJSON_AddNumberToObject(root, "notifTimeoutLow", config.notif_timeout_low_sec);
    cJSON_AddNumberToObject(root, "notifTimeoutNormal", config.notif_timeout_normal_sec);
    cJSON_AddNumberToObject(root, "notifTimeoutCritical", config.notif_timeout_critical_sec);
    cJSON_AddBoolToObject(root, "dndEnabled", config.dnd_enabled);
    cJSON_AddNumberToObject(root, "dndUntilEpoch", (double)config.dnd_until_epoch);
    cJSON_AddNumberToObject(root, "dndUntilHour", config.dnd_until_hour);
    cJSON_AddBoolToObject(root, "notifPrivacyMode", config.notif_privacy_mode);
    add_rule_array(root, "notifAppRules", config.notif_rules, config.notif_rules_n);

    cJSON_AddBoolToObject(root, "soundsEnabled", config.sounds_enabled);
    cJSON_AddBoolToObject(root, "soundNewNotification", config.notif_sound_enabled);
    cJSON_AddNumberToObject(root, "soundVolume", (double)config.sound_volume);

    cJSON_AddBoolToObject(root, "launcherGridView", config.launcher_grid_view);

    cJSON_AddBoolToObject(root, "dockEnabled", config.dock_enabled);
    cJSON_AddBoolToObject(root, "dockAutoHide", config.dock_auto_hide);
    cJSON_AddNumberToObject(root, "dockIconSize", config.dock_icon_size);
    add_string_array(root, "dockPinned", config.dock_pinned, config.dock_pinned_n);
    cJSON_AddBoolToObject(root, "frameEnabled", config.frame_enabled);
    cJSON_AddNumberToObject(root, "frameRadius", (double)config.frame_radius);
    cJSON_AddBoolToObject(root, "materialBlur", config.material_blur);
    cJSON_AddBoolToObject(root, "autostartEnabled", config.autostart_enabled);

    cJSON_AddNumberToObject(root, "osdPosition", config.osd_position);
    cJSON_AddNumberToObject(root, "osdTimeoutMs", config.osd_timeout_ms);
    cJSON_AddNumberToObject(root, "fontScale", (double)config.font_scale);
    cJSON_AddNumberToObject(root, "firstDayOfWeek", config.first_day_of_week);

    cJSON_AddBoolToObject(root, "lockShowClock", config.lock_show_clock);
    cJSON_AddBoolToObject(root, "lockShowDate", config.lock_show_date);
    cJSON_AddBoolToObject(root, "lockShowPasswordField", config.lock_show_password_field);
    cJSON_AddBoolToObject(root, "lockUseWallpaperBg", config.lock_use_wallpaper_bg);

    cJSON_AddBoolToObject(root, "inputTouchpadTap", config.input_touchpad_tap);
    cJSON_AddBoolToObject(root, "inputTouchpadNaturalScroll", config.input_touchpad_natural_scroll);
    cJSON_AddBoolToObject(root, "inputTouchpadDwt", config.input_touchpad_dwt);
    cJSON_AddBoolToObject(root, "inputTouchpadDisabledOnExternalMouse",
                          config.input_touchpad_disabled_on_external_mouse);
    cJSON_AddBoolToObject(root, "inputTouchpadAccelEnabled", config.input_touchpad_accel_enabled);
    cJSON_AddNumberToObject(root, "inputTouchpadAccelSpeed",
                            (double)config.input_touchpad_accel_speed);
    cJSON_AddBoolToObject(root, "inputMouseNaturalScroll", config.input_mouse_natural_scroll);
    cJSON_AddBoolToObject(root, "inputMouseAccelEnabled", config.input_mouse_accel_enabled);
    cJSON_AddNumberToObject(root, "inputMouseAccelSpeed", (double)config.input_mouse_accel_speed);
    cJSON_AddBoolToObject(root, "inputKeyboardNumlock", config.input_keyboard_numlock);
    if (config.input_keyboard_layout[0])
        cJSON_AddStringToObject(root, "inputKeyboardLayout", config.input_keyboard_layout);

    cJSON_AddBoolToObject(root, "systemThemingEnabled", config.systheme_enabled);
    cJSON_AddBoolToObject(root, "systemThemeGtk", config.systheme_gtk);
    cJSON_AddBoolToObject(root, "systemThemeQt", config.systheme_qt);
    cJSON_AddBoolToObject(root, "systemThemeAlacritty", config.systheme_alacritty);
    cJSON_AddBoolToObject(root, "systemThemeVscode", config.systheme_vscode);
    cJSON_AddBoolToObject(root, "systemThemeKitty", config.systheme_kitty);
    cJSON_AddBoolToObject(root, "systemThemeFoot", config.systheme_foot);

    cJSON_AddBoolToObject(root, "systemThemeKvantum", config.systheme_kvantum);
    cJSON_AddBoolToObject(root, "systemThemeKde", config.systheme_kde);
    cJSON_AddBoolToObject(root, "systemThemeGhostty", config.systheme_ghostty);
    cJSON_AddBoolToObject(root, "systemThemeWezterm", config.systheme_wezterm);
    cJSON_AddBoolToObject(root, "systemThemeKonsole", config.systheme_konsole);
    cJSON_AddBoolToObject(root, "systemThemeXresources", config.systheme_xresources);
    cJSON_AddBoolToObject(root, "systemThemeZed", config.systheme_zed);
    cJSON_AddBoolToObject(root, "systemThemeHelix", config.systheme_helix);
    cJSON_AddBoolToObject(root, "systemThemeNeovim", config.systheme_neovim);
    cJSON_AddBoolToObject(root, "systemThemeVim", config.systheme_vim);
    cJSON_AddBoolToObject(root, "systemThemeSublime", config.systheme_sublime);
    cJSON_AddBoolToObject(root, "systemThemeEmacs", config.systheme_emacs);
    cJSON_AddBoolToObject(root, "systemThemeRofi", config.systheme_rofi);
    cJSON_AddBoolToObject(root, "systemThemeWofi", config.systheme_wofi);
    cJSON_AddBoolToObject(root, "systemThemeFuzzel", config.systheme_fuzzel);
    cJSON_AddBoolToObject(root, "systemThemeTofi", config.systheme_tofi);
    cJSON_AddBoolToObject(root, "systemThemeMako", config.systheme_mako);
    cJSON_AddBoolToObject(root, "systemThemeDunst", config.systheme_dunst);
    cJSON_AddBoolToObject(root, "systemThemeSwaync", config.systheme_swaync);
    cJSON_AddBoolToObject(root, "systemThemeBtop", config.systheme_btop);
    cJSON_AddBoolToObject(root, "systemThemeCava", config.systheme_cava);
    cJSON_AddBoolToObject(root, "systemThemeZathura", config.systheme_zathura);
    cJSON_AddBoolToObject(root, "systemThemeQutebrowser", config.systheme_qutebrowser);
    cJSON_AddBoolToObject(root, "systemThemeFirefox", config.systheme_firefox);
    cJSON_AddBoolToObject(root, "systemThemeDiscord", config.systheme_discord);
    cJSON_AddBoolToObject(root, "systemThemeSpicetify", config.systheme_spicetify);
    cJSON_AddBoolToObject(root, "systemThemeGtk2", config.systheme_gtk2);

    cJSON_AddNumberToObject(root, "chargeLimit", config.charge_limit);
    cJSON_AddBoolToObject(root, "batteryNotifications", config.battery_notifications);
    cJSON_AddNumberToObject(root, "lowBatteryThreshold", config.low_battery_threshold);
    cJSON_AddNumberToObject(root, "criticalBatteryThreshold", config.critical_battery_threshold);
    cJSON_AddBoolToObject(root, "autoPowerSaver", config.auto_power_saver);
    cJSON_AddBoolToObject(root, "autoProfileSwitch", config.auto_profile_switch);
    cJSON_AddNumberToObject(root, "profileOnAC", config.profile_on_ac);
    cJSON_AddNumberToObject(root, "profileOnBattery", config.profile_on_battery);

    cJSON_AddBoolToObject(root, "idleTimeoutsEnabled", config.idle_timeouts_enabled);
    cJSON_AddNumberToObject(root, "idleLockAcMin", config.idle_lock_ac_min);
    cJSON_AddNumberToObject(root, "idleLockBatteryMin", config.idle_lock_batt_min);
    cJSON_AddNumberToObject(root, "idleMonitorOffAcMin", config.idle_monitor_off_ac_min);
    cJSON_AddNumberToObject(root, "idleMonitorOffBatteryMin", config.idle_monitor_off_batt_min);
    cJSON_AddNumberToObject(root, "idleSuspendAcMin", config.idle_suspend_ac_min);
    cJSON_AddNumberToObject(root, "idleSuspendBatteryMin", config.idle_suspend_batt_min);
    cJSON_AddNumberToObject(root, "idleHibernateAcMin", config.idle_hibernate_ac_min);
    cJSON_AddNumberToObject(root, "idleHibernateBatteryMin", config.idle_hibernate_batt_min);

    add_audio_max_map(root, "audioDeviceMaxVolumes", config.audio_max_volumes,
                      config.audio_max_volumes_n);
    add_audio_alias_map(root, "audioDeviceAliases", config.audio_aliases, config.audio_aliases_n);
    add_string_array_wide(root, "audioHiddenDevices", config.audio_hidden, config.audio_hidden_n);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text)
        return;

    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fputc('\n', f);
        fclose(f);
        dc_info("config saved to %s", path);
    } else {
        dc_warn("could not write %s", path);
    }
    free(text);
}

/* --- Audio per-device config accessors/setters ---------------------------
 * docs/25-AUDIO-PERDEVICE-PLAN.md sec.3 (Config surface), T2. Keyed by
 * pipewire node.name (D2 in the plan doc: ids are session-local, node.name
 * is the only stable key). T3 (audio.c) is the first consumer -- these are
 * unused elsewhere for now. */

int dc_config_audio_max(const char *name)
{
    if (!name || !name[0])
        return 100;
    for (int i = 0; i < config.audio_max_volumes_n; i++) {
        if (strcmp(config.audio_max_volumes[i].name, name) == 0)
            return config.audio_max_volumes[i].max_percent;
    }
    return 100;
}

const char *dc_config_audio_alias(const char *name)
{
    if (!name || !name[0])
        return NULL;
    for (int i = 0; i < config.audio_aliases_n; i++) {
        if (strcmp(config.audio_aliases[i].name, name) == 0)
            return config.audio_aliases[i].alias;
    }
    return NULL;
}

bool dc_config_audio_hidden(const char *name)
{
    if (!name || !name[0])
        return false;
    for (int i = 0; i < config.audio_hidden_n; i++) {
        if (strcmp(config.audio_hidden[i], name) == 0)
            return true;
    }
    return false;
}

void dc_config_audio_set_max(const char *name, int max_percent)
{
    if (!name || !name[0])
        return;

    for (int i = 0; i < config.audio_max_volumes_n; i++) {
        if (strcmp(config.audio_max_volumes[i].name, name) != 0)
            continue;
        if (max_percent <= 100) {
            /* Reset to default: drop the entry (shift the rest down) so
             * config.json doesn't accumulate no-op overrides. */
            for (int j = i; j < config.audio_max_volumes_n - 1; j++)
                config.audio_max_volumes[j] = config.audio_max_volumes[j + 1];
            config.audio_max_volumes_n--;
        } else {
            config.audio_max_volumes[i].max_percent = max_percent > 200 ? 200 : max_percent;
        }
        return;
    }

    if (max_percent <= 100)
        return; /* already at the implicit default; nothing to add */

    if (config.audio_max_volumes_n >= DC_CONFIG_AUDIO_DEVICES_MAX) {
        /* Cap reached: evict the oldest entry to make room. */
        for (int j = 0; j < config.audio_max_volumes_n - 1; j++)
            config.audio_max_volumes[j] = config.audio_max_volumes[j + 1];
        config.audio_max_volumes_n--;
    }
    dc_audio_max_entry *e = &config.audio_max_volumes[config.audio_max_volumes_n];
    snprintf(e->name, DC_CONFIG_AUDIO_NAME_MAX, "%s", name);
    e->max_percent = max_percent > 200 ? 200 : max_percent;
    config.audio_max_volumes_n++;
}

void dc_config_audio_set_alias(const char *name, const char *alias)
{
    if (!name || !name[0])
        return;

    for (int i = 0; i < config.audio_aliases_n; i++) {
        if (strcmp(config.audio_aliases[i].name, name) != 0)
            continue;
        if (!alias || !alias[0]) {
            for (int j = i; j < config.audio_aliases_n - 1; j++)
                config.audio_aliases[j] = config.audio_aliases[j + 1];
            config.audio_aliases_n--;
        } else {
            snprintf(config.audio_aliases[i].alias, DC_CONFIG_AUDIO_ALIAS_MAX, "%s", alias);
        }
        return;
    }

    if (!alias || !alias[0])
        return; /* already unaliased; nothing to add */

    if (config.audio_aliases_n >= DC_CONFIG_AUDIO_DEVICES_MAX) {
        for (int j = 0; j < config.audio_aliases_n - 1; j++)
            config.audio_aliases[j] = config.audio_aliases[j + 1];
        config.audio_aliases_n--;
    }
    dc_audio_alias_entry *e = &config.audio_aliases[config.audio_aliases_n];
    snprintf(e->name, DC_CONFIG_AUDIO_NAME_MAX, "%s", name);
    snprintf(e->alias, DC_CONFIG_AUDIO_ALIAS_MAX, "%s", alias);
    config.audio_aliases_n++;
}

void dc_config_audio_set_hidden(const char *name, bool hidden)
{
    if (!name || !name[0])
        return;

    for (int i = 0; i < config.audio_hidden_n; i++) {
        if (strcmp(config.audio_hidden[i], name) != 0)
            continue;
        if (!hidden) {
            for (int j = i; j < config.audio_hidden_n - 1; j++)
                snprintf(config.audio_hidden[j], DC_CONFIG_AUDIO_NAME_MAX, "%s",
                         config.audio_hidden[j + 1]);
            config.audio_hidden_n--;
        }
        return;
    }

    if (!hidden)
        return; /* already visible; nothing to add */

    if (config.audio_hidden_n >= DC_CONFIG_AUDIO_DEVICES_MAX) {
        for (int j = 0; j < config.audio_hidden_n - 1; j++)
            snprintf(config.audio_hidden[j], DC_CONFIG_AUDIO_NAME_MAX, "%s",
                     config.audio_hidden[j + 1]);
        config.audio_hidden_n--;
    }
    snprintf(config.audio_hidden[config.audio_hidden_n], DC_CONFIG_AUDIO_NAME_MAX, "%s", name);
    config.audio_hidden_n++;
}
