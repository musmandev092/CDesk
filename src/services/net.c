#include "services/net.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"

/* SSID/signal via `nmcli` (a fork+popen, same shell-out style already used by
 * services/audio.c for wpctl and controlcenter.c for rfkill/wpctl/
 * brightnessctl) -- cached briefly since `nmcli dev wifi list` is a slower
 * call than a sysfs read and dc_net_wifi() can be polled every render frame
 * during a popout's entrance animation. */
#define DC_NET_WIFI_CACHE_SECONDS 3

static void refresh_wifi_details(char *ssid, size_t ssid_sz, int *signal_percent)
{
    static char cached_ssid[64];
    static int cached_signal = -1;
    static time_t cache_time;

    time_t now = time(NULL);
    if (now - cache_time >= DC_NET_WIFI_CACHE_SECONDS) {
        cache_time = now;
        cached_ssid[0] = '\0';
        cached_signal = -1;

        FILE *pipe = popen("nmcli -t -f active,ssid,signal dev wifi list --rescan no 2>/dev/null", "r");
        if (pipe) {
            char line[256];
            while (fgets(line, sizeof(line), pipe)) {
                if (strncmp(line, "yes:", 4) != 0)
                    continue;
                char *rest = line + 4;
                char *last_colon = strrchr(rest, ':');
                if (!last_colon)
                    continue;
                *last_colon = '\0';
                cached_signal = atoi(last_colon + 1);
                /* un-escape nmcli's backslash-escaped ':' within the SSID */
                size_t j = 0;
                for (size_t i = 0; rest[i] != '\0' && j < sizeof(cached_ssid) - 1; i++) {
                    if (rest[i] == '\\' && rest[i + 1] == ':')
                        continue;
                    cached_ssid[j++] = rest[i];
                }
                cached_ssid[j] = '\0';
                break;
            }
            pclose(pipe);
        }
    }

    snprintf(ssid, ssid_sz, "%s", cached_ssid);
    *signal_percent = cached_signal;
}

bool dc_net_wifi(dc_net_info *out)
{
    out->has_wifi = false;
    out->connected = false;
    out->ssid[0] = '\0';
    out->signal_percent = -1;

    DIR *dir = opendir("/sys/class/net");
    if (!dir)
        return false;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        /* Wi-Fi interfaces are named wlan0, wlp*, etc. */
        if (strncmp(ent->d_name, "wl", 2) != 0)
            continue;
        out->has_wifi = true;

        char path[300];
        snprintf(path, sizeof(path), "/sys/class/net/%.200s/operstate", ent->d_name);
        FILE *file = fopen(path, "r");
        if (!file)
            continue;
        char state[32];
        if (fgets(state, sizeof(state), file)) {
            state[strcspn(state, "\r\n")] = '\0';
            if (strcmp(state, "up") == 0)
                out->connected = true;
        }
        fclose(file);
    }
    closedir(dir);

    if (out->connected)
        refresh_wifi_details(out->ssid, sizeof(out->ssid), &out->signal_percent);

    return out->has_wifi;
}

/* --- Wi-Fi scan list (async, docs/13-POPOUTS-SPEC.md sec.1 network section) -
 * Same fork+pipe+non-blocking-drain shape as services/weather.c's fetch, just
 * shelling out to nmcli instead of curl. A single child runs both the wifi
 * list and the saved-connection-name list (separated by a marker line) so
 * expanding the network section only ever has one process in flight. */
#define DC_NET_SCAN_REFRESH_SEC 8
#define DC_NET_SCAN_RETRY_SEC 5
#define DC_NET_SCAN_TIMEOUT_SEC 8
#define DC_NET_SCAN_BUF_CAP 8192
#define DC_NET_SCAN_MARKER "---dankc-known---"

static struct {
    dc_net_wifi_ap aps[DC_NET_SCAN_MAX];
    int count;
    bool have_cache;

    bool fetch_active;
    pid_t pid;
    int fd;
    char buf[DC_NET_SCAN_BUF_CAP];
    size_t len;
    struct timespec fetch_started;
    struct timespec next_attempt;
    bool next_attempt_armed;
} g_scan;

static long scan_secs_since(const struct timespec *from)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec - from->tv_sec;
}

static void scan_arm_next(int seconds_from_now)
{
    clock_gettime(CLOCK_MONOTONIC, &g_scan.next_attempt);
    g_scan.next_attempt.tv_sec += seconds_from_now;
    g_scan.next_attempt_armed = true;
}

static bool scan_attempt_due(void)
{
    if (!g_scan.next_attempt_armed)
        return true;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec != g_scan.next_attempt.tv_sec)
        return now.tv_sec > g_scan.next_attempt.tv_sec;
    return now.tv_nsec >= g_scan.next_attempt.tv_nsec;
}

/* Split an nmcli -t line on unescaped ':' in place, unescaping "\:" -> ':'
 * within each field (mirrors refresh_wifi_details()'s SSID unescaping, but
 * generalized to every field since SECURITY/IN-USE never contain a raw ':'
 * in practice while SSID sometimes does). Returns the field count (capped at
 * `max`; any extra ':'-separated content is folded into the last field). */
static int split_nmcli_fields(char *line, char *fields[], int max)
{
    int n = 0;
    char *out = line;
    fields[0] = out;
    for (char *p = line; *p; p++) {
        if (*p == '\\' && *(p + 1) == ':') {
            *out++ = ':';
            p++;
            continue;
        }
        if (*p == ':' && n + 1 < max) {
            *out++ = '\0';
            n++;
            fields[n] = out;
            continue;
        }
        *out++ = *p;
    }
    *out = '\0';
    return n + 1;
}

