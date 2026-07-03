#include "services/power.h"

#include "core/log.h"
#include "services/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DC_POWER_REFRESH_SECS 5
#define DC_POWER_NAME_LEN 64
#define DC_POWER_MAX_PROFILES 96

#define DC_UPOWER_PP_SERVICE "org.freedesktop.UPower.PowerProfiles"
#define DC_UPOWER_PP_PATH "/org/freedesktop/UPower/PowerProfiles"
#define DC_UPOWER_PP_IFACE "org.freedesktop.UPower.PowerProfiles"

#define DC_TUNED_SERVICE "com.redhat.tuned"
#define DC_TUNED_PATH "/Tuned"
#define DC_TUNED_IFACE "com.redhat.tuned.control"

typedef struct {
    char names[DC_POWER_MAX_PROFILES][DC_POWER_NAME_LEN];
    int count;
} profile_list;

static sd_bus *g_system = NULL;
static bool g_backend_probed = false;
static dc_power_backend g_backend = DC_POWER_BACKEND_NONE;

static dc_power_info g_cache;
static time_t g_last_refresh = 0;

/* Only meaningful for the tuned backends -- the real profile names tuned
 * offers, used by pick_profile_for_mode() to resolve a mode to a profile
 * when setting. */
static profile_list g_tuned_profiles;

/* Run a shell command detached (children auto-reaped via SIG_IGN on SIGCHLD,
 * set in main.c) -- same pattern as battery_popout.c/controlcenter.c's
 * run_detached(). */
static void run_detached(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* --- backend detection ---------------------------------------------------- */

static bool bus_name_has_owner(sd_bus *bus, const char *name)
{
    if (!bus)
        return false;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "NameHasOwner", &err, &reply, "s", name);
    bool has = false;
    if (r >= 0) {
        int b = 0;
        sd_bus_message_read_basic(reply, 'b', &b);
        has = b != 0;
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return has;
}

static const char *backend_name(dc_power_backend b)
{
    switch (b) {
    case DC_POWER_BACKEND_PPD:
        return "power-profiles-daemon (UPower.PowerProfiles)";
    case DC_POWER_BACKEND_TUNED_DBUS:
        return "tuned (com.redhat.tuned)";
    case DC_POWER_BACKEND_TUNED_CLI:
        return "tuned-adm CLI";
    default:
        return "none";
    }
}

static void probe_backend(void)
{
    g_backend_probed = true;

    if (bus_name_has_owner(g_system, DC_UPOWER_PP_SERVICE)) {
        g_backend = DC_POWER_BACKEND_PPD;
    } else if (bus_name_has_owner(g_system, DC_TUNED_SERVICE)) {
        g_backend = DC_POWER_BACKEND_TUNED_DBUS;
    } else if (system("command -v tuned-adm >/dev/null 2>&1") == 0) {
        g_backend = DC_POWER_BACKEND_TUNED_CLI;
    } else {
        g_backend = DC_POWER_BACKEND_NONE;
    }
    dc_info("power: backend = %s", backend_name(g_backend));
}

void dc_power_init(struct dc_dbus *dbus)
{
    g_system = dbus ? dbus->system : NULL;
}

/* --- mode <-> slug mapping ------------------------------------------------- */

static dc_power_mode ppd_slug_to_mode(const char *slug)
{
    if (!slug)
        return DC_POWER_MODE_UNKNOWN;
    if (strcmp(slug, "power-saver") == 0)
        return DC_POWER_MODE_POWER_SAVER;
    if (strcmp(slug, "balanced") == 0)
        return DC_POWER_MODE_BALANCED;
    if (strcmp(slug, "performance") == 0)
        return DC_POWER_MODE_PERFORMANCE;
    return DC_POWER_MODE_UNKNOWN;
}

/* Mode mapping for tuned profile names (task spec): performance -> exact
 * "throughput-performance" else any name containing "performance";
 * power-saver -> exact "powersave" else any name containing "powersave"
 * (covers "laptop-battery-powersave"); balanced -> exact "balanced" else any
 * name containing "balanced" (covers "balanced-battery"). */
static dc_power_mode tuned_slug_to_mode(const char *slug)
{
    if (!slug || !*slug)
        return DC_POWER_MODE_UNKNOWN;
    if (strstr(slug, "performance"))
        return DC_POWER_MODE_PERFORMANCE;
    if (strstr(slug, "powersave"))
        return DC_POWER_MODE_POWER_SAVER;
    if (strstr(slug, "balanced"))
        return DC_POWER_MODE_BALANCED;
    return DC_POWER_MODE_UNKNOWN;
}

static const char *pick_profile_for_mode(const profile_list *list, dc_power_mode mode)
{
    static const char *const preferred[3] = {"powersave", "balanced", "throughput-performance"};
    static const char *const contains[3] = {"powersave", "balanced", "performance"};

    if (mode < DC_POWER_MODE_POWER_SAVER || mode > DC_POWER_MODE_PERFORMANCE)
        return NULL;

    for (int i = 0; i < list->count; i++)
        if (strcmp(list->names[i], preferred[mode]) == 0)
            return list->names[i];
    for (int i = 0; i < list->count; i++)
        if (strstr(list->names[i], contains[mode]))
            return list->names[i];
    return NULL;
}

/* --- backend: power-profiles-daemon / UPower.PowerProfiles ---------------- */

static bool read_ppd(dc_power_info *info)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_system, DC_UPOWER_PP_SERVICE, DC_UPOWER_PP_PATH,
                               "org.freedesktop.DBus.Properties", "GetAll", &err, &reply, "s",
                               DC_UPOWER_PP_IFACE);
    if (r < 0) {
        sd_bus_error_free(&err);
        return false;
    }
    info->available = true;

    if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0)
        goto done;

    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *prop = NULL;
        sd_bus_message_read_basic(reply, 's', &prop);

        if (prop && strcmp(prop, "ActiveProfile") == 0) {
            sd_bus_message_enter_container(reply, 'v', "s");
            const char *val = NULL;
            sd_bus_message_read_basic(reply, 's', &val);
            if (val)
                snprintf(info->active_profile, sizeof(info->active_profile), "%s", val);
            sd_bus_message_exit_container(reply); /* v */
        } else if (prop && strcmp(prop, "Profiles") == 0) {
            sd_bus_message_enter_container(reply, 'v', "aa{sv}");
            sd_bus_message_enter_container(reply, 'a', "a{sv}");
            while (sd_bus_message_enter_container(reply, 'a', "{sv}") > 0) {
                while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                    const char *k = NULL;
                    sd_bus_message_read_basic(reply, 's', &k);
                    if (k && strcmp(k, "Profile") == 0) {
                        sd_bus_message_enter_container(reply, 'v', "s");
                        const char *pv = NULL;
                        sd_bus_message_read_basic(reply, 's', &pv);
                        if (pv && strcmp(pv, "performance") == 0)
                            info->has_performance_mode = true;
                        sd_bus_message_exit_container(reply); /* v */
                    } else {
                        sd_bus_message_skip(reply, "v");
                    }
                    sd_bus_message_exit_container(reply); /* sv */
                }
                sd_bus_message_exit_container(reply); /* {sv} */
            }
            sd_bus_message_exit_container(reply); /* a{sv} */
            sd_bus_message_exit_container(reply); /* v */
        } else {
            sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply); /* sv */
    }
    sd_bus_message_exit_container(reply); /* a{sv} */

