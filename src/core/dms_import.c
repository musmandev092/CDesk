#include "core/dms_import.h"

#include "core/log.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

/* Mirrors config.c's read_file(): whole-file slurp with a 1 MiB sanity cap
 * (settings.json/session.json are both a few KB in practice). */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1 << 20) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static bool xdg_path(char *out, size_t n, const char *xdg_var, const char *home_default,
                     const char *rel)
{
    const char *xdg = getenv(xdg_var);
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%.400s/%s", xdg, rel);
    else if (home)
        snprintf(out, n, "%.400s/%s/%s", home, home_default, rel);
    else
        return false;
    return true;
}

static void imp_bool(const cJSON *root, const char *key, bool *out, int *applied)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        (*applied)++;
    }
}

static void imp_int(const cJSON *root, const char *key, int *out, int lo, int hi, int *applied)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        int v = item->valueint;
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
        *out = v;
        (*applied)++;
    }
}

static void imp_float(const cJSON *root, const char *key, float *out, float lo, float hi,
                      int *applied)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        float v = (float)item->valuedouble;
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
        *out = v;
        (*applied)++;
    }
}

/* Like config.c's get_string_array(), but reports whether it actually wrote
 * anything (dc_dms_import()'s log line summarizes on that). */
static bool imp_widget_array(const cJSON *root, const char *key,
                             char arr[][DC_CONFIG_WIDGET_ID_MAX], int cap, int *out_n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsArray(item))
        return false;

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
    if (n == 0)
        return false;
    *out_n = n;
    return true;
}

/* DMS's currentThemeName ("green", "Monochrome", ...) against dankc's stock
 * theme table (id or display name, case-insensitive). Leaves cfg->theme_id
 * untouched if nothing matches. */
static bool apply_theme_name(dc_config *cfg, const char *name)
{
    int n = dc_theme_count();
    for (int i = 0; i < n; i++) {
        const char *id = dc_theme_id_at(i);
        const char *nm = dc_theme_name_at(i);
        if ((id && strcasecmp(id, name) == 0) || (nm && strcasecmp(nm, name) == 0)) {
            snprintf(cfg->theme_id, sizeof(cfg->theme_id), "%s", id);
            return true;
        }
    }
    return false;
}

/* Parse a "lat,lon" string (DMS's weatherCoordinates format). */
static bool parse_lat_lon(const char *s, double *lat, double *lon)
{
    char *end = NULL;
    double a = strtod(s, &end);
    if (end == s)
        return false;
    while (*end == ',' || *end == ' ')
        end++;
    char *end2 = NULL;
    double b = strtod(end, &end2);
    if (end2 == end)
        return false;
    if (a < -90.0 || a > 90.0 || b < -180.0 || b > 180.0)
        return false;
    *lat = a;
    *lon = b;
    return true;
}

static bool import_coords_from(const cJSON *root, dc_config *cfg)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "weatherCoordinates");
    if (!cJSON_IsString(item) || !item->valuestring)
        return false;
    return parse_lat_lon(item->valuestring, &cfg->weather_lat, &cfg->weather_lon);
}

/* Newer/older DMS versions may or may not keep weatherCoordinates in
 * settings.json; the user's live install keeps it in session.json instead
 * (~/.local/state/DankMaterialShell/session.json) — check that too. */
static bool import_coords_from_session(dc_config *cfg)
{
    char path[DC_CONFIG_PATH_MAX];
    if (!xdg_path(path, sizeof(path), "XDG_STATE_HOME", ".local/state",
                 "DankMaterialShell/session.json"))
        return false;

    char *text = read_file(path);
    if (!text)
        return false;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root)
        return false;
    bool ok = import_coords_from(root, cfg);
    cJSON_Delete(root);
    return ok;
}

