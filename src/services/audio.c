#include "services/audio.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"

#include "cJSON.h"

#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* docs/25-AUDIO-PERDEVICE-PLAN.md T1: a single async `pw-dump` enumeration
 * replaces the old per-purpose wpctl forks (one async fork+pipe for the
 * default sink's volume/mute, two private synchronous popen() readers for
 * the default source's mute and the default sink's display name). One
 * `pw-dump` call lists every Audio/Sink and Audio/Source node with its
 * volume/mute/default-ness in one shot; the legacy readers below just look
 * up the is_default entry in this cache instead of running their own wpctl
 * round trip.
 *
 * Cache window: 5s (docs plan D1/T1), same "return possibly-stale, kick an
 * async refresh in the background, never block" shape as the old
 * DC_AUDIO_CACHE_SECONDS mechanism this replaces. Writes (dc_audio_set_volume,
 * dc_audio_device_set_volume/toggle_mute/set_default) force the cache stale
 * and kick an immediate refresh so the caller's own change is reflected
 * within about one pw-dump round trip. */
#define DC_AUDIO_DEVICE_CACHE_SECONDS 5
#define DC_AUDIO_MAX_DEVICES 32
#define DC_AUDIO_PWDUMP_CAP (4u * 1024u * 1024u) /* hard cap, docs plan T1 risk note */

typedef struct {
    dc_audio_device sinks[DC_AUDIO_MAX_DEVICES];
    int n_sinks;
    dc_audio_device sources[DC_AUDIO_MAX_DEVICES];
    int n_sources;
} dc_audio_devlist;

static dc_audio_devlist g_devices;
static bool g_devices_valid = false; /* ever populated (attempted at least once) */
static time_t g_devices_time = 0;

/* --- async pw-dump fetch ---------------------------------------------------
 *
 * Same fork+non-blocking-fd-on-the-loop shape as the old g_fetch (and
 * services/net.c's wifi scan / services/clipboard.c's transfer_read), but
 * `pw-dump`'s JSON output can run 100KB+ (this codebase's fixed 128-byte
 * g_fetch buffer is unusable for it), so the read buffer here grows
 * geometrically like clipboard.c's transfer_read does, capped at
 * DC_AUDIO_PWDUMP_CAP. */
static struct {
    struct dc_loop *loop;
    bool active;
    pid_t pid;
    int fd;
    char *buf;
    size_t len;
    size_t cap;
} g_pwdump = {.fd = -1};

/* --- small cJSON helpers (mirrors src/services/display.c's json_num/_bool/_str) */

static bool json_str(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(out, cap, "%s", item->valuestring);
        return true;
    }
    if (cap)
        out[0] = '\0';
    return false;
}

/* Find WirePlumber's `metadata.name == "default"` object and pull the
 * `default.audio.sink`/`default.audio.source` node names out of it, e.g.:
 *   { "props": { "metadata.name": "default" },
 *     "metadata": [ { "key": "default.audio.sink",
 *                     "value": { "name": "alsa_output...." } }, ... ] }
 * These are names, not ids, so is_default gets set on the matching node
 * below by name (D2: node.name is the stable per-device key). */
static void find_default_names(const cJSON *root, char *sink_name, size_t sink_cap,
                                char *source_name, size_t source_cap)
{
    sink_name[0] = '\0';
    source_name[0] = '\0';

    const cJSON *node;
    cJSON_ArrayForEach(node, root)
    {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(node, "type");
        if (!cJSON_IsString(type) || !type->valuestring)
            continue;
        if (strcmp(type->valuestring, "PipeWire:Interface:Metadata") != 0)
            continue;

        const cJSON *props = cJSON_GetObjectItemCaseSensitive(node, "props");
        char mname[32];
        if (!json_str(props, "metadata.name", mname, sizeof(mname)))
            continue;
        if (strcmp(mname, "default") != 0)
            continue;

        const cJSON *mdlist = cJSON_GetObjectItemCaseSensitive(node, "metadata");
        const cJSON *entry;
        cJSON_ArrayForEach(entry, mdlist)
        {
            const cJSON *key = cJSON_GetObjectItemCaseSensitive(entry, "key");
            if (!cJSON_IsString(key) || !key->valuestring)
                continue;
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "value");
            if (strcmp(key->valuestring, "default.audio.sink") == 0)
                json_str(value, "name", sink_name, sink_cap);
            else if (strcmp(key->valuestring, "default.audio.source") == 0)
                json_str(value, "name", source_name, source_cap);
        }
        break; /* only one "default" metadata object */
    }
}