done:
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    info->active_mode = ppd_slug_to_mode(info->active_profile);
    return true;
}

static bool set_ppd(dc_power_mode mode)
{
    static const char *const slugs[3] = {"power-saver", "balanced", "performance"};
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_set_property(g_system, DC_UPOWER_PP_SERVICE, DC_UPOWER_PP_PATH,
                                DC_UPOWER_PP_IFACE, "ActiveProfile", &err, "s", slugs[mode]);
    if (r < 0)
        dc_warn("power: PowerProfiles set failed: %s", err.message ? err.message : "?");
    sd_bus_error_free(&err);
    return r >= 0;
}

/* --- backend: tuned, direct D-Bus (com.redhat.tuned) ----------------------- */

static bool tuned_dbus_active(char *out, size_t n)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_system, DC_TUNED_SERVICE, DC_TUNED_PATH, DC_TUNED_IFACE,
                               "active_profile", &err, &reply, "");
    bool ok = false;
    if (r >= 0) {
        const char *val = NULL;
        if (sd_bus_message_read_basic(reply, 's', &val) >= 0 && val) {
            snprintf(out, n, "%s", val);
            ok = true;
        }
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return ok;
}

static bool tuned_dbus_profiles(profile_list *list)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_system, DC_TUNED_SERVICE, DC_TUNED_PATH, DC_TUNED_IFACE,
                               "profiles", &err, &reply, "");
    if (r < 0) {
        sd_bus_error_free(&err);
        return false;
    }
    list->count = 0;
    if (sd_bus_message_enter_container(reply, 'a', "s") >= 0) {
        const char *name = NULL;
        while (list->count < DC_POWER_MAX_PROFILES &&
              sd_bus_message_read_basic(reply, 's', &name) > 0)
            snprintf(list->names[list->count++], DC_POWER_NAME_LEN, "%s", name);
        sd_bus_message_exit_container(reply);
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return list->count > 0;
}

static bool tuned_dbus_switch(const char *name)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_system, DC_TUNED_SERVICE, DC_TUNED_PATH, DC_TUNED_IFACE,
                               "switch_profile", &err, &reply, "s", name);
    bool ok = false;
    if (r >= 0) {
        int success = 0;
        const char *msg = NULL;
        if (sd_bus_message_read(reply, "(bs)", &success, &msg) >= 0)
            ok = success != 0;
    } else {
        dc_warn("power: tuned switch_profile failed: %s", err.message ? err.message : "?");
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    return ok;
}

/* --- backend: tuned-adm CLI (last resort) ---------------------------------- */

