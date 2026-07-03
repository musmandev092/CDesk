/* nightlight.c — see nightlight.h for the public contract + design summary.
 *
 * Backend command shapes (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.4):
 *
 *   wlsunset (preferred, has a live SIGUSR1 day/night/auto toggle -- NOT
 *   installed on the dev machine this was built/verified on, so this path is
 *   implemented per the wlsunset(1) man page but only smoke-tested via `-h`;
 *   flag if it ever misbehaves once wlsunset is actually packaged):
 *     MANUAL (fixed temp):  wlsunset -T <t> -t <t> -S 00:00 -s 00:00
 *       (wlsunset always needs a day/night *period* source -- either -l/-L or
 *       -S/-s. Since day temp == night temp here the period boundary is moot,
 *       so a fixed midnight/midnight pair is used just to satisfy the
 *       argument requirement.)
 *     SUNSET (auto):        wlsunset -l <lat> -L <lon> -T 6500 -t <temp>
 *     TIMES (manual window): wlsunset -S <to> -s <from> -T 6500 -t <temp>
 *       (wlsunset's -S is "sunrise"/day-start, -s is "sunset"/night-start;
 *       our `from` is when night begins and `to` is when it ends, so
 *       sunset=from, sunrise=to.)
 *
 *   gammastep (fallback, what's actually installed + verified here -- see
 *   AGENTS.md worklog / commit messages for the live verification run).
 *   gammastep has no restart-free live toggle; every mode below is a
 *   long-lived resident process (its own `-O` "one-shot" mode does NOT exit
 *   after applying -- it stays resident holding the wl_output gamma object
 *   until killed, confirmed by direct testing) so all three modes are
 *   handled the same way: kill any previous child, fork+exec a new one.
 *     MANUAL (fixed temp):  gammastep -O <t> -m wayland
 *     SUNSET (auto):        gammastep -l <lat>:<lon> -t 6500:<temp> -m wayland
 *     TIMES (manual window): gammastep -c <generated ini> -m wayland
 *       (gammastep has no -S/-s CLI flags; the config-file `dawn-time`/
 *       `dusk-time` keys are the only way to force a fixed daily window, so
 *       TIMES mode writes a small ini under $XDG_STATE_HOME/dankc/ and points
 *       -c at it. dawn-time=<to> (night ends / day begins), dusk-time=<from>
 *       (night begins), location-provider=manual + lat/lon are still
 *       required by gammastep's config parser even though the explicit times
 *       are what actually drive the period, per the manpage.)
 */
#include "services/nightlight.h"

#include "core/config.h"
#include "core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DC_NIGHTLIGHT_TEMP_MIN 2500
#define DC_NIGHTLIGHT_TEMP_MAX 6500
#define DC_NIGHTLIGHT_TEMP_NEUTRAL 6500

static bool g_backend_probed = false;
static dc_nightlight_backend g_backend = DC_NIGHTLIGHT_BACKEND_NONE;
static pid_t g_child_pid = -1; /* the backend process WE forked, -1 if none */

/* --- backend probe ---------------------------------------------------------- */

static bool have_cmd(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

dc_nightlight_backend dc_nightlight_backend_get(void)
{
    if (g_backend_probed)
        return g_backend;
    g_backend_probed = true;
    if (have_cmd("wlsunset"))
        g_backend = DC_NIGHTLIGHT_BACKEND_WLSUNSET;
    else if (have_cmd("gammastep"))
        g_backend = DC_NIGHTLIGHT_BACKEND_GAMMASTEP;
    else {
        g_backend = DC_NIGHTLIGHT_BACKEND_NONE;
        dc_warn("nightlight: neither wlsunset nor gammastep found on PATH -- "
                "install one of them for Night Light to work");
    }
    return g_backend;
}

const char *dc_nightlight_backend_name(dc_nightlight_backend be)
{
    switch (be) {
    case DC_NIGHTLIGHT_BACKEND_WLSUNSET: return "wlsunset";
    case DC_NIGHTLIGHT_BACKEND_GAMMASTEP: return "gammastep";
    default: return "none";
    }
}

/* --- paths ------------------------------------------------------------------ */

/* $XDG_STATE_HOME/dankc/ (or ~/.local/state/dankc/), same convention as
 * services/history.c's history_path(). Used for the generated gammastep
 * TIMES-mode ini and the backend's redirected stdout/stderr log (handy for
 * verifying the exact args a running child was launched with, per this
 * task's own "verify via ps/log" escape hatch). */
static bool state_dir(char *out, size_t n)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%.470s/dankc", xdg);
    else if (home)
        snprintf(out, n, "%.470s/.local/state/dankc", home);
    else
        return false;
    return true;
}

