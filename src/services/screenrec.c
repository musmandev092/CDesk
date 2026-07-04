/* screenrec.c — see screenrec.h for the public contract + process-model
 * rationale (fork+exec, exec-into-recorder for the region path, kill(pid,0)
 * liveness polling since SIGCHLD is SIG_IGN process-wide).
 */
#include "services/screenrec.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "services/notifications.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Module-static single-recording state (dankc only ever drives one recording
 * at a time -- see screenrec.h). */
static struct {
    bool recording;
    uint64_t started_ms;
    char out_path[512];
    pid_t pid;
} g_st;

/* --- PATH probing (mirrors ui/dashboard.c's cmd_exists(); duplicated rather
 * than shared since that one is file-static there -- both are the same
 * "$PATH lookup via access(), no shell" primitive, safe to call anytime,
 * unlike nightlight.c's system()-based have_cmd() which must run before
 * main.c sets SIGCHLD to SIG_IGN). --------------------------------------- */

static bool cmd_exists(const char *name)
{
    const char *path = getenv("PATH");
    if (!path)
        return false;
    while (*path) {
        const char *colon = strchr(path, ':');
        size_t len = colon ? (size_t)(colon - path) : strlen(path);
        if (len > 0 && len < 400) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%.*s/%.100s", (int)len, path, name);
            if (access(buf, X_OK) == 0)
                return true;
        }
        path += len;
        if (*path == ':')
            path++;
    }
    return false;
}

/* wl-screenrec preferred (lower overhead, GPU-side encode); wf-recorder
 * fallback. Honors the `screenRecorderCmd` config override (docs/29 sec.5):
 * if the user pinned a recorder and it's on PATH, use it verbatim; otherwise
 * auto-probe. */
static const char *probe_recorder(void)
{
    const char *override = dc_config_current ? dc_config_current->screen_recorder_cmd : NULL;
    if (override && override[0] && cmd_exists(override))
        return override;
    if (cmd_exists("wl-screenrec"))
        return "wl-screenrec";
    if (cmd_exists("wf-recorder"))
        return "wf-recorder";
    return NULL;
}

/* --- output path -------------------------------------------------------- */

/* Recursive `mkdir -p`, in-place on a mutable copy of `path`. */
static bool mkdir_p(const char *path)
{
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0)
        return false;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

/* Builds ${XDG_VIDEOS_DIR:-$HOME/Videos}/recording-YYYYmmdd-HHMMSS.mp4,
 * creating the directory if needed. */
static bool build_out_path(char *out, size_t outsz)
{
    char dir[400];
    const char *videos = getenv("XDG_VIDEOS_DIR");
    if (videos && *videos) {
        snprintf(dir, sizeof(dir), "%s", videos);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return false;
        snprintf(dir, sizeof(dir), "%s/Videos", home);
    }
    if (!mkdir_p(dir)) {
        dc_warn("screenrec: mkdir -p %s failed: %s", dir, strerror(errno));
        return false;
    }

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    snprintf(out, outsz, "%s/recording-%s.mp4", dir, stamp);
    return true;
}

/* --- fork+exec ----------------------------------------------------------- */

/* Detach stdio in the child: stdin from /dev/null (recorders shouldn't read a
 * terminal), stdout/stderr to /dev/null (nothing owns a tty to show them). */
static void detach_stdio(void)
{
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
    }
}

/* --- public API ------------------------------------------------------------ */

