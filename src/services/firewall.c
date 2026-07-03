/* firewall.c — see firewall.h for the public contract + design summary.
 *
 * Parsing targets:
 *
 *   /etc/ufw/ufw.conf
 *     "ENABLED=yes" | "ENABLED=no"        -- world-readable, no root needed.
 *
 *   /etc/default/ufw
 *     'DEFAULT_INPUT_POLICY="DROP"'  (or ACCEPT/REJECT)
 *     'DEFAULT_OUTPUT_POLICY="ACCEPT"'
 *     'DEFAULT_FORWARD_POLICY="DROP"'     -- also world-readable.
 *
 *   ufw status                            -- needs root; best-effort only.
 *     "Status: active" | "Status: inactive"
 *
 *   firewall-cmd --get-active-zones
 *     "<zone-name>"                       -- first non-indented line.
 */
#include "services/firewall.h"

#include "core/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *const dc_firewall_common_services[DC_FIREWALL_COMMON_SERVICE_COUNT] = {
    "ssh", "http", "https", "samba", "mdns",
};

static bool g_backend_probed = false;
static dc_firewall_backend g_backend = DC_FIREWALL_BACKEND_NONE;

/* --- backend detection ------------------------------------------------- */

static bool cmd_exists(const char *bin)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", bin);
    return system(cmd) == 0;
}

static bool systemctl_is_active(const char *unit)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "systemctl is-active --quiet %s", unit);
    return system(cmd) == 0;
}

const char *dc_firewall_backend_name(dc_firewall_backend b)
{
    switch (b) {
    case DC_FIREWALL_BACKEND_UFW:
        return "ufw";
    case DC_FIREWALL_BACKEND_FIREWALLD:
        return "firewalld";
    default:
        return "none";
    }
}

static void probe_backend(void)
{
    g_backend_probed = true;

    if (systemctl_is_active("firewalld"))
        g_backend = DC_FIREWALL_BACKEND_FIREWALLD;
    else if (cmd_exists("ufw"))
        g_backend = DC_FIREWALL_BACKEND_UFW;
    else if (cmd_exists("firewall-cmd"))
        g_backend = DC_FIREWALL_BACKEND_FIREWALLD; /* installed, not running */
    else
        g_backend = DC_FIREWALL_BACKEND_NONE;

    dc_info("firewall: backend = %s", dc_firewall_backend_name(g_backend));
}

dc_firewall_backend dc_firewall_backend_get(void)
{
    if (!g_backend_probed)
        probe_backend();
    return g_backend;
}

void dc_firewall_debug_force_backend(dc_firewall_backend b)
{
    g_backend_probed = true;
    g_backend = b;
    dc_info("firewall: [DANKC_FIREWALL_TEST] backend force-set to %s", dc_firewall_backend_name(b));
}

/* --- small file-reading helpers ----------------------------------------- */

/* Reads `path` fully; returns false if it can't be opened. Truncates
 * silently at buf_cap-1 (these config files are always tiny). */
static bool read_whole_file(const char *path, char *buf, size_t buf_cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    size_t n = fread(buf, 1, buf_cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return true;
}

/* Finds a "KEY=value" or 'KEY="value"' line (shell-style config file, one
 * assignment per line, '#' comments) and copies the unquoted value into
 * `out`. Returns false if the key isn't found. */
static bool find_kv(const char *text, const char *key, char *out, size_t out_cap)
{
    size_t key_len = strlen(key);
    const char *line = text;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        const char *p = line;
        while (p < line + line_len && isspace((unsigned char)*p))
            p++;

        if ((size_t)(line + line_len - p) > key_len && strncmp(p, key, key_len) == 0 &&
            p[key_len] == '=') {
            const char *v = p + key_len + 1;
            const char *end = line + line_len;
            if (v < end && *v == '"') {
                v++;
                const char *q = memchr(v, '"', (size_t)(end - v));
                end = q ? q : end;
            }
            size_t n = (size_t)(end - v);
            if (n >= out_cap)
                n = out_cap - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }

        line = eol ? eol + 1 : NULL;
    }
    return false;
}