static void ensure_state_dir(void)
{
    char dir[512];
    if (!state_dir(dir, sizeof(dir)))
        return;
    /* One level up may not exist either (~/.local/state); mirror config.c's
     * two-mkdir approach. */
    char parent[512];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        mkdir(parent, 0755);
    }
    mkdir(dir, 0755);
}

static bool ini_path(char *out, size_t n)
{
    char dir[480];
    if (!state_dir(dir, sizeof(dir)))
        return false;
    snprintf(out, n, "%s/nightlight-gammastep.ini", dir);
    return true;
}

static bool log_path(char *out, size_t n)
{
    char dir[480];
    if (!state_dir(dir, sizeof(dir)))
        return false;
    snprintf(out, n, "%s/nightlight.log", dir);
    return true;
}

/* --- process management ------------------------------------------------------ */

static void kill_child(void)
{
    if (g_child_pid > 0) {
        /* SIGTERM (not KILL): both wlsunset and gammastep catch it and reset
         * gamma ramps to neutral before exiting -- that's how they always
         * restore the display when toggled off. SIGCHLD is SIG_IGN
         * process-wide (main.c) so the kernel reaps it without us
         * waitpid()-ing. */
        kill(g_child_pid, SIGTERM);
        g_child_pid = -1;
    }
}

/* fork+exec argv[] detached, stdout/stderr appended to the state-dir log for
 * post-hoc verification, stdin from /dev/null. Returns the child pid (or -1
 * on fork failure -- caller treats that the same as "no child owned"). */