bool dc_dms_import(dc_config *cfg)
{
    char path[DC_CONFIG_PATH_MAX];
    if (!xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config",
                 "DankMaterialShell/settings.json")) {
        dc_debug("dms import: no HOME; skipping");
        return false;
    }

    char *text = read_file(path);
    if (!text) {
        dc_debug("dms import: no %s; skipping (not a DMS switcher)", path);
        return false;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        dc_warn("dms import: %s parse error; skipping", path);
        return false;
    }

    int applied = 0;

    imp_bool(root, "use24HourClock", &cfg->clock_24h, &applied);
    imp_bool(root, "weatherEnabled", &cfg->weather_enabled, &applied);
    imp_bool(root, "useFahrenheit", &cfg->weather_fahrenheit, &applied);

    /* fontScale/animationSpeed: dc_config has no fontScale counterpart at
     * all (skipped). It does have animation_speed, but that's a 0.25x-4x
     * duration MULTIPLIER, while DMS's animationSpeed is a 1-5 PRESET index
     * (see docs/12-BAR-SPEC.md sec.0: "speed preset 4 with
     * customAnimationDuration: 100") — the two scales aren't compatible, so
     * mapping the raw number in would silently corrupt animation speed
     * rather than import it. Left unmapped until dc_config grows a
     * DMS-shaped preset field. */

    bool theme_applied = false;
    const cJSON *theme_name = cJSON_GetObjectItemCaseSensitive(root, "currentThemeName");
    if (cJSON_IsString(theme_name) && theme_name->valuestring) {
        theme_applied = apply_theme_name(cfg, theme_name->valuestring);
        if (theme_applied)
            applied++;
        else
            dc_warn("dms import: unknown theme '%s'; keeping default '%s'",
                    theme_name->valuestring, cfg->theme_id);
    }

    bool widgets_applied = false;
    const cJSON *bar_configs = cJSON_GetObjectItemCaseSensitive(root, "barConfigs");
    const cJSON *bar0 = cJSON_IsArray(bar_configs) ? cJSON_GetArrayItem(bar_configs, 0) : NULL;
    if (cJSON_IsObject(bar0)) {
        const cJSON *pos = cJSON_GetObjectItemCaseSensitive(bar0, "position");
        if (cJSON_IsNumber(pos)) {
            cfg->bar_position = pos->valueint == 1 ? DC_BAR_POSITION_BOTTOM : DC_BAR_POSITION_TOP;
            applied++;
        }
        imp_int(bar0, "spacing", &cfg->bar_spacing, 0, 64, &applied);
        imp_int(bar0, "innerPadding", &cfg->bar_inner_padding, 0, 64, &applied);
        imp_int(bar0, "widgetPadding", &cfg->bar_widget_padding, 0, 64, &applied);
        imp_float(bar0, "transparency", &cfg->bar_transparency, 0.0f, 1.0f, &applied);
        imp_float(bar0, "widgetTransparency", &cfg->bar_widget_transparency, 0.0f, 1.0f, &applied);

        bool l = imp_widget_array(bar0, "leftWidgets", cfg->bar_left_widgets,
                                  DC_CONFIG_WIDGETS_MAX, &cfg->bar_left_widgets_n);
        bool c = imp_widget_array(bar0, "centerWidgets", cfg->bar_center_widgets,
                                  DC_CONFIG_WIDGETS_MAX, &cfg->bar_center_widgets_n);
        bool r = imp_widget_array(bar0, "rightWidgets", cfg->bar_right_widgets,
                                  DC_CONFIG_WIDGETS_MAX, &cfg->bar_right_widgets_n);
        widgets_applied = l || c || r;
        if (widgets_applied)
            applied++;
    }

    /* weatherCoordinates: try settings.json first, then session.json (the
     * user's live DMS install keeps it there — see import_coords_from_session
     * above). */
    bool coords_applied = import_coords_from(root, cfg);
    cJSON_Delete(root);
    if (!coords_applied)
        coords_applied = import_coords_from_session(cfg);
    if (coords_applied)
        applied++;

    dc_info("dms import: %s -> position=%s widgets=%s theme=%s(%s) clock24h=%d weatherEnabled=%d "
            "fahrenheit=%d coords=%s (%d field%s applied)",
            path, cfg->bar_position == DC_BAR_POSITION_BOTTOM ? "bottom" : "top",
            widgets_applied ? "L/C/R" : "no", cfg->theme_id, theme_applied ? "imported" : "default",
            cfg->clock_24h, cfg->weather_enabled, cfg->weather_fahrenheit,
            coords_applied ? "yes" : "no", applied, applied == 1 ? "" : "s");

    return applied > 0;
}