/* Parse a single pw-dump node object into `dev` if it's an Audio/Sink or
 * Audio/Source (returns 0/1/2 for "skip"/"sink"/"source"; `dev` is only
 * filled in on a non-zero return). Reads info.props for identity, and
 * info.params.Props[0] for the live channelVolumes/mute (the same shape
 * `wpctl` itself reads from). */
static int parse_device_node(const cJSON *node, dc_audio_device *dev)
{
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(node, "info");
    if (!cJSON_IsObject(info))
        return 0;
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(info, "props");
    if (!cJSON_IsObject(props))
        return 0;

    char mclass[32];
    if (!json_str(props, "media.class", mclass, sizeof(mclass)))
        return 0;
    bool is_sink = strcmp(mclass, "Audio/Sink") == 0;
    bool is_source = strcmp(mclass, "Audio/Source") == 0;
    if (!is_sink && !is_source)
        return 0; /* Stream media.class and everything else: not this task's list (T7) */

    memset(dev, 0, sizeof(*dev));
    json_str(props, "node.name", dev->name, sizeof(dev->name));
    if (is_source) {
        size_t nlen = strlen(dev->name);
        static const char suffix[] = ".monitor";
        size_t slen = sizeof(suffix) - 1;
        if (nlen >= slen && strcmp(dev->name + nlen - slen, suffix) == 0)
            return 0; /* monitor source, not a real capture device */
    }
    json_str(props, "node.description", dev->desc, sizeof(dev->desc));

    const cJSON *idv = cJSON_GetObjectItemCaseSensitive(node, "id");
    dev->id = cJSON_IsNumber(idv) ? (uint32_t)idv->valuedouble : 0;

    const cJSON *params = cJSON_GetObjectItemCaseSensitive(info, "params");
    const cJSON *propsarr = cJSON_GetObjectItemCaseSensitive(params, "Props");
    const cJSON *props0 = cJSON_IsArray(propsarr) ? cJSON_GetArrayItem(propsarr, 0) : NULL;

    float max_channel_vol = 0.0f;
    if (props0) {
        const cJSON *chvols = cJSON_GetObjectItemCaseSensitive(props0, "channelVolumes");
        const cJSON *ch;
        cJSON_ArrayForEach(ch, chvols)
        {
            if (cJSON_IsNumber(ch) && (float)ch->valuedouble > max_channel_vol)
                max_channel_vol = (float)ch->valuedouble;
        }
        dev->muted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props0, "mute"));
    }
    if (max_channel_vol < 0.0f)
        max_channel_vol = 0.0f;
    /* D1: wpctl/WirePlumber's own volume percent is cubic, not linear --
     * cbrtf() here is the read-side half of that; wpctl set-volume already
     * takes a plain percent on the write side, no inverse conversion. */
    dev->volume = (int)(cbrtf(max_channel_vol) * 100.0f + 0.5f);

    return is_sink ? 1 : 2;
}

static void parse_pwdump(const char *json_text, dc_audio_devlist *out)
{
    memset(out, 0, sizeof(*out));
    if (!json_text || !*json_text)
        return;

    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        dc_warn("audio: pw-dump JSON parse failed");
        return;
    }

    char default_sink_name[96];
    char default_source_name[96];
    find_default_names(root, default_sink_name, sizeof(default_sink_name), default_source_name,
                        sizeof(default_source_name));

    const cJSON *node;
    cJSON_ArrayForEach(node, root)
    {
        dc_audio_device dev;
        int kind = parse_device_node(node, &dev);
        if (kind == 0)
            continue;

        if (kind == 1) {
            dev.is_default = default_sink_name[0] && strcmp(dev.name, default_sink_name) == 0;
            if (out->n_sinks < DC_AUDIO_MAX_DEVICES)
                out->sinks[out->n_sinks++] = dev;
        } else {
            dev.is_default = default_source_name[0] && strcmp(dev.name, default_source_name) == 0;
            if (out->n_sources < DC_AUDIO_MAX_DEVICES)
                out->sources[out->n_sources++] = dev;
        }
    }

    cJSON_Delete(root);
}