static const char *policy_word(const char *raw)
{
    if (strcasecmp(raw, "ACCEPT") == 0)
        return "allow";
    if (strcasecmp(raw, "DROP") == 0)
        return "deny";
    if (strcasecmp(raw, "REJECT") == 0)
        return "reject";
    return raw;
}

/* --- status: ufw --------------------------------------------------------- */

#define DC_UFW_CONF "/etc/ufw/ufw.conf"
#define DC_UFW_DEFAULTS "/etc/default/ufw"

static void read_ufw_status(dc_firewall_info *info)
{
    char conf[4096];
    if (read_whole_file(DC_UFW_CONF, conf, sizeof(conf))) {
        char enabled[8] = {0};
        if (find_kv(conf, "ENABLED", enabled, sizeof(enabled))) {
            info->enabled_known = true;
            info->enabled = strcasecmp(enabled, "yes") == 0;
        }
    }

    char defaults[4096];
    if (read_whole_file(DC_UFW_DEFAULTS, defaults, sizeof(defaults))) {
        char in_policy[16] = {0}, out_policy[16] = {0}, fwd_policy[16] = {0};
        bool got_in = find_kv(defaults, "DEFAULT_INPUT_POLICY", in_policy, sizeof(in_policy));
        bool got_out = find_kv(defaults, "DEFAULT_OUTPUT_POLICY", out_policy, sizeof(out_policy));
        bool got_fwd = find_kv(defaults, "DEFAULT_FORWARD_POLICY", fwd_policy, sizeof(fwd_policy));
        if (got_in || got_out || got_fwd) {
            snprintf(info->default_policy, sizeof(info->default_policy),
                    "incoming: %s, outgoing: %s, routed: %s",
                    got_in ? policy_word(in_policy) : "unknown",
                    got_out ? policy_word(out_policy) : "unknown",
                    got_fwd ? policy_word(fwd_policy) : "unknown");
        }
    }

    /* Best-effort root upgrade: if dankc happens to be running as root (or
     * `ufw` is somehow setuid on this box), prefer the live "Status: ..."
     * line over the config-file read. A normal unprivileged run just gets
     * "ERROR: You need to be root to run this script" on stderr, which
     * matches no "Status:" line below and is silently ignored -- the
     * file-based read above already populated enabled_known/enabled. */
    FILE *pipe = popen("ufw status 2>/dev/null", "r");
    if (pipe) {
        char line[256];
        while (fgets(line, sizeof(line), pipe)) {
            if (strncmp(line, "Status:", 7) == 0) {
                const char *v = line + 7;
                while (*v == ' ')
                    v++;
                info->enabled_known = true;
                info->enabled = strncmp(v, "active", 6) == 0;
                break;
            }
        }
        pclose(pipe);
    }
}

/* --- status: firewalld ---------------------------------------------------- */

static void read_firewalld_status(dc_firewall_info *info)
{
    /* Detection already required this to be active for us to have picked
     * FIREWALLD via that path, but re-check here too since probe_backend()
     * also falls onto FIREWALLD when only `firewall-cmd` is on PATH with
     * the daemon not running. */
    info->enabled_known = true;
    info->enabled = systemctl_is_active("firewalld");

    if (!info->enabled)
        return;

    FILE *pipe = popen("firewall-cmd --get-active-zones 2>/dev/null", "r");
    if (!pipe)
        return;
    char line[128];
    if (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\n")] = '\0';
        snprintf(info->active_zone, sizeof(info->active_zone), "%s", line);
    }
    pclose(pipe);
}

bool dc_firewall_status(dc_firewall_info *out)
{
    memset(out, 0, sizeof(*out));
    out->backend = dc_firewall_backend_get();
    out->available = out->backend != DC_FIREWALL_BACKEND_NONE;

    switch (out->backend) {
    case DC_FIREWALL_BACKEND_UFW:
        read_ufw_status(out);
        break;
    case DC_FIREWALL_BACKEND_FIREWALLD:
        read_firewalld_status(out);
        break;
    default:
        break;
    }

    dc_info("firewall: status backend=%s enabled=%s (known=%d) zone=\"%s\" policy=\"%s\"",
            dc_firewall_backend_name(out->backend),
            out->enabled_known ? (out->enabled ? "on" : "off") : "unknown", out->enabled_known,
            out->active_zone, out->default_policy);
    return out->available;
}

