#include "services/net.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"
#include "dc.h"
#include "services/dbus.h"

/* --- NetworkManager D-Bus subscription (docs/15-PERF-PLAN.md T2.2) --------
 *
 * Preferred SSID/signal source: subscribe once to the Wi-Fi device's (and
 * its currently-active access point's) org.freedesktop.DBus.Properties
 * PropertiesChanged signal on the system bus (services/dbus.c already pumps
 * it on the event loop) and cache the last-known reading. No forking, no
 * polling -- the cache only changes when NetworkManager emits a signal.
 *
 * Only a single Wi-Fi device is tracked (resolved once at dc_net_init()
 * time) -- the common case of one built-in adapter. If NM has none, or the
 * system bus is unavailable, dc_net_wifi() falls back to the nmcli-popen
 * path below unconditionally.
 */
#define DC_NM_DEST "org.freedesktop.NetworkManager"
#define DC_NM_PATH "/org/freedesktop/NetworkManager"
#define DC_NM_DEVICE_IFACE "org.freedesktop.NetworkManager.Device"
#define DC_NM_WIRELESS_IFACE "org.freedesktop.NetworkManager.Device.Wireless"
#define DC_NM_WIRED_IFACE "org.freedesktop.NetworkManager.Device.Wired"
#define DC_NM_AP_IFACE "org.freedesktop.NetworkManager.AccessPoint"
#define DC_NM_IP4CONFIG_IFACE "org.freedesktop.NetworkManager.IP4Config"
#define DC_NM_ACTIVE_CONN_IFACE "org.freedesktop.NetworkManager.Connection.Active"
#define DC_NM_SETTINGS_PATH "/org/freedesktop/NetworkManager/Settings"
#define DC_NM_SETTINGS_IFACE "org.freedesktop.NetworkManager.Settings"
#define DC_NM_SETTINGS_CONN_IFACE "org.freedesktop.NetworkManager.Settings.Connection"
#define DC_NM_DEVICE_TYPE_WIFI 2
#define DC_NM_DEVICE_TYPE_ETHERNET 1

/* org.freedesktop.NetworkManager.Device.State (subset used to classify
 * dc_net_eth_state -- see the NM D-Bus API spec's NM_DEVICE_STATE enum). */
#define NM_DEVICE_STATE_UNAVAILABLE 20
#define NM_DEVICE_STATE_DISCONNECTED 30
#define NM_DEVICE_STATE_PREPARE 40
#define NM_DEVICE_STATE_SECONDARIES 90
#define NM_DEVICE_STATE_ACTIVATED 100
#define NM_DEVICE_STATE_DEACTIVATING 110

/* org.freedesktop.NetworkManager.Device.Wireless.Mode
 * (NM_802_11_MODE_AP -- used by dc_net_hotspot_active()). */
#define NM_802_11_MODE_AP 3

/* DANKC_NET_DRYRUN (docs/18-WIFI-BT-PLAN.md) -- gates every *new* write path
 * added alongside DANKC_WIFI_DRYRUN (ethernet connect/disconnect, hotspot
 * start/stop, saved-network forget/autoconnect-toggle): instead of issuing
 * the D-Bus call, log the exact destination/path/interface/method and args
 * that would have been sent. Left as its own env var (rather than folded
 * into DANKC_WIFI_DRYRUN) because it also covers ethernet and saved-network
 * writes that have nothing to do with Wi-Fi; DANKC_WIFI_DRYRUN keeps gating
 * only dc_net_wifi_connect_psk() exactly as it already did, so neither
 * changes the other's behavior. */
static bool net_dryrun(void)
{
    return getenv("DANKC_NET_DRYRUN") != NULL;
}

static struct {
    sd_bus *bus;
    bool have_wifi_device;
    char device_path[128];

    char ap_path[128];
    bool connected;
    char ssid[64];
    int signal_percent; /* -1 if unknown */

    sd_bus_slot *device_slot; /* PropertiesChanged on device_path (any iface) */
    sd_bus_slot *ap_slot;     /* PropertiesChanged on ap_path (Ssid/Strength) */
} g_nm = {.signal_percent = -1};

/* Read the active access point's Ssid (ay -- raw bytes, not NUL-terminated)
 * + Strength (y, 0-100) into the cache. Called once whenever g_nm.ap_path
 * resolves to a new object (not on every signal -- PropertiesChanged doesn't
 * repeat properties that haven't changed, so a plain re-Get is simpler and
 * still just an IPC round trip, never a fork). */
static void nm_read_ap_properties(void)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_get_property(g_nm.bus, DC_NM_DEST, g_nm.ap_path, DC_NM_AP_IFACE, "Ssid", &err,
                                &reply, "ay");
    if (r >= 0) {
        const void *bytes = NULL;
        size_t n = 0;
        if (sd_bus_message_read_array(reply, 'y', &bytes, &n) >= 0 && bytes && n > 0) {
            size_t copy = n < sizeof(g_nm.ssid) - 1 ? n : sizeof(g_nm.ssid) - 1;
            memcpy(g_nm.ssid, bytes, copy);
            g_nm.ssid[copy] = '\0';
        } else {
            g_nm.ssid[0] = '\0';
        }
    } else {
        g_nm.ssid[0] = '\0';
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);

    uint8_t strength = 0;
    sd_bus_error err2 = SD_BUS_ERROR_NULL;
    r = sd_bus_get_property_trivial(g_nm.bus, DC_NM_DEST, g_nm.ap_path, DC_NM_AP_IFACE, "Strength",
                                    &err2, 'y', &strength);
    sd_bus_error_free(&err2);
    g_nm.signal_percent = r >= 0 ? (int)strength : -1;
}

static int on_ap_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(m);
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    if (g_nm.ap_path[0])
        nm_read_ap_properties();
    return 0;
}

/* Re-read the Wi-Fi device's ActiveAccessPoint and, if it points at a new
 * (or no) access point, re-subscribe the AP-level match and refresh the
 * cached Ssid/Strength. Called once at init and whenever the device itself
 * signals a property change (cheaper to just re-check than to parse which
 * property changed out of the signal payload -- this is a rare event, not a
 * per-frame one). */
static void nm_resolve_ap_from_device(void)
{
    /* ActiveAccessPoint is an object-path property (signature "o"), *not*
     * a string ("s") -- sd_bus_get_property_string() hardcodes "s" and
     * fails outright on an "o" property, so this has to go through the
     * generic sd_bus_get_property() + sd_bus_message_read_basic('o', ...)
     * pair instead (same shape as nm_read_ap_properties()'s Ssid read). */
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_get_property(g_nm.bus, DC_NM_DEST, g_nm.device_path, DC_NM_WIRELESS_IFACE,
                                "ActiveAccessPoint", &err, &reply, "o");
    sd_bus_error_free(&err);

    const char *ap_path = NULL;
    if (r >= 0)
        sd_bus_message_read_basic(reply, 'o', &ap_path);

    bool have_ap = ap_path && ap_path[0] && strcmp(ap_path, "/") != 0;
    if (!have_ap) {
        sd_bus_message_unref(reply);
        if (g_nm.ap_slot) {
            sd_bus_slot_unref(g_nm.ap_slot);
            g_nm.ap_slot = NULL;
        }
        g_nm.ap_path[0] = '\0';
        g_nm.connected = false;
        g_nm.ssid[0] = '\0';
        g_nm.signal_percent = -1;
        return;
    }

    if (strcmp(g_nm.ap_path, ap_path) != 0) {
        snprintf(g_nm.ap_path, sizeof(g_nm.ap_path), "%s", ap_path);
        if (g_nm.ap_slot) {
            sd_bus_slot_unref(g_nm.ap_slot);
            g_nm.ap_slot = NULL;
        }
        sd_bus_match_signal(g_nm.bus, &g_nm.ap_slot, DC_NM_DEST, g_nm.ap_path,
                            "org.freedesktop.DBus.Properties", "PropertiesChanged",
                            on_ap_properties_changed, NULL);
    }
    sd_bus_message_unref(reply);

    g_nm.connected = true;
    nm_read_ap_properties();
}