static void parse_scan_response(void)
{
    g_scan.count = 0;

    char known[32][64];
    int known_n = 0;
    bool in_known = false;

    char *saveptr = NULL;
    char *line = strtok_r(g_scan.buf, "\n", &saveptr);
    while (line) {
        line[strcspn(line, "\r")] = '\0';
        if (strcmp(line, DC_NET_SCAN_MARKER) == 0) {
            in_known = true;
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        if (in_known) {
            if (line[0] && known_n < (int)(sizeof(known) / sizeof(known[0]))) {
                snprintf(known[known_n], sizeof(known[known_n]), "%s", line);
                known_n++;
            }
        } else if (g_scan.count < DC_NET_SCAN_MAX) {
            char *fields[4];
            int n = split_nmcli_fields(line, fields, 4);
            if (n >= 3 && fields[0][0] != '\0') {
                dc_net_wifi_ap *ap = &g_scan.aps[g_scan.count];
                memset(ap, 0, sizeof(*ap));
                snprintf(ap->ssid, sizeof(ap->ssid), "%s", fields[0]);
                ap->signal_percent = atoi(fields[1]);
                ap->secured = fields[2][0] != '\0' && strcmp(fields[2], "--") != 0;
                ap->in_use = n >= 4 && fields[3][0] == '*';
                g_scan.count++;
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* Cross-reference against saved connection names -- a common-case
     * heuristic (auto-created Wi-Fi connections are named after their SSID),
     * not a perfect "is this SSID's password already saved" check. */
    for (int i = 0; i < g_scan.count; i++) {
        for (int k = 0; k < known_n; k++) {
            if (strcmp(g_scan.aps[i].ssid, known[k]) == 0) {
                g_scan.aps[i].known = true;
                break;
            }
        }
    }

    g_scan.have_cache = true;
}

static void scan_finish(bool eof_reached)
{
    close(g_scan.fd);
    g_scan.fd = -1;
    g_scan.fetch_active = false;

    if (eof_reached && g_scan.len > 0) {
        g_scan.buf[g_scan.len] = '\0';
        parse_scan_response();
    } else if (!eof_reached) {
        dc_warn("net: wifi scan timed out, retrying in %ds", DC_NET_SCAN_RETRY_SEC);
    }

    g_scan.len = 0;
    scan_arm_next(eof_reached ? DC_NET_SCAN_REFRESH_SEC : DC_NET_SCAN_RETRY_SEC);
}

static void scan_abort(void)
{
    if (g_scan.pid > 0)
        kill(g_scan.pid, SIGKILL);
    close(g_scan.fd);
    g_scan.fd = -1;
    g_scan.len = 0;
    g_scan.fetch_active = false;
    scan_arm_next(DC_NET_SCAN_RETRY_SEC);
}

static void scan_drain(void)
{
    if (scan_secs_since(&g_scan.fetch_started) > DC_NET_SCAN_TIMEOUT_SEC) {
        scan_abort();
        return;
    }

    struct pollfd pfd = {.fd = g_scan.fd, .events = POLLIN};
    if (poll(&pfd, 1, 0) <= 0)
        return;

    for (;;) {
        if (g_scan.len + 1 >= sizeof(g_scan.buf)) {
            scan_finish(false);
            return;
        }
        ssize_t n = read(g_scan.fd, g_scan.buf + g_scan.len, sizeof(g_scan.buf) - g_scan.len - 1);
        if (n > 0) {
            g_scan.len += (size_t)n;
            continue;
        }
        if (n == 0) {
            scan_finish(true);
            return;
        }
        return; /* EAGAIN: try again next call */
    }
}

static void scan_start(void)
{
    int fds[2];
    if (pipe(fds) < 0) {
        scan_arm_next(DC_NET_SCAN_RETRY_SEC);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        scan_arm_next(DC_NET_SCAN_RETRY_SEC);
        return;
    }

    if (pid == 0) { /* child: nmcli (twice) -> write end of the pipe */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        setsid();
        execl("/bin/sh", "sh", "-c",
              "nmcli -t -f SSID,SIGNAL,SECURITY,IN-USE dev wifi list; "
              "echo '" DC_NET_SCAN_MARKER "'; "
              "nmcli -t -f NAME connection show",
              (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    g_scan.fd = fds[0];
    g_scan.pid = pid;
    g_scan.len = 0;
    g_scan.fetch_active = true;
    clock_gettime(CLOCK_MONOTONIC, &g_scan.fetch_started);
}

int dc_net_wifi_scan(dc_net_wifi_ap *out, int max)
{
    if (g_scan.fetch_active)
        scan_drain();
    else if (scan_attempt_due())
        scan_start();

    int n = g_scan.count < max ? g_scan.count : max;
    if (n > 0)
        memcpy(out, g_scan.aps, (size_t)n * sizeof(*out));
    return n;
}

void dc_net_wifi_connect(const char *ssid)
{
    if (!ssid || !ssid[0])
        return;

    /* Single-quote the SSID for /bin/sh, escaping any embedded single quotes
     * ('\'' -- close quote, literal quote, reopen quote). */
    char quoted[192];
    size_t j = 0;
    quoted[j++] = '\'';
    for (const char *p = ssid; *p && j < sizeof(quoted) - 6; p++) {
        if (*p == '\'') {
            quoted[j++] = '\'';
            quoted[j++] = '\\';
            quoted[j++] = '\'';
            quoted[j++] = '\'';
        } else {
            quoted[j++] = *p;
        }
    }
    quoted[j++] = '\'';
    quoted[j] = '\0';

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect %s", quoted);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Force the current-connection cache to refresh on the next dc_net_wifi()
     * read instead of serving a pre-connect "Disconnected" for up to a few
     * seconds. */
    scan_arm_next(2);
}