static bool tuned_cli_active(char *out, size_t n)
{
    FILE *pipe = popen("tuned-adm active 2>/dev/null", "r");
    if (!pipe)
        return false;
    bool found = false;
    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        char *pos = strstr(line, "Current active profile:");
        if (!pos)
            continue;
        pos += strlen("Current active profile:");
        while (*pos == ' ')
            pos++;
        pos[strcspn(pos, "\n")] = '\0';
        snprintf(out, n, "%s", pos);
        found = true;
    }
    pclose(pipe);
    return found;
}

static bool tuned_cli_list(profile_list *list)
{
    FILE *pipe = popen("tuned-adm list 2>/dev/null", "r");
    if (!pipe)
        return false;
    list->count = 0;
    char line[256];
    while (list->count < DC_POWER_MAX_PROFILES && fgets(line, sizeof(line), pipe)) {
        char name[DC_POWER_NAME_LEN];
        if (sscanf(line, " - %63s", name) == 1)
            snprintf(list->names[list->count++], DC_POWER_NAME_LEN, "%s", name);
    }
    pclose(pipe);
    return list->count > 0;
}

static void tuned_cli_set(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "tuned-adm profile %s", name);
    run_detached(cmd);
}

/* --- public API -------------------------------------------------------------- */

bool dc_power_read(dc_power_info *out)
{
    if (!g_backend_probed)
        probe_backend();

    time_t now = time(NULL);
    if (g_last_refresh != 0 && now - g_last_refresh < DC_POWER_REFRESH_SECS) {
        *out = g_cache;
        return g_cache.available;
    }
    g_last_refresh = now;

    memset(&g_cache, 0, sizeof(g_cache));
    g_cache.backend = g_backend;
    g_cache.active_mode = DC_POWER_MODE_UNKNOWN;

    switch (g_backend) {
    case DC_POWER_BACKEND_PPD:
        read_ppd(&g_cache);
        break;

    case DC_POWER_BACKEND_TUNED_DBUS: {
        char active[DC_POWER_NAME_LEN] = {0};
        if (tuned_dbus_active(active, sizeof(active))) {
            g_cache.available = true;
            snprintf(g_cache.active_profile, sizeof(g_cache.active_profile), "%s", active);
            g_cache.active_mode = tuned_slug_to_mode(active);
        }
        tuned_dbus_profiles(&g_tuned_profiles);
        g_cache.has_performance_mode =
            pick_profile_for_mode(&g_tuned_profiles, DC_POWER_MODE_PERFORMANCE) != NULL;
        break;
    }

    case DC_POWER_BACKEND_TUNED_CLI: {
        char active[DC_POWER_NAME_LEN] = {0};
        if (tuned_cli_active(active, sizeof(active))) {
            g_cache.available = true;
            snprintf(g_cache.active_profile, sizeof(g_cache.active_profile), "%s", active);
            g_cache.active_mode = tuned_slug_to_mode(active);
        }
        tuned_cli_list(&g_tuned_profiles);
        g_cache.has_performance_mode =
            pick_profile_for_mode(&g_tuned_profiles, DC_POWER_MODE_PERFORMANCE) != NULL;
        break;
    }

    default:
        break;
    }

    *out = g_cache;
    return g_cache.available;
}

bool dc_power_set_mode(dc_power_mode mode)
{
    if (mode < DC_POWER_MODE_POWER_SAVER || mode > DC_POWER_MODE_PERFORMANCE)
        return false;

    /* Callers are expected to dc_power_read() before offering a mode to
     * switch to (that's what populates the UI), but don't assume it: probe
     * lazily here too so dc_power_set_mode() is safe to call standalone. */
    if (!g_backend_probed)
        probe_backend();

    bool ok = false;
    switch (g_backend) {
    case DC_POWER_BACKEND_PPD:
        ok = set_ppd(mode);
        break;

    case DC_POWER_BACKEND_TUNED_DBUS: {
        if (g_tuned_profiles.count == 0)
            tuned_dbus_profiles(&g_tuned_profiles);
        const char *name = pick_profile_for_mode(&g_tuned_profiles, mode);
        if (name)
            ok = tuned_dbus_switch(name);
        break;
    }

    case DC_POWER_BACKEND_TUNED_CLI: {
        if (g_tuned_profiles.count == 0)
            tuned_cli_list(&g_tuned_profiles);
        const char *name = pick_profile_for_mode(&g_tuned_profiles, mode);
        if (name) {
            tuned_cli_set(name);
            ok = true; /* fire-and-forget, like battery_popout.c's set_power_profile() */
        }
        break;
    }

    default:
        break;
    }

    if (ok)
        g_last_refresh = 0; /* force the next read to refresh instead of serving the cache */
    return ok;
}

const char *dc_power_mode_label(dc_power_mode mode)
{
    switch (mode) {
    case DC_POWER_MODE_POWER_SAVER:
        return "Power Saver";
    case DC_POWER_MODE_BALANCED:
        return "Balanced";
    case DC_POWER_MODE_PERFORMANCE:
        return "Performance";
    default:
        return "Unknown";
    }
}