static int on_device_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(m);
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    nm_resolve_ap_from_device();
    return 0;
}

/* GetDevices() + DeviceType property reads to find the first device of
 * `want_type` (NM_DEVICE_TYPE_WIFI == 2, NM_DEVICE_TYPE_ETHERNET == 1). A
 * handful of small IPC round trips at startup only -- never repeated, never
 * a fork. Shared by nm_find_wifi_device() and the ethernet device resolver
 * below (dc_net_init()). */
static bool nm_find_device_by_type(sd_bus *bus, uint32_t want_type, char *out, size_t out_sz)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus, DC_NM_DEST, DC_NM_PATH, DC_NM_DEST, "GetDevices", &err, &reply,
                               "");
    sd_bus_error_free(&err);
    if (r < 0)
        return false;

    bool found = false;
    if (sd_bus_message_enter_container(reply, 'a', "o") >= 0) {
        const char *dev_path = NULL;
        while (!found && sd_bus_message_read_basic(reply, 'o', &dev_path) > 0) {
            uint32_t dtype = 0;
            sd_bus_error dt_err = SD_BUS_ERROR_NULL;
            int dr = sd_bus_get_property_trivial(bus, DC_NM_DEST, dev_path, DC_NM_DEVICE_IFACE,
                                                 "DeviceType", &dt_err, 'u', &dtype);
            sd_bus_error_free(&dt_err);
            if (dr >= 0 && dtype == want_type) {
                snprintf(out, out_sz, "%s", dev_path);
                found = true;
            }
        }
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
    return found;
}

static bool nm_find_wifi_device(char *out, size_t out_sz)
{
    return nm_find_device_by_type(g_nm.bus, DC_NM_DEVICE_TYPE_WIFI, out, out_sz);
}

/* Shared system-bus handle for the ethernet/hotspot/saved-networks paths
 * below (docs/18-WIFI-BT-PLAN.md). Kept separate from g_nm.bus, which
 * dc_net_init() deliberately nulls out when no Wi-Fi device is found (so
 * dc_net_wifi() knows to fall back to nmcli) -- that null-out must not also
 * disable ethernet/saved-networks D-Bus access, since those don't depend on
 * a Wi-Fi device existing. NULL if the system bus itself is unavailable. */
static sd_bus *g_net_bus;

static void nm_eth_init(sd_bus *bus);

void dc_net_init(struct dc_dbus *dbus)
{
    sd_bus *bus = dbus ? dbus->system : NULL;
    g_net_bus = bus;
    if (!bus) {
        dc_info("net: no system bus; wifi status will use the nmcli fallback");
        return;
    }
    g_nm.bus = bus;

    if (!nm_find_wifi_device(g_nm.device_path, sizeof(g_nm.device_path))) {
        dc_info("net: no NetworkManager Wi-Fi device found; wifi status will use the nmcli "
                "fallback");
        g_nm.bus = NULL; /* dc_net_wifi() gates on g_nm.have_wifi_device, but be explicit */
    } else {
        g_nm.have_wifi_device = true;

        sd_bus_match_signal(g_nm.bus, &g_nm.device_slot, DC_NM_DEST, g_nm.device_path,
                            "org.freedesktop.DBus.Properties", "PropertiesChanged",
                            on_device_properties_changed, NULL);
        nm_resolve_ap_from_device();

        dc_info("net: NetworkManager wifi device %s -- event-driven, no nmcli poll",
                g_nm.device_path);
    }

    nm_eth_init(bus);
}

/* SSID/signal via `nmcli` (a fork+popen, same shell-out style already used by
 * services/audio.c for wpctl and controlcenter.c for rfkill/wpctl/
 * brightnessctl) -- cached briefly since `nmcli dev wifi list` is a slower
 * call than a sysfs read and dc_net_wifi() can be polled every render frame
 * during a popout's entrance animation.
 *
 * Fallback only: used when dc_net_init() couldn't subscribe to
 * NetworkManager over D-Bus (no system bus, or NM has no Wi-Fi device). */
#define DC_NET_WIFI_CACHE_SECONDS 3

static void refresh_wifi_details_fallback(char *ssid, size_t ssid_sz, int *signal_percent)
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

/* Preferred path: serve straight from the NetworkManager D-Bus cache
 * (updated by PropertiesChanged signals, see above) -- zero forks. Falls
 * back to the nmcli popen() path only if dc_net_init() never got a working
 * NetworkManager Wi-Fi device. */
static void refresh_wifi_details(char *ssid, size_t ssid_sz, int *signal_percent)
{
    if (g_nm.have_wifi_device) {
        if (g_nm.connected && g_nm.ssid[0]) {
            snprintf(ssid, ssid_sz, "%s", g_nm.ssid);
            *signal_percent = g_nm.signal_percent;
        } else {
            ssid[0] = '\0';
            *signal_percent = -1;
        }
        return;
    }

    refresh_wifi_details_fallback(ssid, ssid_sz, signal_percent);
}

/* No fork here (just a handful of sysfs opendir/fopen calls), but
 * docs/POLISH.md P7 item 2 still caches it briefly: the bar's damage-
 * tracking hash (ui/bar/bar.c bar_compute_signature()) now calls this once
 * per bar per ~1Hz tick on top of the control-center-pill's own draw-time
 * call, and link-up/down state doesn't need sub-2s freshness. */
#define DC_NET_LINK_CACHE_SECONDS 2

bool dc_net_wifi(dc_net_info *out)
{
    static dc_net_info cached;
    static bool cached_has_wifi;
    static time_t cache_time;
    static bool cache_valid = false;

    time_t now = time(NULL);
    if (cache_valid && now - cache_time < DC_NET_LINK_CACHE_SECONDS) {
        *out = cached;
        return cached_has_wifi;
    }

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

    cached = *out;
    cached_has_wifi = out->has_wifi;
    cache_time = now;
    cache_valid = true;
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

    /* DANKC_WIFI_FAKE_AP=<ssid> (debug-only, env-gated -- same convention as
     * services/battery.c's DANKC_FAKE_BATTERY): append one synthetic
     * secured, not-yet-known, not-in-use AP so the inline password panel
     * (W1.1) can be screenshotted on a machine whose real scan results don't
     * happen to include an unknown secured network. No-op when unset. */
    const char *fake_ssid = getenv("DANKC_WIFI_FAKE_AP");
    if (fake_ssid && fake_ssid[0] && n < max) {
        dc_net_wifi_ap *ap = &out[n];
        memset(ap, 0, sizeof(*ap));
        snprintf(ap->ssid, sizeof(ap->ssid), "%s", fake_ssid);
        ap->signal_percent = 62;
        ap->secured = true;
        ap->in_use = false;
        ap->known = false;
        n++;
    }

    return n;
}

/* Single-quote `in` for /bin/sh into `out`, escaping any embedded single
 * quotes ('\'' -- close quote, literal quote, reopen quote). Shared by
 * dc_net_wifi_connect() and the password-entry connect job below so both
 * build shell-safe nmcli invocations the same way. */
