#include "core/config.h"

#include "core/dms_import.h"
#include "core/log.h"
#include "theme/dynamic.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

/* DMS-matching defaults. */
static dc_config config = {
    .theme_id = "green",
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

/* Apply the selected stock palette, then overlay a wallpaper-derived palette
 * when dynamic color is enabled. */
static void apply_theme(void)
{
    dc_theme_set(config.theme_id);
    if (config.dynamic_color && config.wallpaper[0]) {
        dc_theme generated;
        if (dc_dynamic_from_image(config.wallpaper, &generated)) {
            dc_theme_set_custom(&generated);
            dc_info("dynamic color from %s", config.wallpaper);
        } else {
            dc_warn("dynamic color: could not read %s", config.wallpaper);
        }
    }
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

    add_string_array(root, "barLeftWidgets", config.bar_left_widgets, config.bar_left_widgets_n);
    add_string_array(root, "barCenterWidgets", config.bar_center_widgets,
                     config.bar_center_widgets_n);
    add_string_array(root, "barRightWidgets", config.bar_right_widgets,
                     config.bar_right_widgets_n);

    cJSON_AddNumberToObject(root, "notifTimeoutLow", config.notif_timeout_low_sec);
    cJSON_AddNumberToObject(root, "notifTimeoutNormal", config.notif_timeout_normal_sec);
    cJSON_AddNumberToObject(root, "notifTimeoutCritical", config.notif_timeout_critical_sec);
    cJSON_AddBoolToObject(root, "dndEnabled", config.dnd_enabled);

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
