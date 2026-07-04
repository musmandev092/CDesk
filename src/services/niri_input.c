#include "services/niri_input.h"

#include "core/log.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DC_NIRI_INPUT_PATH_MAX 512
#define DC_NIRI_INPUT_MANAGED_FILENAME "dankc-input.kdl"
#define DC_NIRI_INPUT_INCLUDE_LINE "include \"" DC_NIRI_INPUT_MANAGED_FILENAME "\""

static bool dryrun_enabled(void)
{
    const char *v = getenv("DANKC_NIRI_DRYRUN");
    return v && v[0] == '1';
}

static dc_niri_input_validate_result g_last_validate = DC_NIRI_INPUT_VALIDATE_UNKNOWN;

dc_niri_input_validate_result dc_niri_input_last_validate(void)
{
    return g_last_validate;
}

/* --- fragment serialization ------------------------------------------------ */

static void serialize_fragment(FILE *f, const dc_niri_input_config *cfg)
{
    fputs("// Managed by DankC's Settings > Mouse & Keyboard tab.\n"
          "// Hand edits are fine, but saving a change through the UI rewrites this whole\n"
          "// file from what dankc currently understands -- anything else here will be\n"
          "// lost on the next save.\n\n",
            f);

    fputs("input {\n", f);

    fputs("    touchpad {\n", f);
    if (cfg->touchpad_tap)
        fputs("        tap\n", f);
    if (cfg->touchpad_natural_scroll)
        fputs("        natural-scroll\n", f);
    if (cfg->touchpad_dwt)
        fputs("        dwt\n", f);
    if (cfg->touchpad_disabled_on_external_mouse)
        fputs("        disabled-on-external-mouse\n", f);
    if (cfg->touchpad_accel_enabled)
        fprintf(f, "        accel-speed %g\n", (double)cfg->touchpad_accel_speed);
    fputs("    }\n", f);

    fputs("    mouse {\n", f);
    if (cfg->mouse_natural_scroll)
        fputs("        natural-scroll\n", f);
    if (cfg->mouse_accel_enabled)
        fprintf(f, "        accel-speed %g\n", (double)cfg->mouse_accel_speed);
    fputs("    }\n", f);

    bool have_layout = cfg->keyboard_layout && cfg->keyboard_layout[0];
    if (cfg->keyboard_numlock || have_layout) {
        fputs("    keyboard {\n", f);
        if (have_layout) {
            fputs("        xkb {\n", f);
            fprintf(f, "            layout \"%s\"\n", cfg->keyboard_layout);
            fputs("        }\n", f);
        }
        if (cfg->keyboard_numlock)
            fputs("        numlock\n", f);
        fputs("    }\n", f);
    }

    fputs("}\n", f);
}

static bool write_managed_file(const char *managed_path, const dc_niri_input_config *cfg)
{
    FILE *f = fopen(managed_path, "w");
    if (!f) {
        dc_warn("niri_input: could not write %s", managed_path);
        return false;
    }
    serialize_fragment(f, cfg);
    fclose(f);
    return true;
}

/* --- config.kdl include (independent copy of display.c's logic; see this
 * file's header comment for why it's not shared) --------------------------- */

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static bool ensure_include(const char *config_path, const char *managed_filename)
{
    char *text = read_whole_file(config_path);
    if (!text) {
        FILE *f = fopen(config_path, "w");
        if (!f) {
            dc_warn("niri_input: could not create %s", config_path);
            return false;
        }
        fprintf(f, "%s\n", DC_NIRI_INPUT_INCLUDE_LINE);
        fclose(f);
        return true;
    }
    if (strstr(text, managed_filename)) {
        free(text);
        return true;
    }

    char backup_path[DC_NIRI_INPUT_PATH_MAX + 32];
    snprintf(backup_path, sizeof(backup_path), "%s.bak-%ld", config_path, (long)time(NULL));
    FILE *bf = fopen(backup_path, "w");
    if (!bf) {
        dc_warn("niri_input: could not create backup %s; aborting include", backup_path);
        free(text);
        return false;
    }
    fputs(text, bf);
    fclose(bf);
    free(text);

    FILE *f = fopen(config_path, "a");
    if (!f) {
        dc_warn("niri_input: could not append to %s (backup at %s is safe to restore)",
                config_path, backup_path);
        return false;
    }
    fprintf(f, "\n// Added by DankC Settings > Mouse & Keyboard (backup: %s):\n", backup_path);
    fprintf(f, "%s\n", DC_NIRI_INPUT_INCLUDE_LINE);
    fclose(f);
    dc_info("niri_input: added include to %s (backup %s)", config_path, backup_path);
    return true;
}