static void shell_quote_single(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    if (out_sz == 0)
        return;
    out[j++] = '\'';
    for (const char *p = in; *p && j < out_sz - 6; p++) {
        if (*p == '\'') {
            out[j++] = '\'';
            out[j++] = '\\';
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            out[j++] = *p;
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
}

void dc_net_wifi_connect(const char *ssid)
{
    if (!ssid || !ssid[0])
        return;

    char quoted[192];
    shell_quote_single(ssid, quoted, sizeof(quoted));

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

/* --- Wi-Fi password entry connect job (control-center inline password
 * field, W1.1) -- same fork+pipe+non-blocking-drain shape as the scan list
 * above, but for a single one-shot `nmcli dev wifi connect ... password ...`
 * whose combined stdout+stderr is inspected once the child exits so the UI
 * can tell a wrong password apart from success. */
#define DC_NET_CONNECT_TIMEOUT_SEC 15
#define DC_NET_CONNECT_BUF_CAP 2048
#define DC_NET_CONNECT_DRYRUN_DELAY_SEC 1

static struct {
    dc_net_connect_state state;
    char err[128];

    bool active; /* a real nmcli child is running */
    pid_t pid;
    int fd;
    char buf[DC_NET_CONNECT_BUF_CAP];
    size_t len;
    struct timespec started;

    bool dryrun_pending; /* DANKC_WIFI_DRYRUN: simulate without a child */
} g_connect = {.fd = -1};

/* Look for nmcli's own "Error: ..." line in its (stdout+stderr) output and
 * copy it into `err` -- that's the only failure signal available (no exit
 * status; SIGCHLD is SIG_IGN process-wide, see the file header comment). */
static void connect_extract_error(const char *buf, char *err, size_t err_sz)
{
    const char *e = strstr(buf, "Error");
    if (!e)
        e = strstr(buf, "error");
    if (!e) {
        snprintf(err, err_sz, "Connection failed");
        return;
    }
    size_t len = strcspn(e, "\r\n");
    if (len >= err_sz)
        len = err_sz - 1;
    memcpy(err, e, len);
    err[len] = '\0';
}

static void connect_finish(bool eof_reached)
{
    close(g_connect.fd);
    g_connect.fd = -1;
    g_connect.active = false;

    if (!eof_reached) {
        g_connect.state = DC_NET_CONNECT_FAILED;
        snprintf(g_connect.err, sizeof(g_connect.err), "Timed out");
        return;
    }

    g_connect.buf[g_connect.len] = '\0';
    if (strstr(g_connect.buf, "Error") || strstr(g_connect.buf, "error")) {
        g_connect.state = DC_NET_CONNECT_FAILED;
        connect_extract_error(g_connect.buf, g_connect.err, sizeof(g_connect.err));
    } else {
        g_connect.state = DC_NET_CONNECT_SUCCESS;
        /* Reflect "Connected" in the scan list soon instead of waiting a
         * full DC_NET_SCAN_REFRESH_SEC. */
        scan_arm_next(2);
    }
}

void dc_net_wifi_connect_reset(void)
{
    if (g_connect.pid > 0)
        kill(g_connect.pid, SIGKILL);
    if (g_connect.fd >= 0)
        close(g_connect.fd);
    memset(&g_connect, 0, sizeof(g_connect));
    g_connect.fd = -1;
    g_connect.state = DC_NET_CONNECT_IDLE;
}

void dc_net_wifi_connect_psk(const char *ssid, const char *psk)
{
    if (!ssid || !ssid[0])
        return;
    dc_net_wifi_connect_reset();

    char quoted_ssid[192], quoted_psk[192];
    shell_quote_single(ssid, quoted_ssid, sizeof(quoted_ssid));
    shell_quote_single(psk ? psk : "", quoted_psk, sizeof(quoted_psk));

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect %s password %s 2>&1", quoted_ssid,
             quoted_psk);

    g_connect.state = DC_NET_CONNECT_IN_PROGRESS;
    clock_gettime(CLOCK_MONOTONIC, &g_connect.started);

    if (getenv("DANKC_WIFI_DRYRUN")) {
        /* Never spawn nmcli -- just prove the command was built correctly. */
        dc_info("net: [DANKC_WIFI_DRYRUN] would run: %s", cmd);
        g_connect.dryrun_pending = true;
        return;
    }

    int fds[2];
    if (pipe(fds) < 0) {
        g_connect.state = DC_NET_CONNECT_FAILED;
        snprintf(g_connect.err, sizeof(g_connect.err), "pipe() failed");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        g_connect.state = DC_NET_CONNECT_FAILED;
        snprintf(g_connect.err, sizeof(g_connect.err), "fork() failed");
        return;
    }

    if (pid == 0) { /* child: nmcli -> write end of the pipe */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    g_connect.fd = fds[0];
    g_connect.pid = pid;
    g_connect.len = 0;
    g_connect.active = true;
}

dc_net_connect_state dc_net_wifi_connect_poll(char *err, size_t err_sz)
{
    if (g_connect.dryrun_pending) {
        if (scan_secs_since(&g_connect.started) < DC_NET_CONNECT_DRYRUN_DELAY_SEC)
            return DC_NET_CONNECT_IN_PROGRESS;
        g_connect.dryrun_pending = false;
        g_connect.state = DC_NET_CONNECT_SUCCESS;
        return g_connect.state;
    }

    if (g_connect.active) {
        if (scan_secs_since(&g_connect.started) > DC_NET_CONNECT_TIMEOUT_SEC) {
            kill(g_connect.pid, SIGKILL);
            connect_finish(false);
        } else {
            struct pollfd pfd = {.fd = g_connect.fd, .events = POLLIN};
            if (poll(&pfd, 1, 0) > 0) {
                for (;;) {
                    if (g_connect.len + 1 >= sizeof(g_connect.buf)) {
                        connect_finish(true);
                        break;
                    }
                    ssize_t n = read(g_connect.fd, g_connect.buf + g_connect.len,
                                     sizeof(g_connect.buf) - g_connect.len - 1);
                    if (n > 0) {
                        g_connect.len += (size_t)n;
                        continue;
                    }
                    if (n == 0)
                        connect_finish(true);
                    break; /* EAGAIN (n < 0): try again next call */
                }
            }
        }
    }

    if (g_connect.state == DC_NET_CONNECT_FAILED && err && err_sz)
        snprintf(err, err_sz, "%s", g_connect.err);
    return g_connect.state;
}

/* --- Ethernet (wired) — NetworkManager D-Bus (docs/18-WIFI-BT-PLAN.md sec.2.1)
 * ---------------------------------------------------------------------------
 *
 * Same event-driven shape as the Wi-Fi device tracking at the top of this
 * file: resolve the wired device once at dc_net_init() time, subscribe to
 * its PropertiesChanged, and additionally track its Ip4Config object -- NM
 * replaces that object with a fresh path on every reconfigure (DHCP renew,
 * cable replug, ...), so the IP4Config subscription has to be re-armed
 * exactly like the Wi-Fi AP subscription is in nm_resolve_ap_from_device().
 */
static struct {
    sd_bus *bus;
    bool have_device;
    char device_path[128];

    char ip4_path[128];
    sd_bus_slot *device_slot; /* PropertiesChanged on device_path */
    sd_bus_slot *ip4_slot;    /* PropertiesChanged on ip4_path, re-armed on change */

    dc_net_eth_info info;
} g_eth;

/* Read Device.Ip4Config's AddressData (first entry)/Gateway/NameserverData
 * (up to DC_NET_ETH_DNS_MAX) into g_eth.info. Called whenever g_eth.ip4_path
 * resolves to a new object and whenever that object itself signals a
 * property change (DHCP lease renewal can change the gateway/DNS without the
 * object path changing). */
static void eth_read_ip4(void)
{
    dc_net_eth_info *info = &g_eth.info;
    info->ipv4_address[0] = '\0';
    info->ipv4_prefix = 0;
    info->ipv4_gateway[0] = '\0';
    info->ipv4_dns_count = 0;

    if (!g_eth.ip4_path[0])
        return;

    char *gateway = NULL;
    sd_bus_error gerr = SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_string(g_eth.bus, DC_NM_DEST, g_eth.ip4_path, DC_NM_IP4CONFIG_IFACE,
                                   "Gateway", &gerr, &gateway) >= 0 &&
        gateway)
        snprintf(info->ipv4_gateway, sizeof(info->ipv4_gateway), "%s", gateway);
    sd_bus_error_free(&gerr);
    free(gateway);

    /* AddressData / NameserverData: "aa{sv}" -- an array of dicts, each with
     * an "address" (s) key (AddressData also has "prefix" (u)). Every entry
     * has to be drained even though only the first (DC_NET_ETH_DNS_MAX for
     * nameservers) is kept, or sd_bus_message_exit_container() below would
     * be called with unread siblings still pending in the array frame. */
    sd_bus_error aerr = SD_BUS_ERROR_NULL;
    sd_bus_message *areply = NULL;
    if (sd_bus_get_property(g_eth.bus, DC_NM_DEST, g_eth.ip4_path, DC_NM_IP4CONFIG_IFACE,
                            "AddressData", &aerr, &areply, "aa{sv}") >= 0 &&
        sd_bus_message_enter_container(areply, 'a', "{sv}") > 0) {
        bool have_addr = false;
        while (sd_bus_message_enter_container(areply, 'a', "{sv}") > 0) {
            char address[64] = {0};
            uint32_t prefix = 0;
            while (sd_bus_message_enter_container(areply, 'e', "sv") > 0) {
                const char *key = NULL;
                sd_bus_message_read_basic(areply, 's', &key);
                if (key && strcmp(key, "address") == 0) {
                    const char *val = NULL;
                    sd_bus_message_enter_container(areply, 'v', "s");
                    sd_bus_message_read_basic(areply, 's', &val);
                    sd_bus_message_exit_container(areply);
                    if (val)
                        snprintf(address, sizeof(address), "%s", val);
                } else if (key && strcmp(key, "prefix") == 0) {
                    sd_bus_message_enter_container(areply, 'v', "u");
                    sd_bus_message_read_basic(areply, 'u', &prefix);
                    sd_bus_message_exit_container(areply);
                } else {
                    sd_bus_message_skip(areply, "v");
                }
                sd_bus_message_exit_container(areply); /* e sv */
            }
            sd_bus_message_exit_container(areply); /* a{sv} */
            if (!have_addr && address[0]) {
                snprintf(info->ipv4_address, sizeof(info->ipv4_address), "%s", address);
                info->ipv4_prefix = prefix;
                have_addr = true;
            }
        }
        sd_bus_message_exit_container(areply); /* aa{sv} */
    }
    sd_bus_error_free(&aerr);
    sd_bus_message_unref(areply);

    sd_bus_error nerr = SD_BUS_ERROR_NULL;
    sd_bus_message *nreply = NULL;
    if (sd_bus_get_property(g_eth.bus, DC_NM_DEST, g_eth.ip4_path, DC_NM_IP4CONFIG_IFACE,
                            "NameserverData", &nerr, &nreply, "aa{sv}") >= 0 &&
        sd_bus_message_enter_container(nreply, 'a', "{sv}") > 0) {
        while (sd_bus_message_enter_container(nreply, 'a', "{sv}") > 0) {
            char address[64] = {0};
            while (sd_bus_message_enter_container(nreply, 'e', "sv") > 0) {
                const char *key = NULL;
                sd_bus_message_read_basic(nreply, 's', &key);
                if (key && strcmp(key, "address") == 0) {
                    const char *val = NULL;
                    sd_bus_message_enter_container(nreply, 'v', "s");
                    sd_bus_message_read_basic(nreply, 's', &val);
                    sd_bus_message_exit_container(nreply);
                    if (val)
                        snprintf(address, sizeof(address), "%s", val);
                } else {
                    sd_bus_message_skip(nreply, "v");
                }
                sd_bus_message_exit_container(nreply); /* e sv */
            }
            sd_bus_message_exit_container(nreply); /* a{sv} */
            if (address[0] && info->ipv4_dns_count < DC_NET_ETH_DNS_MAX)
                snprintf(info->ipv4_dns[info->ipv4_dns_count++], sizeof(info->ipv4_dns[0]), "%s",
                        address);
        }
        sd_bus_message_exit_container(nreply); /* aa{sv} */
    }
    sd_bus_error_free(&nerr);
    sd_bus_message_unref(nreply);
}

static int on_eth_ip4_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(m);
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    eth_read_ip4();
    return 0;
}

/* Full re-read of the wired device: State + Wired.Carrier (classified into
 * dc_net_eth_state), HwAddress, Wired.Speed, the active connection's display
 * name, and -- if Device.Ip4Config points at a new object -- re-subscribing
 * the IP4Config match before re-reading it via eth_read_ip4(). Called once at
 * init and whenever the device itself signals a property change. */
static void nm_eth_refresh(void)
{
    dc_net_eth_info *info = &g_eth.info;
    info->has_device = true;

    char *val = NULL;
    sd_bus_error ierr = SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_string(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_DEVICE_IFACE,
                                   "Interface", &ierr, &val) >= 0 &&
        val)
        snprintf(info->device_name, sizeof(info->device_name), "%s", val);
    sd_bus_error_free(&ierr);
    free(val);
    val = NULL;

    uint32_t nm_state = 0;
    sd_bus_error serr = SD_BUS_ERROR_NULL;
    sd_bus_get_property_trivial(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_DEVICE_IFACE,
                                "State", &serr, 'u', &nm_state);
    sd_bus_error_free(&serr);

    int carrier = 0;
    sd_bus_error cerr = SD_BUS_ERROR_NULL;
    sd_bus_get_property_trivial(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_WIRED_IFACE,
                                "Carrier", &cerr, 'b', &carrier);
    sd_bus_error_free(&cerr);

    if (!carrier)
        info->state = DC_NET_ETH_NO_CABLE;
    else if (nm_state == NM_DEVICE_STATE_ACTIVATED)
        info->state = DC_NET_ETH_CONNECTED;
    else if (nm_state >= NM_DEVICE_STATE_PREPARE && nm_state <= NM_DEVICE_STATE_SECONDARIES)
        info->state = DC_NET_ETH_CONNECTING;
    else if (nm_state == NM_DEVICE_STATE_DISCONNECTED)
        info->state = DC_NET_ETH_DISCONNECTED;
    else /* UNAVAILABLE, UNMANAGED, DEACTIVATING, FAILED, UNKNOWN */
        info->state = DC_NET_ETH_UNAVAILABLE;

    info->mac[0] = '\0';
    sd_bus_error werr = SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_string(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_WIRED_IFACE,
                                   "HwAddress", &werr, &val) >= 0 &&
        val)
        snprintf(info->mac, sizeof(info->mac), "%s", val);
    sd_bus_error_free(&werr);
    free(val);
    val = NULL;
    if (!info->mac[0]) { /* fall back to the generic Device.HwAddress */
        sd_bus_error herr = SD_BUS_ERROR_NULL;
        if (sd_bus_get_property_string(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_DEVICE_IFACE,
                                       "HwAddress", &herr, &val) >= 0 &&
            val)
            snprintf(info->mac, sizeof(info->mac), "%s", val);
        sd_bus_error_free(&herr);
        free(val);
        val = NULL;
    }

    uint32_t speed = 0;
    sd_bus_error sperr = SD_BUS_ERROR_NULL;
    sd_bus_get_property_trivial(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_WIRED_IFACE,
                                "Speed", &sperr, 'u', &speed);
    sd_bus_error_free(&sperr);
    info->link_speed_mbps = (info->state == DC_NET_ETH_CONNECTED) ? speed : 0;

    info->connection_name[0] = '\0';
    sd_bus_error acerr = SD_BUS_ERROR_NULL;
    sd_bus_message *acreply = NULL;
    if (sd_bus_get_property(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_DEVICE_IFACE,
                            "ActiveConnection", &acerr, &acreply, "o") >= 0) {
        const char *ac = NULL;
        sd_bus_message_read_basic(acreply, 'o', &ac);
        if (ac && ac[0] && strcmp(ac, "/") != 0) {
            char *id = NULL;
            sd_bus_error iderr = SD_BUS_ERROR_NULL;
            if (sd_bus_get_property_string(g_eth.bus, DC_NM_DEST, ac, DC_NM_ACTIVE_CONN_IFACE, "Id",
                                           &iderr, &id) >= 0 &&
                id)
                snprintf(info->connection_name, sizeof(info->connection_name), "%s", id);
            sd_bus_error_free(&iderr);
            free(id);
        }
    }
    sd_bus_error_free(&acerr);
    sd_bus_message_unref(acreply);

    sd_bus_error ip4err = SD_BUS_ERROR_NULL;
    sd_bus_message *ip4reply = NULL;
    if (sd_bus_get_property(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_DEVICE_IFACE,
                            "Ip4Config", &ip4err, &ip4reply, "o") >= 0) {
        const char *ip4_path = NULL;
        sd_bus_message_read_basic(ip4reply, 'o', &ip4_path);
        bool have_ip4 = ip4_path && ip4_path[0] && strcmp(ip4_path, "/") != 0;
        if (have_ip4 && strcmp(g_eth.ip4_path, ip4_path) != 0) {
            snprintf(g_eth.ip4_path, sizeof(g_eth.ip4_path), "%s", ip4_path);
            if (g_eth.ip4_slot) {
                sd_bus_slot_unref(g_eth.ip4_slot);
                g_eth.ip4_slot = NULL;
            }
            sd_bus_match_signal(g_eth.bus, &g_eth.ip4_slot, DC_NM_DEST, g_eth.ip4_path,
                                "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                on_eth_ip4_properties_changed, NULL);
        } else if (!have_ip4 && g_eth.ip4_path[0]) {
            if (g_eth.ip4_slot) {
                sd_bus_slot_unref(g_eth.ip4_slot);
                g_eth.ip4_slot = NULL;
            }
            g_eth.ip4_path[0] = '\0';
        }
    }
    sd_bus_error_free(&ip4err);
    sd_bus_message_unref(ip4reply);

    eth_read_ip4();
}

