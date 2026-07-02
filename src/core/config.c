#include "core/config.h"

#include "core/log.h"
#include "theme/dynamic.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* DMS-matching defaults. */
static dc_config config = {
    .theme_id = "green",
    .clock_24h = true,
    .show_date = true,
    .animations_enabled = true,
    .animation_speed = 1.0f,
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

void dc_config_load(void)
{
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[512];
    if (xdg && *xdg)
        snprintf(path, sizeof(path), "%.480s/dankc/config.json", xdg);
    else if (home)
        snprintf(path, sizeof(path), "%.480s/.config/dankc/config.json", home);
    else {
        dc_info("no HOME; using default config");
        apply_theme();
        return;
    }

    char *text = read_file(path);
    if (!text) {
        dc_info("no config at %s; using defaults (theme=%s)", path, config.theme_id);
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
    get_bool(root, "animationsEnabled", &config.animations_enabled);
    get_float(root, "animationSpeed", &config.animation_speed, 0.25f, 4.0f);
    get_bool(root, "dynamicColor", &config.dynamic_color);
    get_string(root, "wallpaper", config.wallpaper, sizeof(config.wallpaper));
    cJSON_Delete(root);

    apply_theme();
    dc_info("config loaded: theme=%s clock24h=%d anim=%d/%.2fx dynamic=%d", config.theme_id,
            config.clock_24h, config.animations_enabled, (double)config.animation_speed,
            config.dynamic_color);
}
