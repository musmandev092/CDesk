#include "services/audio.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* wpctl forks a process, so cache it (docs/POLISH.md P7 item 2). The bar's
 * damage-tracking hash (ui/bar/bar.c bar_compute_signature()) now reads this
 * every ~1Hz tick per bar just to check for a volume change, on top of the
 * 1Hz clock_tick's own OSD-change read and the control-center-pill's draw
 * read — so without a real cache window this would fork on nearly every tick
 * across 2+ bars. Widened to 10s per docs/15-PERF-PLAN.md T2.3 (was 3s).
 * TRADEOFF: dc_audio_set_volume() below still forces the cache stale
 * immediately (see below), so a user's own slider drag/OSD reflects near-
 * instantly. The longer window only affects external volume changes (media
 * keys, another app) — detection latency is now up to ~10s instead of ~1s,
 * but saves ~13 forks/min (~20 → ~7). External volume changes on the bar are
 * acceptable per docs/POLISH.md P7 ("2-3s worst-case is fine"). */
#define DC_AUDIO_CACHE_SECONDS 10

static dc_audio_info g_cache;
static bool g_cache_ok = false;
static time_t g_cache_time = 0;
static bool g_cache_valid = false;

/* --- Async wpctl fetch (docs/16-PERF2-PLAN.md T1.2) ------------------------
 *
 * `wpctl get-volume` takes ~35-40ms (fork+exec+dynamic link+PipeWire round
 * trip) — a synchronous popen()/pclose() used to block the single-threaded
 * event loop for that whole window, once at first render and again every
 * cache refresh for the life of the process. This mirrors services/net.c's
 * wifi scan / services/clipboard.c's transfer_read shape instead: fork+exec
 * with stdout piped, the read end registered non-blocking on the event loop,
 * and the result parsed once EOF is seen. Reaping relies on the process-wide
 * `signal(SIGCHLD, SIG_IGN)` main.c installs before entering the loop (same
 * convention net.c's fire-and-forget forks already rely on) — no waitpid
 * here. */
static struct {
    struct dc_loop *loop;
    bool active;
    pid_t pid;
    int fd;
    char buf[128];
    size_t len;
} g_fetch = {.fd = -1};

static void audio_fetch_finish(bool eof_reached)
{
    dc_loop_remove_fd(g_fetch.loop, g_fetch.fd);
    close(g_fetch.fd);
    g_fetch.fd = -1;
    g_fetch.active = false;

    dc_audio_info result = {0};
    bool ok = false;
    if (eof_reached && g_fetch.len > 0) {
        g_fetch.buf[g_fetch.len] = '\0';
        float volume = 0.0f;
        if (sscanf(g_fetch.buf, "Volume: %f", &volume) == 1) {
            result.volume = (int)(volume * 100.0f + 0.5f);
            result.available = true;
            ok = true;
        }
        if (strstr(g_fetch.buf, "MUTED"))
            result.muted = true;
    }
    g_fetch.len = 0;

    if (ok) {
        g_cache = result;
        g_cache_ok = true;
    } else {
        g_cache_ok = false;
    }
    g_cache_time = time(NULL);
    g_cache_valid = true;
}

/* Pipe is readable: accumulate until EOF, then parse (same drain shape as
 * services/net.c's scan_drain() / services/clipboard.c's transfer_read()). */
static void audio_fetch_read_cb(int fd, uint32_t revents, void *user_data)
{
    DC_UNUSED(revents);
    DC_UNUSED(user_data);
    for (;;) {
        if (g_fetch.len + 1 >= sizeof(g_fetch.buf)) {
            audio_fetch_finish(true); /* enough for "Volume: 0.45 [MUTED]" */
            return;
        }
        ssize_t n = read(fd, g_fetch.buf + g_fetch.len, sizeof(g_fetch.buf) - g_fetch.len - 1);
        if (n > 0) {
            g_fetch.len += (size_t)n;
            continue;
        }
        if (n == 0) {
            audio_fetch_finish(true);
            return;
        }
        return; /* EAGAIN: wait for the next POLLIN */
    }
}

