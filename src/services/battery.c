#include "services/battery.h"

#include "core/log.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DC_POWER_SUPPLY_DIR "/sys/class/power_supply"

static bool read_line(const char *path, char *buf, size_t cap)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return false;
    bool ok = fgets(buf, (int)cap, file) != NULL;
    fclose(file);
    if (!ok)
        return false;
    buf[strcspn(buf, "\n")] = '\0';
    return true;
}

/* Read a sysfs attribute as a long. Returns -1 if absent/unparseable (every
 * value this is used for -- energy/charge/voltage -- is non-negative, so -1
 * is an unambiguous "missing" sentinel). */
static long read_attr(const char *dir_name, const char *attr)
{
    char path[512];
    char value[32];
    snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/%s", dir_name, attr);
    if (!read_line(path, value, sizeof(value)))
        return -1;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    return (end != value && v >= 0) ? v : -1;
}

/* Any Mains/USB power-supply reporting online=1 (docs/12-BAR-SPEC.md sec.4/6
 * battery item 5) -- hoisted from bar.c's former bar_ac_online() so every
 * caller of dc_battery_read gets a correct ac_online for free. Best-effort:
 * false if no supply is found or /sys/class/power_supply is unreadable
 * (never treated as fatal). */
static bool scan_ac_online(void)
{
    DIR *dir = opendir(DC_POWER_SUPPLY_DIR);
    if (!dir)
        return false;

    bool online = false;
    struct dirent *entry;
    char path[512], value[32];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/type", entry->d_name);
        if (!read_line(path, value, sizeof(value)))
            continue;
        if (strcmp(value, "Mains") != 0 && strcmp(value, "USB") != 0)
            continue;

        snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/online", entry->d_name);
        if (read_line(path, value, sizeof(value)) && value[0] == '1')
            online = true;
    }
    closedir(dir);
    return online;
}

bool dc_battery_read(dc_battery_info *out)
{
    memset(out, 0, sizeof(*out));
    out->energy_full_wh = -1.0;
    out->energy_full_design_wh = -1.0;
    out->health_percent = -1;
    out->charge_limit = -1;

    DIR *dir = opendir(DC_POWER_SUPPLY_DIR);
    if (!dir)
        return false;

    bool found = false;
    struct dirent *entry;
    char path[512];
    char value[32];

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/type", entry->d_name);
        if (!read_line(path, value, sizeof(value)) || strcmp(value, "Battery") != 0)
            continue;

        snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/capacity", entry->d_name);
        if (!read_line(path, value, sizeof(value)))
            continue;
        out->percent = atoi(value);

        snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/status", entry->d_name);
        if (read_line(path, value, sizeof(value))) {
            out->charging = strcmp(value, "Charging") == 0;
            out->full = strcmp(value, "Full") == 0;
        }
        out->present = true;
        found = true;
        snprintf(out->batt_dir, sizeof(out->batt_dir), "%s", entry->d_name);

        /* Charge-limit support (docs/24-BATTERY-POWER-PLAN.md T1): not every
         * driver exposes charge_control_end_threshold (ThinkPad/ASUS/LG are
         * EC-dependent); treat anything outside 1-100 as unsupported rather
         * than trusting a stray 0/garbage value. */
        long charge_limit = read_attr(entry->d_name, "charge_control_end_threshold");
        if (charge_limit >= 1 && charge_limit <= 100) {
            out->charge_limit_supported = true;
            out->charge_limit = (int)charge_limit;
        } else {
            out->charge_limit_supported = false;
            out->charge_limit = -1;
        }

        /* Battery popout "Health"/"Capacity" cards (docs/13-POPOUTS-SPEC.md
         * sec.2): energy_full[_design] (µWh) preferred, charge_full[_design]
         * (µAh) * voltage_now (µV) as a fallback -- some drivers (notably
         * some ThinkPads/older EC firmware) only expose the charge_* family. */
        long energy_full = read_attr(entry->d_name, "energy_full");
        long energy_full_design = read_attr(entry->d_name, "energy_full_design");
        long charge_full = read_attr(entry->d_name, "charge_full");
        long charge_full_design = read_attr(entry->d_name, "charge_full_design");
        long voltage_now = read_attr(entry->d_name, "voltage_now");

        if (energy_full > 0)
            out->energy_full_wh = (double)energy_full / 1e6;
        else if (charge_full > 0 && voltage_now > 0)
            out->energy_full_wh = (double)charge_full * (double)voltage_now / 1e12;

        if (energy_full_design > 0)
            out->energy_full_design_wh = (double)energy_full_design / 1e6;
        else if (charge_full_design > 0 && voltage_now > 0)
            out->energy_full_design_wh = (double)charge_full_design * (double)voltage_now / 1e12;

        if (energy_full > 0 && energy_full_design > 0)
            out->health_percent =
                (int)((double)energy_full / (double)energy_full_design * 100.0 + 0.5);
        else if (charge_full > 0 && charge_full_design > 0)
            out->health_percent =
                (int)((double)charge_full / (double)charge_full_design * 100.0 + 0.5);

        break;
    }

    closedir(dir);

    out->ac_online = scan_ac_online();

    if (found) {
        /* UI RESCALE (docs/24-BATTERY-POWER-PLAN.md T1 key decisions): keep
         * the raw sysfs capacity in percent_raw for automation (low/critical
         * thresholds must fire off the real capacity, not a rescaled one),
         * but rescale `percent` against the charge limit so "100%" is
         * reachable under a limit instead of topping out at e.g. 80%. Only
         * rescale for limits in the sane 50-99 range -- outside that a driver
         * quirk (e.g. reporting 0 or 100) should not distort the displayed
         * percent. */
        out->percent_raw = out->percent;
        if (out->charge_limit_supported && out->charge_limit >= 50 && out->charge_limit <= 99) {
            int limit = out->charge_limit;
            int rescaled = (out->percent_raw * 100 + limit / 2) / limit;
            out->percent = rescaled > 100 ? 100 : rescaled;
        }

        /* Once the raw capacity reaches the configured limit, the driver
         * should have stopped charging there -- treat that as "full" the
         * same way status=="Full" would at an unlimited 100%, so the UI
         * shows a full/green glyph instead of a stalled "Discharging" one.
         * Skip this if sysfs still reports actively "Charging" below 100%
         * raw (some drivers report one last tick before the status flips to
         * "Not charging"), so we don't flash full->charging->full. */
        if (out->charge_limit_supported && out->percent_raw >= out->charge_limit &&
            !(out->charging && out->percent_raw < 100))
            out->full = true;
    }

    /* DANKC_FAKE_BATTERY=<percent> (debug-only, env-gated -- same convention
     * as DANKC_MARQUEE_TEST): override the real percent so the bar/popout
     * battery glyph's per-level tiering can be screenshotted at any level
     * without needing to actually run the laptop down. DANKC_FAKE_BATTERY_
     * CHARGING=1 additionally fakes the charging flag. DANKC_FAKE_BATTERY_
     * AC=0|1 fakes ac_online (so battery_auto.c's AC-edge automation can be
     * exercised without physically plugging/unplugging). No-op when unset. */
    if (found) {
        const char *fake = getenv("DANKC_FAKE_BATTERY");
        if (fake) {
            int v = atoi(fake);
            v = v < 0 ? 0 : (v > 100 ? 100 : v);
            out->percent = v;
            out->percent_raw = v;
            out->full = out->percent >= 100;
        }
        const char *fake_chg = getenv("DANKC_FAKE_BATTERY_CHARGING");
        if (fake_chg)
            out->charging = atoi(fake_chg) != 0;
    }
    const char *fake_ac = getenv("DANKC_FAKE_BATTERY_AC");
    if (fake_ac)
        out->ac_online = atoi(fake_ac) != 0;

    return found;
}