static int on_eth_device_properties_changed(sd_bus_message *m, void *userdata,
                                            sd_bus_error *ret_error)
{
    DC_UNUSED(m);
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    nm_eth_refresh();
    return 0;
}

/* Resolve the first NM_DEVICE_TYPE_ETHERNET device (if any) and subscribe to
 * it, mirroring dc_net_init()'s Wi-Fi device resolution above. Safe no-op
 * (dc_net_ethernet() reports has_device=false) if `bus` is NULL or the
 * system has no wired device known to NetworkManager. */
static void nm_eth_init(sd_bus *bus)
{
    if (!bus)
        return;
    g_eth.bus = bus;

    if (!nm_find_device_by_type(bus, DC_NM_DEVICE_TYPE_ETHERNET, g_eth.device_path,
                                sizeof(g_eth.device_path))) {
        dc_info("net: no NetworkManager ethernet device found");
        return;
    }
    g_eth.have_device = true;

    sd_bus_match_signal(g_eth.bus, &g_eth.device_slot, DC_NM_DEST, g_eth.device_path,
                        "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        on_eth_device_properties_changed, NULL);
    nm_eth_refresh();

    dc_info("net: NetworkManager ethernet device %s -- event-driven, no nmcli poll",
            g_eth.device_path);
}

bool dc_net_ethernet(dc_net_eth_info *out)
{
    if (!g_eth.have_device) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    *out = g_eth.info;
    return true;
}

void dc_net_eth_connect(void)
{
    if (!g_eth.have_device) {
        dc_warn("net: dc_net_eth_connect() called with no ethernet device present");
        return;
    }

    /* Device.AvailableConnections is already filtered by NM to connections
     * compatible with this device; use the first if any exist, else "/" so
     * NetworkManager picks/creates a default profile for the device. */
    char conn_path[128] = "/";
    sd_bus_error aerr = SD_BUS_ERROR_NULL;
    sd_bus_message *areply = NULL;
    if (sd_bus_get_property(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_DEVICE_IFACE,
                            "AvailableConnections", &aerr, &areply, "ao") >= 0 &&
        sd_bus_message_enter_container(areply, 'a', "o") > 0) {
        const char *first = NULL;
        if (sd_bus_message_read_basic(areply, 'o', &first) > 0 && first)
            snprintf(conn_path, sizeof(conn_path), "%s", first);
        sd_bus_message_exit_container(areply);
    }
    sd_bus_error_free(&aerr);
    sd_bus_message_unref(areply);

    if (net_dryrun()) {
        dc_info("net: [DANKC_NET_DRYRUN] would call %s %s %s.ActivateConnection(\"%s\", \"%s\", "
                "\"/\")",
                DC_NM_DEST, DC_NM_PATH, DC_NM_DEST, conn_path, g_eth.device_path);
        return;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_eth.bus, DC_NM_DEST, DC_NM_PATH, DC_NM_DEST, "ActivateConnection",
                               &err, &reply, "ooo", conn_path, g_eth.device_path, "/");
    if (r < 0)
        dc_warn("net: ActivateConnection (ethernet) failed: %s",
                err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
}

void dc_net_eth_disconnect(void)
{
    if (!g_eth.have_device)
        return;

    char active_path[128] = {0};
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    if (sd_bus_get_property(g_eth.bus, DC_NM_DEST, g_eth.device_path, DC_NM_DEVICE_IFACE,
                            "ActiveConnection", &err, &reply, "o") >= 0) {
        const char *ac = NULL;
        sd_bus_message_read_basic(reply, 'o', &ac);
        if (ac && ac[0] && strcmp(ac, "/") != 0)
            snprintf(active_path, sizeof(active_path), "%s", ac);
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);

    if (!active_path[0])
        return; /* nothing active to deactivate */

    if (net_dryrun()) {
        dc_info("net: [DANKC_NET_DRYRUN] would call %s %s %s.DeactivateConnection(\"%s\")",
                DC_NM_DEST, DC_NM_PATH, DC_NM_DEST, active_path);
        return;
    }

    sd_bus_error derr = SD_BUS_ERROR_NULL;
    sd_bus_message *dreply = NULL;
    int r = sd_bus_call_method(g_eth.bus, DC_NM_DEST, DC_NM_PATH, DC_NM_DEST,
                               "DeactivateConnection", &derr, &dreply, "o", active_path);
    if (r < 0)
        dc_warn("net: DeactivateConnection (ethernet) failed: %s",
                derr.message ? derr.message : strerror(-r));
    sd_bus_error_free(&derr);
    sd_bus_message_unref(dreply);
}

/* --- Wi-Fi known/saved networks — NetworkManager D-Bus (docs/18-WIFI-BT-PLAN.md
 * sec.2.3) -------------------------------------------------------------------
 *
 * Unlike the scan list (services/weather.c-style fork+pipe, cached 8s),
 * these are a handful of synchronous D-Bus round trips: ListConnections()
 * returns object paths already resolved by NetworkManager, and each
 * GetSettings() is a single small method call, no fork needed.
 */

/* Parse a Settings.Connection.GetSettings() a{sa{sv}} reply, pulling out
 * connection.id/type/autoconnect and (if present) 802-11-wireless.ssid
 * (an NM byte array, decoded here into a C string). Any out-param may be
 * NULL if the caller doesn't need it. autoconnect defaults to true when the
 * key is absent from the reply -- that's NM's own default, not just this
 * function's. Returns false only if the GetSettings call itself failed. */
static bool net_conn_read_settings(const char *conn_path, char *id_out, size_t id_len,
                                   char *type_out, size_t type_len, bool *autoconnect_out,
                                   char *ssid_out, size_t ssid_len)
{
    if (id_out && id_len)
        id_out[0] = '\0';
    if (type_out && type_len)
        type_out[0] = '\0';
    if (autoconnect_out)
        *autoconnect_out = true;
    if (ssid_out && ssid_len)
        ssid_out[0] = '\0';

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_net_bus, DC_NM_DEST, conn_path, DC_NM_SETTINGS_CONN_IFACE,
                               "GetSettings", &err, &reply, "");
    sd_bus_error_free(&err);
    if (r < 0)
        return false;

    if (sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") > 0) {
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *group = NULL;
            sd_bus_message_read_basic(reply, 's', &group);
            bool is_conn = group && strcmp(group, "connection") == 0;
            bool is_wifi = group && strcmp(group, "802-11-wireless") == 0;

            if ((is_conn || is_wifi) && sd_bus_message_enter_container(reply, 'a', "{sv}") > 0) {
                while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                    const char *key = NULL;
                    sd_bus_message_read_basic(reply, 's', &key);
                    if (is_conn && key && strcmp(key, "id") == 0 && id_out) {
                        const char *v = NULL;
                        sd_bus_message_enter_container(reply, 'v', "s");
                        sd_bus_message_read_basic(reply, 's', &v);
                        sd_bus_message_exit_container(reply);
                        if (v)
                            snprintf(id_out, id_len, "%s", v);
                    } else if (is_conn && key && strcmp(key, "type") == 0 && type_out) {
                        const char *v = NULL;
                        sd_bus_message_enter_container(reply, 'v', "s");
                        sd_bus_message_read_basic(reply, 's', &v);
                        sd_bus_message_exit_container(reply);
                        if (v)
                            snprintf(type_out, type_len, "%s", v);
                    } else if (is_conn && key && strcmp(key, "autoconnect") == 0 &&
                              autoconnect_out) {
                        int v = 1;
                        sd_bus_message_enter_container(reply, 'v', "b");
                        sd_bus_message_read_basic(reply, 'b', &v);
                        sd_bus_message_exit_container(reply);
                        *autoconnect_out = v != 0;
                    } else if (is_wifi && key && strcmp(key, "ssid") == 0 && ssid_out) {
                        const void *bytes = NULL;
                        size_t n = 0;
                        sd_bus_message_enter_container(reply, 'v', "ay");
                        sd_bus_message_read_array(reply, 'y', &bytes, &n);
                        sd_bus_message_exit_container(reply);
                        if (bytes && n > 0) {
                            size_t copy = n < ssid_len - 1 ? n : ssid_len - 1;
                            memcpy(ssid_out, bytes, copy);
                            ssid_out[copy] = '\0';
                        }
                    } else {
                        sd_bus_message_skip(reply, "v");
                    }
                    sd_bus_message_exit_container(reply); /* e sv */
                }
                sd_bus_message_exit_container(reply); /* a{sv} */
            } else if (!is_conn && !is_wifi) {
                sd_bus_message_skip(reply, "a{sv}");
            }
            sd_bus_message_exit_container(reply); /* e sa{sv} */
        }
        sd_bus_message_exit_container(reply); /* a{sa{sv}} */
    }
    sd_bus_message_unref(reply);
    return true;
}

