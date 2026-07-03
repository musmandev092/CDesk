#include "services/display.h"

#include "core/log.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

/* --- small cJSON helpers (mirrors src/niri/niri.c's json_num/json_bool/json_str) */

static double json_num(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : 0.0;
}

static bool json_bool(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(item);
}

static void json_str(char *dst, size_t cap, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

/* --- one-shot raw socket request -----------------------------------------
 *
 * docs/03-SERVICES.md sec.12: "Use one connection per one-shot request";
 * EventStream (src/niri/niri.c) keeps its own dedicated long-lived
 * connection, so reads here open/close their own rather than reusing it.
 * Blocking by design -- bounded by a local AF_UNIX round-trip, not worth an
 * async round-trip for a Settings-panel read. Returns a malloc'd NUL
 * terminated response line (caller frees), or NULL on any failure.
 */
static char *niri_request(const char *request_json)
{
    const char *path = getenv("NIRI_SOCKET");
    if (!path || !*path) {
        dc_warn("display: NIRI_SOCKET unset");
        return NULL;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        dc_warn("display: socket() failed");
        return NULL;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dc_warn("display: failed to connect NIRI_SOCKET (%s)", path);
        close(fd);
        return NULL;
    }

    size_t req_len = strlen(request_json);
    if (write(fd, request_json, req_len) != (ssize_t)req_len) {
        dc_warn("display: short write requesting %s", request_json);
        close(fd);
        return NULL;
    }

    /* Read until we see a newline (one reply line) or the peer closes. */
    size_t cap = 256 * 1024;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return NULL;
    }
    size_t len = 0;
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = grown;
        }
        ssize_t got = read(fd, buf + len, cap - len - 1);
        if (got < 0) {
            dc_warn("display: read failed on niri socket");
            free(buf);
            close(fd);
            return NULL;
        }
        if (got == 0)
            break; /* peer closed */
        len += (size_t)got;
        if (memchr(buf + len - (size_t)got, '\n', (size_t)got))
            break;
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}

/* --- transform name table ------------------------------------------------ */

static const char *const transform_names[] = {
    [DC_DISPLAY_TRANSFORM_NORMAL] = "normal",
    [DC_DISPLAY_TRANSFORM_90] = "90",
    [DC_DISPLAY_TRANSFORM_180] = "180",
    [DC_DISPLAY_TRANSFORM_270] = "270",
    [DC_DISPLAY_TRANSFORM_FLIPPED] = "flipped",
    [DC_DISPLAY_TRANSFORM_FLIPPED_90] = "flipped-90",
    [DC_DISPLAY_TRANSFORM_FLIPPED_180] = "flipped-180",
    [DC_DISPLAY_TRANSFORM_FLIPPED_270] = "flipped-270",
};

const char *dc_display_transform_name(dc_display_transform t)
{
    if ((size_t)t >= sizeof(transform_names) / sizeof(transform_names[0]))
        return "normal";
    return transform_names[t];
}

dc_display_transform dc_display_transform_from_name(const char *name)
{
    if (!name)
        return DC_DISPLAY_TRANSFORM_NORMAL;
    /* niri's JSON reports "Normal", "Rotated90", ... (Rust enum Debug/Serde
     * shape) OR the lowercase CLI spelling; accept both defensively. */
    if (!strcasecmp(name, "normal"))
        return DC_DISPLAY_TRANSFORM_NORMAL;
    if (!strcasecmp(name, "90") || !strcasecmp(name, "rotated90"))
        return DC_DISPLAY_TRANSFORM_90;
    if (!strcasecmp(name, "180") || !strcasecmp(name, "rotated180"))
        return DC_DISPLAY_TRANSFORM_180;
    if (!strcasecmp(name, "270") || !strcasecmp(name, "rotated270"))
        return DC_DISPLAY_TRANSFORM_270;
    if (!strcasecmp(name, "flipped"))
        return DC_DISPLAY_TRANSFORM_FLIPPED;
    if (!strcasecmp(name, "flipped-90") || !strcasecmp(name, "flipped90"))
        return DC_DISPLAY_TRANSFORM_FLIPPED_90;
    if (!strcasecmp(name, "flipped-180") || !strcasecmp(name, "flipped180"))
        return DC_DISPLAY_TRANSFORM_FLIPPED_180;
    if (!strcasecmp(name, "flipped-270") || !strcasecmp(name, "flipped270"))
        return DC_DISPLAY_TRANSFORM_FLIPPED_270;
    return DC_DISPLAY_TRANSFORM_NORMAL;
}