static pid_t spawn_argv(char *const argv[])
{
    char logf[512];
    bool have_log = log_path(logf, sizeof(logf));

    pid_t pid = fork();
    if (pid < 0) {
        dc_warn("nightlight: fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
            dup2(devnull, STDIN_FILENO);
        int logfd = have_log ? open(logf, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

/* --- argv builders ------------------------------------------------------------ */

#define DC_NL_ARGV_MAX 16

static void argv_push(char *argv[], int *n, char *arg)
{
    if (*n < DC_NL_ARGV_MAX - 1)
        argv[(*n)++] = arg;
}

/* Writes gammastep's TIMES-mode config.ini. Returns true on success. */
static bool write_gammastep_times_ini(const char *path, int night_temp, double lat, double lon,
                                      const char *from_hhmm, const char *to_hhmm)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        dc_warn("nightlight: could not write %s: %s", path, strerror(errno));
        return false;
    }
    fprintf(f,
           "[general]\n"
           "temp-day=%d\n"
           "temp-night=%d\n"
           "adjustment-method=wayland\n"
           "location-provider=manual\n"
           "dawn-time=%s\n"
           "dusk-time=%s\n"
           "\n"
           "[manual]\n"
           "lat=%.6f\n"
           "lon=%.6f\n",
           DC_NIGHTLIGHT_TEMP_NEUTRAL, night_temp, to_hhmm, from_hhmm, lat, lon);
    fclose(f);
    return true;
}

/* Builds and spawns the backend command for the current config. Assumes the
 * caller already killed any previous child. Leaves g_child_pid untouched on
 * failure (stays -1). */
static void launch_for_config(void)
{
    dc_nightlight_backend be = dc_nightlight_backend_get();
    if (be == DC_NIGHTLIGHT_BACKEND_NONE)
        return;

    const dc_config *cfg = dc_config_current;
    int temp = cfg->nightlight_temp;
    if (temp < DC_NIGHTLIGHT_TEMP_MIN)
        temp = DC_NIGHTLIGHT_TEMP_MIN;
    if (temp > DC_NIGHTLIGHT_TEMP_MAX)
        temp = DC_NIGHTLIGHT_TEMP_MAX;

    char *argv[DC_NL_ARGV_MAX] = {0};
    int n = 0;
    char temp_s[16], neutral_s[16], pair_s[40], loc_s[64];
    char ini[512];

    snprintf(temp_s, sizeof(temp_s), "%d", temp);
    snprintf(neutral_s, sizeof(neutral_s), "%d", DC_NIGHTLIGHT_TEMP_NEUTRAL);

    if (be == DC_NIGHTLIGHT_BACKEND_GAMMASTEP) {
        switch (cfg->nightlight_schedule_mode) {
        case DC_NIGHTLIGHT_SCHED_SUNSET:
            snprintf(loc_s, sizeof(loc_s), "%.6f:%.6f", cfg->weather_lat, cfg->weather_lon);
            snprintf(pair_s, sizeof(pair_s), "%d:%d", DC_NIGHTLIGHT_TEMP_NEUTRAL, temp);
            argv_push(argv, &n, "gammastep");
            argv_push(argv, &n, "-l"); argv_push(argv, &n, loc_s);
            argv_push(argv, &n, "-t"); argv_push(argv, &n, pair_s);
            argv_push(argv, &n, "-m"); argv_push(argv, &n, "wayland");
            break;
        case DC_NIGHTLIGHT_SCHED_TIMES:
            ensure_state_dir();
            if (!ini_path(ini, sizeof(ini)) ||
                !write_gammastep_times_ini(ini, temp, cfg->weather_lat, cfg->weather_lon,
                                          cfg->nightlight_from, cfg->nightlight_to)) {
                dc_warn("nightlight: falling back to manual temp (ini write failed)");
                argv_push(argv, &n, "gammastep");
                argv_push(argv, &n, "-O"); argv_push(argv, &n, temp_s);
                argv_push(argv, &n, "-m"); argv_push(argv, &n, "wayland");
                break;
            }
            argv_push(argv, &n, "gammastep");
            argv_push(argv, &n, "-c"); argv_push(argv, &n, ini);
            argv_push(argv, &n, "-m"); argv_push(argv, &n, "wayland");
            break;
        case DC_NIGHTLIGHT_SCHED_MANUAL:
        default:
            argv_push(argv, &n, "gammastep");
            argv_push(argv, &n, "-O"); argv_push(argv, &n, temp_s);
            argv_push(argv, &n, "-m"); argv_push(argv, &n, "wayland");
            break;
        }
    } else { /* DC_NIGHTLIGHT_BACKEND_WLSUNSET (documented, not verified locally) */
        switch (cfg->nightlight_schedule_mode) {
        case DC_NIGHTLIGHT_SCHED_SUNSET:
            snprintf(loc_s, sizeof(loc_s), "%.6f", cfg->weather_lat);
            argv_push(argv, &n, "wlsunset");
            argv_push(argv, &n, "-l"); argv_push(argv, &n, loc_s);
            {
                static char lon_s[32];
                snprintf(lon_s, sizeof(lon_s), "%.6f", cfg->weather_lon);
                argv_push(argv, &n, "-L"); argv_push(argv, &n, lon_s);
            }
            argv_push(argv, &n, "-T"); argv_push(argv, &n, neutral_s);
            argv_push(argv, &n, "-t"); argv_push(argv, &n, temp_s);
            break;
        case DC_NIGHTLIGHT_SCHED_TIMES:
            /* -S = sunrise (day starts / night ends) = our `to`.
             * -s = sunset (night starts) = our `from`. */
            argv_push(argv, &n, "wlsunset");
            argv_push(argv, &n, "-S");
            argv_push(argv, &n, cfg->nightlight_to[0] ? (char *)cfg->nightlight_to : "06:00");
            argv_push(argv, &n, "-s");
            argv_push(argv, &n, cfg->nightlight_from[0] ? (char *)cfg->nightlight_from : "18:00");
            argv_push(argv, &n, "-T"); argv_push(argv, &n, neutral_s);
            argv_push(argv, &n, "-t"); argv_push(argv, &n, temp_s);
            break;
        case DC_NIGHTLIGHT_SCHED_MANUAL:
        default:
            argv_push(argv, &n, "wlsunset");
            argv_push(argv, &n, "-T"); argv_push(argv, &n, temp_s);
            argv_push(argv, &n, "-t"); argv_push(argv, &n, temp_s);
            argv_push(argv, &n, "-S"); argv_push(argv, &n, "00:00");
            argv_push(argv, &n, "-s"); argv_push(argv, &n, "00:00");
            break;
        }
    }
    argv[n] = NULL;

    dc_info("nightlight: launching %s (schedule=%d temp=%dK)",
            dc_nightlight_backend_name(be), cfg->nightlight_schedule_mode, temp);
    g_child_pid = spawn_argv(argv);
}

/* Re-applies the current config.json-persisted state (used by init + every
 * mutator below). */
static void apply(void)
{
    kill_child();
    if (dc_config_current->nightlight_enabled)
        launch_for_config();
}

/* --- public API -------------------------------------------------------------- */

void dc_nightlight_init(void)
{
    /* Probe eagerly so a missing backend is logged once at startup rather
     * than silently on first toggle. */
    dc_nightlight_backend_get();
    apply();
}

void dc_nightlight_shutdown(void)
{
    kill_child();
}

bool dc_nightlight_active(void)
{
    return g_child_pid > 0;
}

int dc_nightlight_get_temp(void)
{
    return dc_config_current->nightlight_temp;
}

dc_nightlight_schedule dc_nightlight_get_schedule(void)
{
    return (dc_nightlight_schedule)dc_config_current->nightlight_schedule_mode;
}

void dc_nightlight_get_times(char *from, size_t from_n, char *to, size_t to_n)
{
    if (from && from_n)
        snprintf(from, from_n, "%s", dc_config_current->nightlight_from);
    if (to && to_n)
        snprintf(to, to_n, "%s", dc_config_current->nightlight_to);
}

void dc_nightlight_enable(bool on)
{
    dc_config *cfg = dc_config_mut();
    cfg->nightlight_enabled = on;
    dc_config_save();
    apply();
}

void dc_nightlight_toggle(void)
{
    dc_nightlight_enable(!dc_config_current->nightlight_enabled);
}

void dc_nightlight_set_temp(int kelvin)
{
    if (kelvin < DC_NIGHTLIGHT_TEMP_MIN)
        kelvin = DC_NIGHTLIGHT_TEMP_MIN;
    if (kelvin > DC_NIGHTLIGHT_TEMP_MAX)
        kelvin = DC_NIGHTLIGHT_TEMP_MAX;
    dc_config *cfg = dc_config_mut();
    cfg->nightlight_temp = kelvin;
    dc_config_save();
    apply(); /* no-op relaunch if not currently enabled */
}

void dc_nightlight_set_schedule(dc_nightlight_schedule mode, const char *from_hhmm,
                                const char *to_hhmm)
{
    dc_config *cfg = dc_config_mut();
    cfg->nightlight_schedule_mode = (int)mode;
    if (mode == DC_NIGHTLIGHT_SCHED_TIMES) {
        if (from_hhmm)
            snprintf(cfg->nightlight_from, sizeof(cfg->nightlight_from), "%s", from_hhmm);
        if (to_hhmm)
            snprintf(cfg->nightlight_to, sizeof(cfg->nightlight_to), "%s", to_hhmm);
    }
    dc_config_save();
    apply();
}