/* Recursively duplicate the value at `src`'s current read position into the
 * message being built at `dst`, generically for every D-Bus container type
 * (array/struct/dict-entry/variant) plus all basic scalar types.
 *
 * Needed for dc_net_saved_set_autoconnect(): NetworkManager's
 * Settings.Connection.Update() replaces the *entire* connection (there is no
 * partial-merge API), so toggling one "connection.autoconnect" key means
 * re-serializing every other group/key exactly as GetSettings() returned it.
 * This is the generic "copy everything except the one key I'm patching"
 * primitive that makes that safe without hand-writing a re-encoder for every
 * settings group NetworkManager might have on a saved connection (802.1x,
 * ipv6, proxy, bond/team/bridge sub-settings, ...). */
static int net_copy_value(sd_bus_message *dst, sd_bus_message *src)
{
    char type = 0;
    const char *contents = NULL;
    int r = sd_bus_message_peek_type(src, &type, &contents);
    if (r <= 0)
        return r;

    if (type == SD_BUS_TYPE_ARRAY || type == SD_BUS_TYPE_STRUCT ||
        type == SD_BUS_TYPE_DICT_ENTRY || type == SD_BUS_TYPE_VARIANT) {
        r = sd_bus_message_enter_container(src, type, contents);
        if (r < 0)
            return r;
        r = sd_bus_message_open_container(dst, type, contents);
        if (r < 0) {
            sd_bus_message_exit_container(src);
            return r;
        }
        while (sd_bus_message_at_end(src, false) == 0) {
            r = net_copy_value(dst, src);
            if (r < 0)
                break;
        }
        sd_bus_message_exit_container(src);
        sd_bus_message_close_container(dst);
        return r;
    }

    /* Basic scalar type: read into a union big enough for any of them (the
     * union's address is valid for every member since they share storage),
     * then re-append with the same type code -- both calls dispatch on
     * `type` internally, same as every other read_basic()/append() pair
     * already used throughout this file. */
    union {
        uint8_t y;
        int b;
        int16_t n;
        uint16_t q;
        int32_t i;
        uint32_t u;
        int64_t x;
        uint64_t t;
        double d;
        const char *s;
    } val;
    r = sd_bus_message_read_basic(src, type, &val);
    if (r < 0)
        return r;
    return sd_bus_message_append_basic(dst, type, &val);
}

