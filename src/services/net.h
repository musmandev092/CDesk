/* net.h — Wi-Fi link state: NetworkManager D-Bus (preferred) or sysfs/nmcli
 * (fallback when NM isn't on the bus).
 *
 * docs/15-PERF-PLAN.md T2.2: the bar's SSID+signal readout used to fork
 * `nmcli dev wifi list` on a timer (~20 forks/min at idle). dc_net_init()
 * subscribes to org.freedesktop.NetworkManager PropertiesChanged on the
 * system bus (services/dbus.c) instead, so the cache updates event-driven,
 * off a signal, with zero forks. If the system bus or NetworkManager itself
 * is unavailable, dc_net_wifi() transparently falls back to the original
 * nmcli-popen path (still forks, but only then).
 */
#ifndef DC_SERVICES_NET_H
#define DC_SERVICES_NET_H

#include <stdbool.h>
#include <stddef.h>

struct dc_dbus;

typedef struct dc_net_info {
    bool has_wifi;   /* a wl* interface exists */
    bool connected;  /* it is operationally up */

    /* Only meaningful when `connected`. Sourced from NetworkManager's D-Bus
     * cache (dc_net_init()) when available; falls back to a briefly-cached
     * `nmcli` popen() otherwise. `ssid` is empty and `signal_percent` is -1
     * if neither source has anything to report. */
    char ssid[64];
    int signal_percent; /* 0-100, or -1 if unknown */
} dc_net_info;

/* Bind the system bus and subscribe to NetworkManager's Wi-Fi device +
 * active-access-point PropertiesChanged signals (event-driven SSID/signal
 * cache -- no forks). Call once at startup, same convention as
 * dc_bluez_init()/dc_power_init(). Safe no-op (dc_net_wifi() falls back to
 * nmcli) if `dbus` is NULL, the system bus is unavailable, or no
 * NetworkManager Wi-Fi device is found. */
void dc_net_init(struct dc_dbus *dbus);

/* Fill `out` from /sys/class/net. Returns true if a Wi-Fi interface exists. */
bool dc_net_wifi(dc_net_info *out);

/* --- Wi-Fi scan list (control-center expandable network section) --------
 *
 * `nmcli dev wifi list` can block for well over a second (it triggers a
 * fresh scan), so this follows services/weather.c's async fork+pipe+
 * non-blocking-read pattern instead of net.c's own dc_net_wifi()'s plain
 * popen() (that call only ever reads one already-buffered line from a
 * `--rescan no` query, cheap enough to stay synchronous). */
#define DC_NET_SCAN_MAX 12

typedef struct dc_net_wifi_ap {
    char ssid[64];
    int signal_percent; /* 0-100 */
    bool secured;        /* SECURITY column non-empty */
    bool in_use;         /* the currently-connected AP */
    bool known;          /* a saved NetworkManager connection matches this SSID */
} dc_net_wifi_ap;

/* Copy up to `max` cached scan results into `out` (freshest completed scan;
 * empty until the first one finishes). Kicks off a new async scan if none is
 * in flight and the refresh interval has elapsed, and drains an in-flight
 * scan's pipe (non-blocking) -- safe to call every render frame while the
 * network section is expanded, same contract as dc_weather_get(). Returns
 * the number of APs written to `out`. */
int dc_net_wifi_scan(dc_net_wifi_ap *out, int max);

/* Connect to `ssid`, fire-and-forget (`nmcli dev wifi connect`, detached --
 * relies on NetworkManager reusing a saved connection's credentials for
 * already-known SSIDs). Only meaningful for open networks or ones with
 * `known` set -- a secured+unknown SSID needs a password, see
 * dc_net_wifi_connect_psk() below (docs/13-POPOUTS-SPEC.md sec.1). */
void dc_net_wifi_connect(const char *ssid);

/* --- Wi-Fi password entry (control-center inline password field, W1.1) --
 *
 * Unlike dc_net_wifi_connect() above, connecting with a fresh password needs
 * to report back success/failure (wrong password should re-prompt inline
 * instead of silently failing), so this follows the same async fork+pipe+
 * non-blocking-drain shape as the scan list -- one nmcli child at a time,
 * its combined stdout+stderr captured and inspected once it exits (SIGCHLD is
 * SIG_IGN process-wide, so there is no exit status to waitpid() on; nmcli's
 * own "Error: ..." text on failure is the only signal available, same as a
 * user watching a terminal would use). */
typedef enum {
    DC_NET_CONNECT_IDLE = 0,
    DC_NET_CONNECT_IN_PROGRESS,
    DC_NET_CONNECT_SUCCESS,
    DC_NET_CONNECT_FAILED,
} dc_net_connect_state;

/* Start connecting to `ssid` with `psk` (`nmcli dev wifi connect <ssid>
 * password <psk>`), replacing any previous in-flight connect job. Async --
 * call dc_net_wifi_connect_poll() to progress/observe it.
 *
 * If the environment variable DANKC_WIFI_DRYRUN is set (any value), no
 * `nmcli` process is spawned: the exact command line that would have run is
 * logged via dc_info() instead, and the job resolves to
 * DC_NET_CONNECT_SUCCESS on its own after a short simulated delay -- lets the
 * whole password-entry -> connecting -> connected UI flow be exercised and
 * screenshotted without touching the user's real network. */
void dc_net_wifi_connect_psk(const char *ssid, const char *psk);

/* Progress and report the in-flight connect job (non-blocking -- safe to
 * call every render frame while a password prompt/"Connecting..." row is
 * shown, same contract as dc_net_wifi_scan()). On DC_NET_CONNECT_FAILED,
 * `err` (if non-NULL) is filled with a short human-readable reason; left
 * untouched otherwise. A successful connect also nudges the scan list to
 * refresh soon so the row flips to "Connected" without waiting a full
 * DC_NET_SCAN_REFRESH_SEC. */
dc_net_connect_state dc_net_wifi_connect_poll(char *err, size_t err_sz);

/* Abort any in-flight connect job and reset to DC_NET_CONNECT_IDLE (killing
 * the nmcli child if one is running). Called after a terminal state
 * (success/failure) has been consumed, or when the user cancels the
 * password prompt. */
void dc_net_wifi_connect_reset(void);

#endif /* DC_SERVICES_NET_H */