/* --- niri validate ---------------------------------------------------------
 *
 * Needs an actual waitpid()-able child, which main.c's process-wide
 * `signal(SIGCHLD, SIG_IGN)` breaks (the kernel auto-reaps before waitpid()
 * can collect the status, so it fails ECHILD -- verified directly, same
 * problem printers.c's dc_printers_available() comment documents for
 * system()). Fixed here by temporarily restoring the default SIGCHLD
 * disposition around this one synchronous fork+wait, then restoring SIG_IGN
 * -- safe because dankc is single-threaded and this window never yields to
 * the Wayland event loop. */
static dc_niri_input_validate_result run_niri_validate(const char *config_path)
{
    if (dryrun_enabled()) {
        dc_info("niri_input: [dryrun] niri validate -c %s", config_path);
        return DC_NIRI_INPUT_VALIDATE_UNKNOWN;
    }

    struct sigaction old_sa;
    struct sigaction dfl_sa = {0};
    dfl_sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &dfl_sa, &old_sa);

    pid_t pid = fork();
    if (pid < 0) {
        sigaction(SIGCHLD, &old_sa, NULL);
        dc_warn("niri_input: fork() failed, skipping validate");
        return DC_NIRI_INPUT_VALIDATE_UNKNOWN;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execlp("niri", "niri", "validate", "-c", config_path, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    sigaction(SIGCHLD, &old_sa, NULL);

    if (r != pid) {
        dc_warn("niri_input: waitpid() for `niri validate` failed");
        return DC_NIRI_INPUT_VALIDATE_UNKNOWN;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        dc_info("niri_input: `niri validate` OK");
        return DC_NIRI_INPUT_VALIDATE_OK;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        dc_info("niri_input: `niri` binary not found, skipping validate");
        return DC_NIRI_INPUT_VALIDATE_UNKNOWN;
    }
    dc_warn("niri_input: `niri validate` reported problems (see niri's own log/stderr)");
    return DC_NIRI_INPUT_VALIDATE_FAILED;
}

/* --- persist ---------------------------------------------------------------- */

static bool resolve_config_dir(char *dir, size_t cap, const char *override_dir)
{
    if (override_dir && override_dir[0]) {
        strncpy(dir, override_dir, cap - 1);
        dir[cap - 1] = '\0';
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        dc_warn("niri_input: $HOME unset, cannot locate niri config dir");
        return false;
    }
    snprintf(dir, cap, "%s/.config/niri", home);
    return true;
}

bool dc_niri_input_persist(const dc_niri_input_config *cfg, const char *config_dir_override)
{
    char dir[DC_NIRI_INPUT_PATH_MAX];
    if (!resolve_config_dir(dir, sizeof(dir), config_dir_override))
        return false;

    char managed_path[DC_NIRI_INPUT_PATH_MAX];
    char config_path[DC_NIRI_INPUT_PATH_MAX];
    snprintf(managed_path, sizeof(managed_path), "%s/" DC_NIRI_INPUT_MANAGED_FILENAME, dir);
    snprintf(config_path, sizeof(config_path), "%s/config.kdl", dir);

    if (dryrun_enabled()) {
        dc_info("niri_input: [dryrun] would write %s:", managed_path);
        FILE *f = tmpfile();
        if (f) {
            serialize_fragment(f, cfg);
            rewind(f);
            char line[256];
            while (fgets(line, sizeof(line), f))
                dc_info("niri_input: [dryrun]   %.*s", (int)strcspn(line, "\n"), line);
            fclose(f);
        }
        dc_info("niri_input: [dryrun] would ensure include %s in %s", DC_NIRI_INPUT_INCLUDE_LINE,
                config_path);
        g_last_validate = run_niri_validate(config_path);
        return true;
    }

    if (!write_managed_file(managed_path, cfg))
        return false;
    if (!ensure_include(config_path, DC_NIRI_INPUT_MANAGED_FILENAME))
        return false;

    dc_info("niri_input: persisted input settings to %s", managed_path);
    g_last_validate = run_niri_validate(config_path);
    return true;
}
