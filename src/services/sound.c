#include "services/sound.h"

#include "core/config.h"
#include "core/log.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- player binary discovery ----------------------------------------------
 *
 * Preference order matches docs/14-COMPLETION-PLAN.md W1.3: pw-play (native
 * PipeWire, what this system actually runs) first, paplay (works against
 * PipeWire's pulse-compat socket too) as the fallback. Resolved once and
 * cached -- a missing binary means every dc_sound_notify() call after the
 * first is a single `access()` per PATH entry, not a fork. */
static bool g_player_probed = false;
/* Sized to match find_in_path()'s local `candidate` scratch buffer exactly,
 * so the compiler can prove the final snprintf() copy can't truncate. */
static char g_player_path[600] = "";

static bool find_in_path(const char *name, char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *dir = strtok(buf, ":"); dir; dir = strtok(NULL, ":")) {
        char candidate[600];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (access(candidate, X_OK) == 0) {
            snprintf(out, out_sz, "%s", candidate);
            return true;
        }
    }
    return false;
}

static const char *resolve_player(void)
{
    if (g_player_probed)
        return g_player_path[0] ? g_player_path : NULL;
    g_player_probed = true;
    if (find_in_path("pw-play", g_player_path, sizeof(g_player_path)))
        return g_player_path;
    if (find_in_path("paplay", g_player_path, sizeof(g_player_path)))
        return g_player_path;
    dc_warn("sound: neither pw-play nor paplay found in PATH; notification sounds disabled");
    return NULL;
}

static bool player_is_paplay(const char *player)
{
    const char *slash = strrchr(player, '/');
    const char *base = slash ? slash + 1 : player;
    return strcmp(base, "paplay") == 0;
}

/* --- sound-theme file resolution -------------------------------------------
 *
 * Same theme-dir search shape as services/icons.c's find_icon_file(): user
 * dir before system dir, and (here) a vendored last-resort copy so
 * notification sounds work out of the box even when the
 * sound-theme-freedesktop package isn't installed (not the case on every
 * distro -- see assets/sounds/freedesktop/CREDITS for provenance). */
static bool try_path(char *out, size_t out_sz, const char *fmt, ...)
{
    char path[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(path, sizeof(path), fmt, ap);
    va_end(ap);
    if (access(path, R_OK) == 0) {
        snprintf(out, out_sz, "%s", path);
        return true;
    }
    return false;
}

static bool resolve_sound_file(const char *event, char *out, size_t out_sz)
{
    const char *home = getenv("HOME");
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    char user_sounds[512] = "";
    if (xdg_data_home && *xdg_data_home)
        snprintf(user_sounds, sizeof(user_sounds), "%s/sounds", xdg_data_home);
    else if (home)
        snprintf(user_sounds, sizeof(user_sounds), "%s/.local/share/sounds", home);

    static const char *const exts[] = {"oga", "ogg", "wav"};

    /* 1. User + system freedesktop sound theme (docs 14 W1.3: resolve from
     * /usr/share/sounds/freedesktop/stereo, event.oga preferred). */
    const char *bases[2];
    int nbases = 0;
    if (user_sounds[0])
        bases[nbases++] = user_sounds;
    bases[nbases++] = "/usr/share/sounds";
    for (int b = 0; b < nbases; b++) {
        for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
            if (try_path(out, out_sz, "%s/freedesktop/stereo/%s.%s", bases[b], event, exts[e]))
                return true;
        }
    }

    /* 2. Vendored fallback (dev-tree relative path, then installed path --
     * same two-step pattern as render/nvg.c's FONT_CANDIDATES). The relative
     * path is probed against the cwd AND the executable's parent directory
     * (bin/dankc layout), so launching from outside the repo root still
     * finds the vendored copies (mirrors nvg.c's probe_font_path()). */
    static char exe_dir[512];
    static bool exe_dir_init = false;
    if (!exe_dir_init) {
        exe_dir_init = true;
        ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
        if (n > 0) {
            exe_dir[n] = '\0';
            char *slash = strrchr(exe_dir, '/');
            if (slash)
                *slash = '\0';
            else
                exe_dir[0] = '\0';
        } else {
            exe_dir[0] = '\0';
        }
    }
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        if (try_path(out, out_sz, "assets/sounds/freedesktop/%s.%s", event, exts[e]))
            return true;
        if (exe_dir[0] &&
            try_path(out, out_sz, "%s/../assets/sounds/freedesktop/%s.%s", exe_dir, event, exts[e]))
            return true;
    }
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        if (try_path(out, out_sz, "/usr/share/dankc/sounds/freedesktop/%s.%s", event, exts[e]))
            return true;
    }
    return false;
}

/* Resolved once per event kind and cached -- a burst of notifications must
 * not re-walk the filesystem per call. path[0] == '\0' after a failed
 * resolution attempt means "known unresolvable", so we warn exactly once. */
typedef struct {
    bool tried;
    bool warned;
    /* Sized to match try_path()'s local `path` scratch buffer exactly, same
     * reasoning as g_player_path above. */
    char path[1024];
} resolved_sound;

static resolved_sound g_normal, g_critical;

static const char *sound_path_for(dc_urgency urgency)
{
    resolved_sound *slot = (urgency == DC_URGENCY_CRITICAL) ? &g_critical : &g_normal;
    const char *event = (urgency == DC_URGENCY_CRITICAL) ? "message-new-instant" : "message";
    if (!slot->tried) {
        slot->tried = true;
        if (!resolve_sound_file(event, slot->path, sizeof(slot->path)))
            slot->path[0] = '\0';
    }
    if (!slot->path[0]) {
        if (!slot->warned) {
            slot->warned = true;
            dc_warn("sound: no sound-theme file found for event \"%s\"; notification sounds "
                    "muted for this urgency", event);
        }
        return NULL;
    }
    return slot->path;
}

/* --- playback --------------------------------------------------------------
 *
 * Fire-and-forget fork+exec, same shape as services/audio.c's
 * dc_audio_set_volume()/power.c's run_detached() -- reaped by main.c's
 * process-wide SIGCHLD = SIG_IGN, no waitpid() here. */
static void spawn_player(const char *player, const char *path, float volume)
{
    char volbuf[32];
    pid_t pid = fork();
    if (pid != 0)
        return; /* parent: fire-and-forget */

    setsid();
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
    }

    if (player_is_paplay(player)) {
        snprintf(volbuf, sizeof(volbuf), "%d", (int)(volume * 65536.0f));
        execl(player, player, "--volume", volbuf, path, (char *)NULL);
    } else {
        snprintf(volbuf, sizeof(volbuf), "%.3f", (double)volume);
        execl(player, player, "--volume", volbuf, path, (char *)NULL);
    }
    _exit(127);
}

void dc_sound_notify(dc_urgency urgency)
{
    const dc_config *cfg = dc_config_current;
    if (!cfg->sounds_enabled || !cfg->notif_sound_enabled || cfg->dnd_enabled)
        return;

    const char *player = resolve_player();
    if (!player)
        return;

    const char *path = sound_path_for(urgency);
    if (!path)
        return;

    /* Burst debounce (docs/14 W1.3 acceptance: 5 rapid notify-sends play at
     * most 2 sounds): a global ~300ms cooldown regardless of urgency mix. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    static int64_t last_played_ms = 0;
    if (now_ms - last_played_ms < 300)
        return;
    last_played_ms = now_ms;

    dc_info("sound: playing %s via %s", path, player);
    spawn_player(player, path, cfg->sound_volume);
}