static void pwdump_finish(bool eof_reached)
{
    dc_loop_remove_fd(g_pwdump.loop, g_pwdump.fd);
    close(g_pwdump.fd);
    g_pwdump.fd = -1;
    g_pwdump.active = false;

    dc_audio_devlist result;
    if (eof_reached && g_pwdump.buf && g_pwdump.len > 0) {
        g_pwdump.buf[g_pwdump.len] = '\0';
        parse_pwdump(g_pwdump.buf, &result);
    } else {
        memset(&result, 0, sizeof(result)); /* pw-dump missing/failed/empty: 0 devices */
    }

    free(g_pwdump.buf);
    g_pwdump.buf = NULL;
    g_pwdump.len = 0;
    g_pwdump.cap = 0;

    g_devices = result;
    g_devices_valid = true;
    g_devices_time = time(NULL);
}

/* Pipe is readable: accumulate into a growable buffer until EOF, then parse
 * (same drain shape as clipboard.c's transfer_read(), minus the
 * image/overflow-truncation branch -- pw-dump's output is JSON, a truncated
 * read just fails to parse, which is handled gracefully by parse_pwdump()). */
static void pwdump_read_cb(int fd, uint32_t revents, void *user_data)
{
    DC_UNUSED(revents);
    DC_UNUSED(user_data);
    for (;;) {
        size_t avail = (g_pwdump.cap > g_pwdump.len + 1) ? (g_pwdump.cap - g_pwdump.len - 1) : 0;
        if (avail == 0) {
            if (g_pwdump.cap >= DC_AUDIO_PWDUMP_CAP) {
                pwdump_finish(true); /* cap reached: parse whatever we have */
                return;
            }
            size_t ncap = g_pwdump.cap ? g_pwdump.cap * 2 : 8192;
            if (ncap > DC_AUDIO_PWDUMP_CAP)
                ncap = DC_AUDIO_PWDUMP_CAP;
            char *nb = realloc(g_pwdump.buf, ncap);
            if (!nb) {
                pwdump_finish(false);
                return;
            }
            g_pwdump.buf = nb;
            g_pwdump.cap = ncap;
            continue;
        }
        ssize_t n = read(fd, g_pwdump.buf + g_pwdump.len, avail);
        if (n > 0) {
            g_pwdump.len += (size_t)n;
            continue;
        }
        if (n == 0) {
            pwdump_finish(true);
            return;
        }
        return; /* EAGAIN: wait for the next POLLIN */
    }
}