/* --- write: charge_control_end_threshold via pkexec ------------------------
 *
 * See battery.h's doc-comment for the fire-and-forget/no-success-signal/
 * DANKC_BATTERY_DRYRUN contract. Shape mirrors logind.c's
 * dc_logind_conf_write_dropin() (fork + execlp("pkexec", ...)), but the
 * privileged command here is `pkexec tee <path>` reading the value off a
 * pipe on its stdin, not `pkexec install <tmpfile> <dest>` -- sysfs
 * attributes are magic files (0 size, single value) rather than regular
 * files a copy-based tool can stage/rename into place, and argv-exec into
 * tee sidesteps any shell-quoting concerns entirely (no `sh -c` involved).
 */
static bool battery_dryrun_enabled(void)
{
    const char *v = getenv("DANKC_BATTERY_DRYRUN");
    return v && v[0] == '1';
}

bool dc_battery_set_charge_limit(const char *batt_dir, int pct)
{
    if (pct < 50 || pct > 100)
        return false;
    if (!batt_dir || !batt_dir[0] || strchr(batt_dir, '/') != NULL)
        return false;

    char path[512];
    snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/charge_control_end_threshold",
            batt_dir);

    if (battery_dryrun_enabled()) {
        dc_info("[DRYRUN] battery: would write %d to %s", pct, path);
        return true;
    }

    int fds[2];
    if (pipe(fds) < 0) {
        dc_warn("battery: pipe() failed, cannot set charge limit");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        dc_warn("battery: fork() failed, cannot set charge limit");
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    if (pid == 0) { /* child: pkexec tee <path>, value arrives on its stdin */
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        setsid();
        execlp("pkexec", "pkexec", "tee", path, (char *)NULL);
        _exit(127);
    }

    /* Parent: fire-and-forget -- write "%d\n" and close our end so tee sees
     * EOF and exits after writing once; the polkit prompt (answered by
     * dankc's own agent, see polkit.c) and the actual privileged write run
     * fully detached. Reaped by main's SIGCHLD = SIG_IGN, so there is no
     * waitpid() here to learn the outcome -- see battery.h. */
    close(fds[0]);
    char value[16];
    int len = snprintf(value, sizeof(value), "%d\n", pct);
    ssize_t written = write(fds[1], value, (size_t)len);
    (void)written; /* best-effort; nothing useful to do if the pipe write is short/fails */
    close(fds[1]);

    dc_info("battery: requested charge limit %d on %s (via pkexec tee)", pct, path);
    return true;
}