void dc_display_format_refresh(int refresh_mhz, char *buf, size_t buf_cap)
{
    snprintf(buf, buf_cap, "%d.%03d", refresh_mhz / 1000, refresh_mhz % 1000);
}

/* --- read: dc_display_list ------------------------------------------------ */

static void parse_one_output(dc_display_info *info, const char *conn_name, const cJSON *entry)
{
    memset(info, 0, sizeof(*info));
    /* Prefer the object's own "name" field; fall back to the map key. */
    json_str(info->name, sizeof(info->name), entry, "name");
    if (!info->name[0])
        strncpy(info->name, conn_name, sizeof(info->name) - 1);
    json_str(info->make, sizeof(info->make), entry, "make");
    json_str(info->model, sizeof(info->model), entry, "model");
    json_str(info->serial, sizeof(info->serial), entry, "serial");

    const cJSON *modes = cJSON_GetObjectItemCaseSensitive(entry, "modes");
    int n = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, modes)
    {
        if (n >= DC_DISPLAY_MAX_MODES)
            break;
        dc_display_mode *mode = &info->modes[n++];
        mode->width = (int)json_num(m, "width");
        mode->height = (int)json_num(m, "height");
        mode->refresh_mhz = (int)json_num(m, "refresh_rate");
        mode->is_preferred = json_bool(m, "is_preferred");
    }
    info->mode_count = n;

    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(entry, "current_mode");
    info->current_mode_idx = cJSON_IsNumber(cur) ? cur->valueint : -1;

    info->vrr_supported = json_bool(entry, "vrr_supported");
    info->vrr_enabled = json_bool(entry, "vrr_enabled");

    const cJSON *logical = cJSON_GetObjectItemCaseSensitive(entry, "logical");
    if (cJSON_IsObject(logical)) {
        info->enabled = true;
        info->x = (int)json_num(logical, "x");
        info->y = (int)json_num(logical, "y");
        info->logical_width = (int)json_num(logical, "width");
        info->logical_height = (int)json_num(logical, "height");
        const cJSON *scale = cJSON_GetObjectItemCaseSensitive(logical, "scale");
        info->scale = cJSON_IsNumber(scale) ? scale->valuedouble : 1.0;
        char xform[32];
        json_str(xform, sizeof(xform), logical, "transform");
        info->transform = dc_display_transform_from_name(xform);
    } else {
        info->enabled = false;
        info->scale = 1.0;
        info->transform = DC_DISPLAY_TRANSFORM_NORMAL;
    }
}

int dc_display_list(dc_display_info out[DC_DISPLAY_MAX_OUTPUTS])
{
    memset(out, 0, sizeof(*out) * DC_DISPLAY_MAX_OUTPUTS);

    char *reply = niri_request("\"Outputs\"\n");
    if (!reply)
        return 0;

    cJSON *root = cJSON_Parse(reply);
    free(reply);
    if (!root) {
        dc_warn("display: failed to parse Outputs reply JSON");
        return 0;
    }

    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "Ok");
    const cJSON *outputs = cJSON_GetObjectItemCaseSensitive(ok, "Outputs");
    if (!cJSON_IsObject(outputs)) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "Err");
        if (cJSON_IsString(err))
            dc_warn("display: niri Outputs request failed: %s", err->valuestring);
        cJSON_Delete(root);
        return 0;
    }

    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, outputs)
    {
        if (n >= DC_DISPLAY_MAX_OUTPUTS)
            break;
        parse_one_output(&out[n], entry->string, entry);
        n++;
    }
    cJSON_Delete(root);

    /* Second request: which output currently has focus. Best-effort -- if it
     * fails, every entry simply stays is_focused=false. */
    char *focused_reply = niri_request("\"FocusedOutput\"\n");
    if (focused_reply) {
        cJSON *froot = cJSON_Parse(focused_reply);
        free(focused_reply);
        if (froot) {
            const cJSON *fok = cJSON_GetObjectItemCaseSensitive(froot, "Ok");
            const cJSON *focused = cJSON_GetObjectItemCaseSensitive(fok, "FocusedOutput");
            char focused_name[DC_DISPLAY_NAME_MAX];
            json_str(focused_name, sizeof(focused_name), focused, "name");
            if (focused_name[0]) {
                for (int i = 0; i < n; i++)
                    if (strcmp(out[i].name, focused_name) == 0)
                        out[i].is_focused = true;
            }
            cJSON_Delete(froot);
        }
    }

    return n;
}