static void audio_pwdump_start(void)
{
    if (g_pwdump.active || !g_pwdump.loop)
        return;

    int fds[2];
    if (pipe(fds) < 0)
        return;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }

    if (pid == 0) { /* child: pw-dump -> write end of the pipe */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        setsid();
        execlp("pw-dump", "pw-dump", (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    g_pwdump.fd = fds[0];
    g_pwdump.pid = pid;
    g_pwdump.len = 0;
    g_pwdump.cap = 0;
    free(g_pwdump.buf);
    g_pwdump.buf = NULL;
    g_pwdump.active = true;
    dc_loop_add_fd(g_pwdump.loop, fds[0], POLLIN, pwdump_read_cb, NULL);
    dc_debug("audio: forking pw-dump (async, device cache miss/stale)");
}

static void audio_devices_maybe_refresh(void)
{
    time_t now = time(NULL);
    bool stale = !g_devices_valid || now - g_devices_time >= DC_AUDIO_DEVICE_CACHE_SECONDS;
    if (stale)
        audio_pwdump_start(); /* no-op if already in flight or no loop bound yet */
}

/* Force the device cache stale and kick an immediate refresh -- used by every
 * write below (dc_audio_set_volume, dc_audio_device_set_volume/toggle_mute/
 * set_default) so the service self-invalidates its own cache instead of
 * relying on an external dirty flag (replaces the old settings.c
 * g_audio_dirty_until mechanism, docs plan D1/T1). */
static void audio_devices_invalidate(void)
{
    g_devices_time = 0;
    audio_pwdump_start();
}

void dc_audio_init(struct dc_loop *loop)
{
    g_pwdump.loop = loop;
}

/* --- legacy default-sink/source readers (kept for main.c's OSD tick and the
 * bar -- exact original signatures, now backed by the device cache above) -- */

bool dc_audio_read(dc_audio_info *out)
{
    audio_devices_maybe_refresh();

    out->available = false;
    out->volume = 0;
    out->muted = false;

    if (!g_devices_valid)
        return false; /* never populated yet: "no reading yet", matches prior behavior */

    for (int i = 0; i < g_devices.n_sinks; i++) {
        if (g_devices.sinks[i].is_default) {
            out->available = true;
            out->volume = g_devices.sinks[i].volume;
            out->muted = g_devices.sinks[i].muted;
            return true;
        }
    }
    return false; /* pw-dump unavailable, or no default sink known */
}

void dc_audio_set_volume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f", percent / 100.0f);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    audio_devices_invalidate();
}

bool dc_audio_read_source(dc_audio_info *out)
{
    audio_devices_maybe_refresh();

    out->available = false;
    out->volume = 0;
    out->muted = false;

    if (!g_devices_valid)
        return false;

    for (int i = 0; i < g_devices.n_sources; i++) {
        if (g_devices.sources[i].is_default) {
            out->available = true;
            out->volume = g_devices.sources[i].volume;
            out->muted = g_devices.sources[i].muted;
            return true;
        }
    }
    return false;
}

bool dc_audio_read_sink_name(char *out, size_t out_sz)
{
    audio_devices_maybe_refresh();

    if (g_devices_valid) {
        for (int i = 0; i < g_devices.n_sinks; i++) {
            if (g_devices.sinks[i].is_default) {
                const char *name =
                        g_devices.sinks[i].desc[0] ? g_devices.sinks[i].desc : g_devices.sinks[i].name;
                snprintf(out, out_sz, "%s", name);
                return true;
            }
        }
    }

    if (out_sz)
        out[0] = '\0';
    return false;
}

/* --- per-device API (docs/25-AUDIO-PERDEVICE-PLAN.md T1) ------------------ */

int dc_audio_sinks(dc_audio_device *out, int max)
{
    audio_devices_maybe_refresh();
    if (!g_devices_valid || max <= 0)
        return 0;
    int n = g_devices.n_sinks < max ? g_devices.n_sinks : max;
    if (out && n > 0)
        memcpy(out, g_devices.sinks, (size_t)n * sizeof(*out));
    return n;
}

int dc_audio_sources(dc_audio_device *out, int max)
{
    audio_devices_maybe_refresh();
    if (!g_devices_valid || max <= 0)
        return 0;
    int n = g_devices.n_sources < max ? g_devices.n_sources : max;
    if (out && n > 0)
        memcpy(out, g_devices.sources, (size_t)n * sizeof(*out));
    return n;
}

void dc_audio_device_set_volume(uint32_t id, int percent)
{
    if (percent < 0)
        percent = 0;
    /* TODO(T3): clamp to the per-device/default max-volume + alias lookup
     * (docs/25-AUDIO-PERDEVICE-PLAN.md D4) -- not implemented yet, this is a
     * plain non-negative clamp only. */

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "wpctl set-volume %u %d%%", id, percent);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    audio_devices_invalidate();
}

void dc_audio_device_toggle_mute(uint32_t id)
{
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "wpctl set-mute %u toggle", id);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    audio_devices_invalidate();
}

void dc_audio_set_default(uint32_t id)
{
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "wpctl set-default %u", id);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    audio_devices_invalidate();
}