static struct {
    dc_net_saved_net items[DC_NET_SAVED_MAX];
    int count;
} g_saved;

int dc_net_saved_list(dc_net_saved_net *out, int max)
{
    g_saved.count = 0;

    if (g_net_bus) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        int r = sd_bus_call_method(g_net_bus, DC_NM_DEST, DC_NM_SETTINGS_PATH, DC_NM_SETTINGS_IFACE,
                                   "ListConnections", &err, &reply, "");
        sd_bus_error_free(&err);

        if (r >= 0 && sd_bus_message_enter_container(reply, 'a', "o") >= 0) {
            const char *path = NULL;
            while (g_saved.count < DC_NET_SAVED_MAX &&
                  sd_bus_message_read_basic(reply, 'o', &path) > 0) {
                char id[64], type[32], ssid[64];
                bool autoconnect = true;
                if (!net_conn_read_settings(path, id, sizeof(id), type, sizeof(type), &autoconnect,
                                            ssid, sizeof(ssid)))
                    continue;
                if (strcmp(type, "802-11-wireless") != 0)
                    continue;

                dc_net_saved_net *s = &g_saved.items[g_saved.count++];
                snprintf(s->path, sizeof(s->path), "%s", path);
                snprintf(s->id, sizeof(s->id), "%s", id);
                snprintf(s->ssid, sizeof(s->ssid), "%s", ssid);
                s->autoconnect = autoconnect;
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_unref(reply);
    }

    int n = g_saved.count < max ? g_saved.count : max;
    if (n > 0 && out)
        memcpy(out, g_saved.items, (size_t)n * sizeof(*out));
    return n;
}

bool dc_net_saved_forget(const char *path)
{
    if (!path || !path[0] || !g_net_bus)
        return false;

    if (net_dryrun()) {
        dc_info("net: [DANKC_NET_DRYRUN] would call %s %s %s.Delete()", DC_NM_DEST, path,
                DC_NM_SETTINGS_CONN_IFACE);
        return true;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_net_bus, DC_NM_DEST, path, DC_NM_SETTINGS_CONN_IFACE, "Delete",
                               &err, &reply, "");
    if (r < 0)
        dc_warn("net: Settings.Connection.Delete failed for %s: %s", path,
                err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return r >= 0;
}

bool dc_net_saved_set_autoconnect(const char *path, bool enable)
{
    if (!path || !path[0] || !g_net_bus)
        return false;

    if (net_dryrun()) {
        dc_info("net: [DANKC_NET_DRYRUN] would call %s %s %s.Update() with "
                "connection.autoconnect=%s (all other settings preserved verbatim)",
                DC_NM_DEST, path, DC_NM_SETTINGS_CONN_IFACE, enable ? "true" : "false");
        return true;
    }

    sd_bus_error gerr = SD_BUS_ERROR_NULL;
    sd_bus_message *greply = NULL;
    int r = sd_bus_call_method(g_net_bus, DC_NM_DEST, path, DC_NM_SETTINGS_CONN_IFACE, "GetSettings",
                               &gerr, &greply, "");
    sd_bus_error_free(&gerr);
    if (r < 0)
        return false;

    sd_bus_message *m = NULL;
    r = sd_bus_message_new_method_call(g_net_bus, &m, DC_NM_DEST, path, DC_NM_SETTINGS_CONN_IFACE,
                                       "Update");
    if (r < 0) {
        sd_bus_message_unref(greply);
        return false;
    }

    bool ok = true;
    if (sd_bus_message_enter_container(greply, 'a', "{sa{sv}}") > 0) {
        sd_bus_message_open_container(m, 'a', "{sa{sv}}");

        while (sd_bus_message_enter_container(greply, 'e', "sa{sv}") > 0) {
            const char *group = NULL;
            sd_bus_message_read_basic(greply, 's', &group);
            bool is_conn = group && strcmp(group, "connection") == 0;

            sd_bus_message_open_container(m, 'e', "sa{sv}");
            sd_bus_message_append(m, "s", group);
            sd_bus_message_open_container(m, 'a', "{sv}");

            bool saw_autoconnect = false;
            if (sd_bus_message_enter_container(greply, 'a', "{sv}") > 0) {
                while (sd_bus_message_enter_container(greply, 'e', "sv") > 0) {
                    const char *key = NULL;
                    sd_bus_message_read_basic(greply, 's', &key);
                    if (is_conn && key && strcmp(key, "autoconnect") == 0) {
                        saw_autoconnect = true;
                        sd_bus_message_skip(greply, "v"); /* discard the old value */
                        sd_bus_message_open_container(m, 'e', "sv");
                        sd_bus_message_append(m, "s", "autoconnect");
                        sd_bus_message_open_container(m, 'v', "b");
                        sd_bus_message_append(m, "b", enable ? 1 : 0);
                        sd_bus_message_close_container(m);
                        sd_bus_message_close_container(m);
                    } else {
                        sd_bus_message_open_container(m, 'e', "sv");
                        sd_bus_message_append(m, "s", key);
                        if (net_copy_value(m, greply) < 0)
                            ok = false;
                        sd_bus_message_close_container(m);
                    }
                    sd_bus_message_exit_container(greply); /* e sv */
                }
                sd_bus_message_exit_container(greply); /* a{sv} */
            }

            /* NM omits the "autoconnect" key entirely when it's at its
             * default (true) -- add it explicitly if this connection didn't
             * have it and it's being set to something (still just `enable`,
             * writing true here is a no-op vs. NM's own default but keeps
             * the round trip simple and explicit). */
            if (is_conn && !saw_autoconnect) {
                sd_bus_message_open_container(m, 'e', "sv");
                sd_bus_message_append(m, "s", "autoconnect");
                sd_bus_message_open_container(m, 'v', "b");
                sd_bus_message_append(m, "b", enable ? 1 : 0);
                sd_bus_message_close_container(m);
                sd_bus_message_close_container(m);
            }

            sd_bus_message_close_container(m); /* a{sv} */
            sd_bus_message_close_container(m); /* e sa{sv} */
            sd_bus_message_exit_container(greply); /* e sa{sv} */
        }

        sd_bus_message_close_container(m); /* a{sa{sv}} */
        sd_bus_message_exit_container(greply); /* a{sa{sv}} */
    }
    sd_bus_message_unref(greply);

    if (!ok) {
        sd_bus_message_unref(m);
        return false;
    }

    sd_bus_error uerr = SD_BUS_ERROR_NULL;
    sd_bus_message *ureply = NULL;
    r = sd_bus_call(g_net_bus, m, 0, &uerr, &ureply);
    sd_bus_message_unref(m);
    if (r < 0)
        dc_warn("net: Settings.Connection.Update (autoconnect) failed for %s: %s", path,
                uerr.message ? uerr.message : strerror(-r));
    sd_bus_error_free(&uerr);
    sd_bus_message_unref(ureply);
    return r >= 0;
}

const dc_net_saved_net *dc_net_saved_find_by_ssid(const char *ssid)
{
    if (!ssid || !ssid[0])
        return NULL;
    for (int i = 0; i < g_saved.count; i++)
        if (strcmp(g_saved.items[i].ssid, ssid) == 0)
            return &g_saved.items[i];
    return NULL;
}

/* --- Hotspot (Wi-Fi AP mode) — NetworkManager D-Bus (docs/18-WIFI-BT-PLAN.md
 * sec.2.2) ---------------------------------------------------------------- */

/* Look up the Wi-Fi device's currently-active connection object path
 * (Device.ActiveConnection, "/" or unset if none). Shared by
 * dc_net_hotspot_stop()/dc_net_hotspot_active(); the ethernet equivalent of
 * this same read lives inline in dc_net_eth_disconnect() above (small enough
 * not to be worth sharing across the two very different device kinds). */
static bool nm_active_connection_path(const char *device_path, char *out, size_t out_sz)
{
    out[0] = '\0';
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    if (sd_bus_get_property(g_net_bus, DC_NM_DEST, device_path, DC_NM_DEVICE_IFACE,
                            "ActiveConnection", &err, &reply, "o") >= 0) {
        const char *ac = NULL;
        sd_bus_message_read_basic(reply, 'o', &ac);
        if (ac && ac[0] && strcmp(ac, "/") != 0)
            snprintf(out, out_sz, "%s", ac);
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return out[0] != '\0';
}

/* Read Connection.Active's "Connection" property (the Settings/Connection
 * object path backing this active connection) -- needed to Delete() the
 * ephemeral hotspot connection once it's deactivated. */
static bool nm_active_connection_settings_path(const char *active_path, char *out, size_t out_sz)
{
    out[0] = '\0';
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    if (sd_bus_get_property(g_net_bus, DC_NM_DEST, active_path, DC_NM_ACTIVE_CONN_IFACE,
                            "Connection", &err, &reply, "o") >= 0) {
        const char *conn = NULL;
        sd_bus_message_read_basic(reply, 'o', &conn);
        if (conn)
            snprintf(out, out_sz, "%s", conn);
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return out[0] != '\0';
}

bool dc_net_hotspot_active(char *ssid_out, size_t len)
{
    if (ssid_out && len)
        ssid_out[0] = '\0';
    if (!g_net_bus || !g_nm.have_wifi_device)
        return false;

    uint32_t mode = 0;
    sd_bus_error merr = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property_trivial(g_net_bus, DC_NM_DEST, g_nm.device_path,
                                        DC_NM_WIRELESS_IFACE, "Mode", &merr, 'u', &mode);
    sd_bus_error_free(&merr);
    if (r < 0 || mode != NM_802_11_MODE_AP)
        return false;

    if (!ssid_out || !len)
        return true;

    /* No "current AP" object exists in AP mode the way there is for a
     * client connection -- decode the SSID off the active connection's own
     * saved 802-11-wireless.ssid via Settings.Connection.GetSettings()
     * instead (net_conn_read_settings(), defined in the saved-networks
     * section below). */
    char active_path[128];
    if (!nm_active_connection_path(g_nm.device_path, active_path, sizeof(active_path)))
        return true; /* AP mode confirmed, just no active-connection object to read */

    char settings_path[128];
    if (!nm_active_connection_settings_path(active_path, settings_path, sizeof(settings_path)))
        return true;

    net_conn_read_settings(settings_path, NULL, 0, NULL, 0, NULL, ssid_out, len);
    return true;
}

dc_net_hotspot_start_result dc_net_hotspot_start(const char *ssid, const char *password,
                                                 const char *band)
{
    if (!ssid || !ssid[0])
        return DC_NET_HOTSPOT_START_DBUS_FAILED;
    if (!g_net_bus || !g_nm.have_wifi_device)
        return DC_NET_HOTSPOT_START_NO_DEVICE;

    /* g_nm.connected reflects whether the Wi-Fi device is currently a
     * connected client (tracked by the PropertiesChanged subscription at the
     * top of this file) -- see the "IMPORTANT" note in net.h. */
    bool was_connected = g_nm.connected;

    if (net_dryrun()) {
        dc_info("net: [DANKC_NET_DRYRUN] would call %s %s %s.AddAndActivateConnection(...) on "
                "device %s:",
                DC_NM_DEST, DC_NM_PATH, DC_NM_DEST, g_nm.device_path);
        dc_info("net: [DANKC_NET_DRYRUN]   connection: {id=\"%s\", type=802-11-wireless, "
                "autoconnect=false}",
                ssid);
        if (band && band[0])
            dc_info("net: [DANKC_NET_DRYRUN]   802-11-wireless: {mode=ap, ssid=\"%s\", band=%s%s}",
                    ssid, band, password && password[0] ? ", security=802-11-wireless-security" : "");
        else
            dc_info("net: [DANKC_NET_DRYRUN]   802-11-wireless: {mode=ap, ssid=\"%s\"%s}", ssid,
                    password && password[0] ? ", security=802-11-wireless-security" : "");
        if (password && password[0])
            dc_info("net: [DANKC_NET_DRYRUN]   802-11-wireless-security: {key-mgmt=wpa-psk, "
                    "psk=\"%s\"}",
                    password);
        else
            dc_info("net: [DANKC_NET_DRYRUN]   (open hotspot -- no 802-11-wireless-security group)");
        dc_info("net: [DANKC_NET_DRYRUN]   ipv4: {method=shared}");
        if (was_connected)
            dc_warn("net: [DANKC_NET_DRYRUN] wifi device %s is currently a connected client "
                    "(ssid \"%s\") -- starting the hotspot would drop that connection",
                    g_nm.device_path, g_nm.ssid);
        return was_connected ? DC_NET_HOTSPOT_START_WAS_CONNECTED : DC_NET_HOTSPOT_START_OK;
    }

    if (was_connected)
        dc_warn("net: starting hotspot on %s will drop its active client connection (ssid \"%s\")",
                g_nm.device_path, g_nm.ssid);

    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(g_net_bus, &m, DC_NM_DEST, DC_NM_PATH, DC_NM_DEST,
                                           "AddAndActivateConnection");
    if (r < 0)
        return DC_NET_HOTSPOT_START_DBUS_FAILED;

    /* a{sa{sv}} settings dict, built by hand -- same nested-container shape
     * as services/polkit.c's register_agent() (see its header comment). */
    sd_bus_message_open_container(m, 'a', "{sa{sv}}");

    /* "connection": {id, type, autoconnect} */
    sd_bus_message_open_container(m, 'e', "sa{sv}");
    sd_bus_message_append(m, "s", "connection");
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "id");
    sd_bus_message_open_container(m, 'v', "s");
    sd_bus_message_append(m, "s", ssid);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "type");
    sd_bus_message_open_container(m, 'v', "s");
    sd_bus_message_append(m, "s", "802-11-wireless");
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "autoconnect");
    sd_bus_message_open_container(m, 'v', "b");
    sd_bus_message_append(m, "b", 0);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m); /* a{sv} */
    sd_bus_message_close_container(m); /* e */

    /* "802-11-wireless": {mode=ap, ssid, band?, security?} */
    sd_bus_message_open_container(m, 'e', "sa{sv}");
    sd_bus_message_append(m, "s", "802-11-wireless");
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "mode");
    sd_bus_message_open_container(m, 'v', "s");
    sd_bus_message_append(m, "s", "ap");
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "ssid");
    sd_bus_message_open_container(m, 'v', "ay");
    sd_bus_message_append_array(m, 'y', ssid, strlen(ssid));
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    if (band && band[0]) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "band");
        sd_bus_message_open_container(m, 'v', "s");
        sd_bus_message_append(m, "s", band);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
    }
    if (password && password[0]) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "security");
        sd_bus_message_open_container(m, 'v', "s");
        sd_bus_message_append(m, "s", "802-11-wireless-security");
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
    }
    sd_bus_message_close_container(m); /* a{sv} */
    sd_bus_message_close_container(m); /* e */

    /* "802-11-wireless-security": {key-mgmt=wpa-psk, psk} -- omitted entirely
     * for an open hotspot. */
    if (password && password[0]) {
        sd_bus_message_open_container(m, 'e', "sa{sv}");
        sd_bus_message_append(m, "s", "802-11-wireless-security");
        sd_bus_message_open_container(m, 'a', "{sv}");
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "key-mgmt");
        sd_bus_message_open_container(m, 'v', "s");
        sd_bus_message_append(m, "s", "wpa-psk");
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "psk");
        sd_bus_message_open_container(m, 'v', "s");
        sd_bus_message_append(m, "s", password);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m); /* a{sv} */
        sd_bus_message_close_container(m); /* e */
    }

    /* "ipv4": {method=shared} -- NM runs its own DHCP server + NAT for the
     * lifetime of the AP-mode connection. */
    sd_bus_message_open_container(m, 'e', "sa{sv}");
    sd_bus_message_append(m, "s", "ipv4");
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "method");
    sd_bus_message_open_container(m, 'v', "s");
    sd_bus_message_append(m, "s", "shared");
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m); /* a{sv} */
    sd_bus_message_close_container(m); /* e */

    sd_bus_message_close_container(m); /* a{sa{sv}} */

    sd_bus_message_append(m, "oo", g_nm.device_path, "/");

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    r = sd_bus_call(g_net_bus, m, 0, &err, &reply);
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);

    if (r < 0) {
        dc_warn("net: AddAndActivateConnection (hotspot) failed: %s",
                err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return DC_NET_HOTSPOT_START_DBUS_FAILED;
    }
    sd_bus_error_free(&err);

    return was_connected ? DC_NET_HOTSPOT_START_WAS_CONNECTED : DC_NET_HOTSPOT_START_OK;
}