static void audio_fetch_start(void)
{
    if (g_fetch.active || !g_fetch.loop)
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

    if (pid == 0) { /* child: wpctl get-volume -> write end of the pipe */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        setsid();
        execlp("wpctl", "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    g_fetch.fd = fds[0];
    g_fetch.pid = pid;
    g_fetch.len = 0;
    g_fetch.active = true;
    dc_loop_add_fd(g_fetch.loop, fds[0], POLLIN, audio_fetch_read_cb, NULL);
    dc_debug("audio: forking wpctl get-volume (async, cache miss/stale)");
}

void dc_audio_init(struct dc_loop *loop)
{
    g_fetch.loop = loop;
}

bool dc_audio_read(dc_audio_info *out)
{
    time_t now = time(NULL);
    bool stale = !g_cache_valid || now - g_cache_time >= DC_AUDIO_CACHE_SECONDS;
    if (stale)
        audio_fetch_start(); /* no-op if already in flight or no loop bound yet */

    if (!g_cache_valid) {
        /* Never populated yet (first call, or before dc_audio_init()): report
         * "no reading yet" instead of blocking — matches dc_weather_get()'s
         * documented pattern (read cached, refresh out-of-band). */
        out->available = false;
        out->volume = 0;
        out->muted = false;
        return false;
    }

    *out = g_cache;
    return g_cache_ok;
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

    /* Force the cache stale (instead of wiping it outright) so the *next*
     * dc_audio_read() still shows the last-known value while a fresh async
     * fetch is in flight — no "no reading" flash — and kick that fetch right
     * now rather than waiting for the next tick to notice the staleness. */
    g_cache_time = 0;
    audio_fetch_start();
}

/* --- default source (mic) mute + default sink device name --------------
 *
 * Added for the mic-mute and audio-output-switch OSD variants (main.c's
 * clock_tick polls these every tick and diffs, exactly like the sink volume/
 * mute block above). Both are plain synchronous popen() calls, each behind
 * their own DC_AUDIO_SOURCE_CACHE_SECONDS/DC_AUDIO_SINK_NAME_CACHE_SECONDS
 * window rather than the async fork+pipe machinery above: that machinery
 * exists because the sink read used to run every ~1s before the T1.2
 * optimization widened it to 10s; these two are never polled more often than
 * once every 10s to begin with, so a single blocking popen() call that rare
 * is the same tradeoff ui/controlcenter.c's/ui/settings.c's own private
 * audio_source_read()/read_audio_device_names() copies already make (just
 * with a longer cache window here, since main.c's poll runs continuously in
 * the background rather than only while a popout is open). */
#define DC_AUDIO_SOURCE_CACHE_SECONDS 10
#define DC_AUDIO_SINK_NAME_CACHE_SECONDS 10

bool dc_audio_read_source(dc_audio_info *out)
{
    static dc_audio_info cache;
    static bool cache_ok;
    static time_t cache_time;

    time_t now = time(NULL);
    if (cache_time != 0 && now - cache_time < DC_AUDIO_SOURCE_CACHE_SECONDS) {
        *out = cache;
        return cache_ok;
    }
    cache_time = now;

    out->available = false;
    out->volume = 0;
    out->muted = false;

    FILE *pipe = popen("wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null", "r");
    bool ok = false;
    if (pipe) {
        char line[128];
        if (fgets(line, sizeof(line), pipe)) {
            float volume = 0.0f;
            if (sscanf(line, "Volume: %f", &volume) == 1) {
                out->volume = (int)(volume * 100.0f + 0.5f);
                out->available = true;
                ok = true;
            }
            if (strstr(line, "MUTED"))
                out->muted = true;
        }
        pclose(pipe);
    }

    cache = *out;
    cache_ok = ok;
    return ok;
}

bool dc_audio_read_sink_name(char *out, size_t out_sz)
{
    static char cache[64];
    static bool cache_ok;
    static time_t cache_time;

    time_t now = time(NULL);
    if (cache_time == 0 || now - cache_time >= DC_AUDIO_SINK_NAME_CACHE_SECONDS) {
        cache_time = now;
        cache[0] = '\0';
        cache_ok = false;

        /* `wpctl inspect` prints a property line like
         * `    node.description = "Built-in Audio Analog Stereo"`; we grep by
         * key, not by any leading default-marker, so this doesn't care
         * whether inspect prefixes it with one. */
        FILE *pipe = popen("wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
        if (pipe) {
            char line[256];
            while (fgets(line, sizeof(line), pipe)) {
                char *key = strstr(line, "node.description");
                if (!key)
                    continue;
                char *q1 = strchr(key, '"');
                char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                if (!q1 || !q2)
                    continue;
                size_t len = (size_t)(q2 - (q1 + 1));
                if (len >= sizeof(cache))
                    len = sizeof(cache) - 1;
                memcpy(cache, q1 + 1, len);
                cache[len] = '\0';
                cache_ok = true;
                break;
            }
            pclose(pipe);
        }
    }

    snprintf(out, out_sz, "%s", cache);
    return cache_ok;
}