bool dc_screenrec_start(struct dc_notifications *n, bool region)
{
    if (g_st.recording) {
        dc_warn("screenrec: start requested while already recording -- ignored");
        return false;
    }

    const char *recorder = probe_recorder();
    if (!recorder) {
        dc_warn("screenrec: neither wl-screenrec nor wf-recorder found on PATH");
        if (n)
            dc_notifications_post_local(n, "dankc", "Screen recording",
                                        "wl-screenrec/wf-recorder not found -- install one to enable "
                                        "screen recording.",
                                        DC_URGENCY_NORMAL);
        return false;
    }

    char path[512];
    if (!build_out_path(path, sizeof(path))) {
        dc_warn("screenrec: could not determine/create the output directory");
        if (n)
            dc_notifications_post_local(n, "dankc", "Screen recording",
                                        "Could not create the recordings directory.", DC_URGENCY_NORMAL);
        return false;
    }

    /* Optional system-audio capture: both wl-screenrec and wf-recorder take
     * `--audio` (docs/29 sec.5, screenRecorderAudio config toggle). */
    const bool audio = dc_config_current && dc_config_current->screen_recorder_audio;
    const char *audio_flag = audio ? "--audio" : NULL;

    pid_t pid;
    if (region) {
        /* slurp must resolve a geometry before the recorder can start; exec
         * into the recorder once it does, so the tracked pid becomes the
         * recorder's (see screenrec.h's "Region recording" note). If slurp is
         * cancelled, `&&` short-circuits and this whole child exits quickly
         * with no output file -- dc_screenrec_active() treats that as a
         * clean cancel. */
        char shcmd[900];
        snprintf(shcmd, sizeof(shcmd), "g=$(slurp) && exec %s %s-g \"$g\" -f \"%s\"",
                 recorder, audio ? "--audio " : "", path);
        pid = fork();
        if (pid == 0) {
            detach_stdio();
            execl("/bin/sh", "sh", "-c", shcmd, (char *)NULL);
            _exit(127);
        }
    } else {
        pid = fork();
        if (pid == 0) {
            detach_stdio();
            if (audio_flag)
                execlp(recorder, recorder, audio_flag, "-f", path, (char *)NULL);
            else
                execlp(recorder, recorder, "-f", path, (char *)NULL);
            _exit(127);
        }
    }

    if (pid < 0) {
        dc_warn("screenrec: fork failed: %s", strerror(errno));
        if (n)
            dc_notifications_post_local(n, "dankc", "Screen recording",
                                        "Failed to start the recorder process.", DC_URGENCY_NORMAL);
        return false;
    }

    g_st.recording = true;
    g_st.pid = pid;
    g_st.started_ms = (uint64_t)dc_anim_now_ms();
    snprintf(g_st.out_path, sizeof(g_st.out_path), "%s", path);
    dc_info("screenrec: recording started (%s, %s) -> %s", recorder, region ? "region" : "fullscreen",
            path);
    return true;
}

void dc_screenrec_stop(struct dc_notifications *n)
{
    if (!g_st.recording)
        return;

    /* SIGINT (never SIGKILL): both wl-screenrec and wf-recorder trap it to
     * finalize/mux the mp4 container cleanly before exiting. A SIGKILL'd
     * recorder leaves a truncated, often unplayable file.
     *
     * Signal the whole process group (-pid), not just the tracked pid: the
     * forked child calls setsid() (see detach_stdio()), making it the
     * leader of its own new process group, so -pid reaches every member of
     * it. This matters for a region recording stopped *before* slurp
     * resolves -- the tracked pid is still the wrapper shell at that point,
     * and slurp is a grandchild (shell -> `$(slurp)` subshell -> slurp) in
     * the same group; signaling only the shell's pid left slurp running
     * forever, unkillable and invisible once dankc had already cleared its
     * state (confirmed by direct testing before this fix). Once the shell
     * has exec'd into the recorder, -pid == pid for signaling purposes
     * since it's the sole member. */
    kill(-g_st.pid, SIGINT);
    dc_info("screenrec: stop requested (pid %d) -> %s", (int)g_st.pid, g_st.out_path);

    if (n) {
        char body[560];
        snprintf(body, sizeof(body), "Saved %s", g_st.out_path);
        dc_notifications_post_local(n, "dankc", "Screen recording", body, DC_URGENCY_NORMAL);
    }

    memset(&g_st, 0, sizeof(g_st));
}

bool dc_screenrec_active(void)
{
    if (!g_st.recording)
        return false;

    if (kill(g_st.pid, 0) == 0)
        return true; /* still alive */

    /* Child died on its own -- either it crashed, or (region mode) slurp was
     * cancelled before the `exec` ever replaced it with the recorder. A
     * death within 2s with no output file yet is the clean-cancel case
     * (screenrec.h); anything else is logged so a genuinely crashed recorder
     * isn't silently swallowed. */
    uint64_t now = (uint64_t)dc_anim_now_ms();
    uint64_t elapsed = now > g_st.started_ms ? now - g_st.started_ms : 0;
    struct stat st;
    bool has_file = stat(g_st.out_path, &st) == 0 && st.st_size > 0;
    if (elapsed < 2000 && !has_file)
        dc_info("screenrec: recording cancelled");
    else
        dc_warn("screenrec: recorder process (pid %d) exited unexpectedly", (int)g_st.pid);

    memset(&g_st, 0, sizeof(g_st));
    return false;
}

int dc_screenrec_elapsed_sec(void)
{
    if (!g_st.recording)
        return 0;
    uint64_t now = (uint64_t)dc_anim_now_ms();
    uint64_t elapsed = now > g_st.started_ms ? now - g_st.started_ms : 0;
    return (int)(elapsed / 1000);
}