void dc_net_hotspot_stop(void)
{
    if (!g_net_bus || !g_nm.have_wifi_device)
        return;

    char ssid[64] = {0};
    if (!dc_net_hotspot_active(ssid, sizeof(ssid))) {
        dc_info("net: dc_net_hotspot_stop() called but no hotspot is active");
        return;
    }

    char active_path[128];
    if (!nm_active_connection_path(g_nm.device_path, active_path, sizeof(active_path)))
        return;

    char settings_path[128];
    bool have_settings = nm_active_connection_settings_path(active_path, settings_path,
                                                            sizeof(settings_path));

    if (net_dryrun()) {
        dc_info("net: [DANKC_NET_DRYRUN] would call %s %s %s.DeactivateConnection(\"%s\")",
                DC_NM_DEST, DC_NM_PATH, DC_NM_DEST, active_path);
        if (have_settings)
            dc_info("net: [DANKC_NET_DRYRUN] would call %s %s %s.Delete() (hotspot \"%s\")",
                    DC_NM_DEST, settings_path, DC_NM_SETTINGS_CONN_IFACE, ssid);
        return;
    }

    sd_bus_error derr = SD_BUS_ERROR_NULL;
    sd_bus_message *dreply = NULL;
    int r = sd_bus_call_method(g_net_bus, DC_NM_DEST, DC_NM_PATH, DC_NM_DEST,
                               "DeactivateConnection", &derr, &dreply, "o", active_path);
    if (r < 0)
        dc_warn("net: DeactivateConnection (hotspot) failed: %s",
                derr.message ? derr.message : strerror(-r));
    sd_bus_error_free(&derr);
    sd_bus_message_unref(dreply);

    if (have_settings) {
        sd_bus_error delerr = SD_BUS_ERROR_NULL;
        sd_bus_message *delreply = NULL;
        int dr = sd_bus_call_method(g_net_bus, DC_NM_DEST, settings_path, DC_NM_SETTINGS_CONN_IFACE,
                                    "Delete", &delerr, &delreply, "");
        if (dr < 0)
            dc_warn("net: Settings.Connection.Delete (hotspot) failed: %s",
                    delerr.message ? delerr.message : strerror(-dr));
        sd_bus_error_free(&delerr);
        sd_bus_message_unref(delreply);
    }
}