/* --- writes: pkexec, dry-run gated --------------------------------------- */

static bool dryrun_enabled(void)
{
    const char *v = getenv("DANKC_FIREWALL_DRYRUN");
    return v && v[0] == '1';
}

/* argv must be NULL-terminated; argv[0] is always "pkexec". Fire-and-forget
 * (reaped by main's SIGCHLD=SIG_IGN), same shape as power.c's
 * run_detached() / printers.c's run_cmd() -- except this one always goes
 * through pkexec, whose polkit prompt dankc's own registered agent
 * (services/polkit.c) answers. */
static void run_pkexec(const char *tag, const char *const argv[], int argc)
{
    if (dryrun_enabled()) {
        char line[512];
        int off = snprintf(line, sizeof(line), "[dryrun]");
        for (int i = 0; i < argc && argv[i]; i++) {
            off += snprintf(line + off, off < (int)sizeof(line) ? sizeof(line) - (size_t)off : 0,
                    " %s", argv[i]);
        }
        dc_info("firewall: %s: %s", tag, line);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execvp("pkexec", (char *const *)argv);
        _exit(127);
    }
    /* Parent: fire-and-forget -- the polkit prompt (answered by dankc's own
     * modal) and the actual privileged command run fully detached. */
}

bool dc_firewall_set_enabled(bool enable)
{
    dc_firewall_backend backend = dc_firewall_backend_get();

    switch (backend) {
    case DC_FIREWALL_BACKEND_UFW: {
        const char *argv[] = {"pkexec", "ufw", enable ? "enable" : "disable", NULL};
        run_pkexec("set-enabled", argv, 3);
        return true;
    }
    case DC_FIREWALL_BACKEND_FIREWALLD: {
        const char *argv[] = {"pkexec", "systemctl", enable ? "enable" : "disable", "--now",
                "firewalld", NULL};
        run_pkexec("set-enabled", argv, 5);
        return true;
    }
    default:
        dc_warn("firewall: set_enabled called with no backend detected, ignoring");
        return false;
    }
}

bool dc_firewall_allow(const char *service, bool allow)
{
    if (!service || !service[0]) {
        dc_warn("firewall: allow called with empty service name, ignoring");
        return false;
    }

    dc_firewall_backend backend = dc_firewall_backend_get();

    switch (backend) {
    case DC_FIREWALL_BACKEND_UFW: {
        const char *argv[] = {"pkexec", "ufw", allow ? "allow" : "deny", service, NULL};
        run_pkexec("allow-service", argv, 4);
        return true;
    }
    case DC_FIREWALL_BACKEND_FIREWALLD: {
        dc_firewall_info info = {0};
        /* Best-effort active-zone lookup so the rule lands where the user
         * actually expects it; fall back to firewalld's own "public"
         * default zone if we couldn't determine one (e.g. daemon not
         * running yet, or this is the DANKC_FIREWALL_TEST forced-backend
         * path on a machine that doesn't have firewalld installed at all). */
        read_firewalld_status(&info);
        const char *zone = info.active_zone[0] ? info.active_zone : "public";

        char zone_arg[80];
        snprintf(zone_arg, sizeof(zone_arg), "--zone=%s", zone);
        char service_arg[80];
        snprintf(service_arg, sizeof(service_arg), "--%s-service=%s",
                allow ? "add" : "remove", service);

        const char *argv1[] = {"pkexec", "firewall-cmd", zone_arg, service_arg, "--permanent",
                NULL};
        run_pkexec("allow-service", argv1, 5);

        const char *argv2[] = {"pkexec", "firewall-cmd", "--reload", NULL};
        run_pkexec("allow-service-reload", argv2, 3);
        return true;
    }
    default:
        dc_warn("firewall: allow called with no backend detected, ignoring");
        return false;
    }
}
