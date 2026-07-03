/* polkit.c — polkit authentication agent. See polkit.h for the
 * libpolkit-agent-vs-direct-D-Bus rationale.
 *
 * Protocol (docs/03-SERVICES.md sec.10, cross-checked against the installed
 * /usr/lib/polkit-1/polkit-agent-helper-1 binary's own strings table, which
 * lists exactly PAM_PROMPT_ECHO_OFF/ON, PAM_ERROR_MSG, PAM_TEXT_INFO,
 * SUCCESS/FAILURE and polkit_authority_authentication_agent_response*_sync --
 * i.e. the helper calls AuthenticationAgentResponse2 itself, so this file
 * never does):
 *
 *   1. polkitd calls BeginAuthentication(action_id, message, icon, details,
 *      cookie, identities) on our exported AuthenticationAgent object. We
 *      reply empty immediately (the real outcome is reported later,
 *      asynchronously, by the helper -- see below) and, in parallel, show
 *      the password modal and spawn `polkit-agent-helper-1 <username>`.
 *   2. `cookie\n` is written to the helper's stdin right away (before any
 *      prompt appears -- the helper reads it unconditionally as its first
 *      line).
 *   3. The helper then drives one `pam_authenticate()` call, whose PAM
 *      conversation is bridged over its stdout/stdin: each
 *      "PAM_PROMPT_ECHO_OFF"/"PAM_PROMPT_ECHO_ON" line means "send a
 *      password line back on stdin now". Note some PAM stacks configure
 *      pam_unix with retry=N, so a wrong attempt can produce a
 *      "PAM_ERROR_MSG <text>" line followed by *another* prompt line, all
 *      within this same helper process/pam_authenticate() call -- this is
 *      the normal multi-attempt path, not a restart.
 *   4. A final "SUCCESS\n" or "FAILURE\n" line means the helper is done: it
 *      has already told polkitd the outcome via AuthenticationAgentResponse2
 *      itself, so we just clean up our side (close the pipes, hide/error the
 *      modal).
 *
 * CancelAuthentication(cookie) is polkitd telling us to give up (e.g. the
 * original caller lost interest) -- SIGTERM the helper and hide the modal.
 */
#include "services/polkit.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"
#include "services/dbus.h"
#include "ui/polkit_modal.h"
#include "wayland/wl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <systemd/sd-login.h>

#define DC_POLKIT_AGENT_PATH "/org/dankc/PolkitAgent"
#define DC_POLKIT_AUTHORITY_DEST "org.freedesktop.PolicyKit1"
#define DC_POLKIT_AUTHORITY_PATH "/org/freedesktop/PolicyKit1/Authority"
#define DC_POLKIT_AUTHORITY_IFACE "org.freedesktop.PolicyKit1.Authority"
#define DC_POLKIT_AGENT_IFACE "org.freedesktop.PolicyKit1.AuthenticationAgent"

#define DC_POLKIT_COOKIE_MAX 128
#define DC_POLKIT_USER_MAX 64
#define DC_POLKIT_PW_MAX 256
#define DC_POLKIT_LINEBUF_MAX 1024

/* Distro-dependent install location for the PAM helper -- Arch (this
 * machine) uses the first; the others cover Debian/Ubuntu/older FHS
 * variants seen in the wild. */
static const char *const HELPER_CANDIDATES[] = {
    "/usr/lib/polkit-1/polkit-agent-helper-1",
    "/usr/libexec/polkit-1/polkit-agent-helper-1",
    "/usr/lib/policykit-1/polkit-agent-helper-1",
    "/lib/polkit-1/polkit-agent-helper-1",
};

struct dc_polkit {
    sd_bus *bus; /* system bus, borrowed from dc_dbus */
    struct dc_loop *loop;
    struct dc_wayland *wl;
    struct dc_polkit_modal *modal;
    sd_bus_slot *vtable_slot;
    bool active; /* RegisterAuthenticationAgent succeeded */

    /* One in-flight request at a time -- see polkit.h's doc comment. */
    bool session_active;
    char cookie[DC_POLKIT_COOKIE_MAX];
    char username[DC_POLKIT_USER_MAX];
    pid_t helper_pid;
    int helper_stdin_fd;  /* write end -- feed password lines here */
    int helper_stdout_fd; /* read end -- registered on the loop */
    char linebuf[DC_POLKIT_LINEBUF_MAX];
    size_t linebuf_len;

    bool prompt_ready; /* helper has asked for a password at least once */
    bool has_pending_password;
    char pending_password[DC_POLKIT_PW_MAX];
};

static void session_finish(dc_polkit *pk);
static void flush_pending_password(dc_polkit *pk);

/* --- small helpers --------------------------------------------------------
 */

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static const char *find_helper(void)
{
    for (size_t i = 0; i < DC_ARRAY_LEN(HELPER_CANDIDATES); i++) {
        if (access(HELPER_CANDIDATES[i], X_OK) == 0)
            return HELPER_CANDIDATES[i];
    }
    return NULL;
}

static struct dc_output *pk_first_output(dc_wayland *wl)
{
    if (!wl)
        return NULL;
    dc_output *o;
    wl_list_for_each(o, &wl->outputs, link) {
        return o;
    }
    return NULL;
}

/* --- PAM-helper subprocess -------------------------------------------------
 */

static void helper_readable(int fd, uint32_t revents, void *data);

static void spawn_helper(dc_polkit *pk, const char *username, const char *cookie)
{
    const char *helper = find_helper();
    if (!helper) {
        dc_warn("polkit: no polkit-agent-helper-1 found on this system; cannot authenticate");
        dc_polkit_modal_set_error(pk->modal, "No polkit authentication helper installed.");
        return;
    }

    int in_fds[2], out_fds[2];
    if (pipe(in_fds) < 0) {
        dc_warn("polkit: pipe() failed: %s", strerror(errno));
        dc_polkit_modal_set_error(pk->modal, "Internal error starting authentication.");
        return;
    }
    if (pipe(out_fds) < 0) {
        dc_warn("polkit: pipe() failed: %s", strerror(errno));
        close(in_fds[0]);
        close(in_fds[1]);
        dc_polkit_modal_set_error(pk->modal, "Internal error starting authentication.");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        dc_warn("polkit: fork() failed: %s", strerror(errno));
        close(in_fds[0]);
        close(in_fds[1]);
        close(out_fds[0]);
        close(out_fds[1]);
        dc_polkit_modal_set_error(pk->modal, "Internal error starting authentication.");
        return;
    }

    if (pid == 0) { /* child: helper's stdin/stdout <- our pipes */
        dup2(in_fds[0], STDIN_FILENO);
        dup2(out_fds[1], STDOUT_FILENO);
        close(in_fds[0]);
        close(in_fds[1]);
        close(out_fds[0]);
        close(out_fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        setsid();
        execl(helper, helper, username, (char *)NULL);
        _exit(127);
    }

    close(in_fds[0]);
    close(out_fds[1]);
    fcntl(out_fds[0], F_SETFL, O_NONBLOCK);

    pk->helper_pid = pid;
    pk->helper_stdin_fd = in_fds[1];
    pk->helper_stdout_fd = out_fds[0];
    pk->linebuf_len = 0;
    pk->linebuf[0] = '\0';
    pk->prompt_ready = false;

    dc_loop_add_fd(pk->loop, pk->helper_stdout_fd, POLLIN, helper_readable, pk);

    /* Per docs/03 sec.10: the cookie goes on the helper's stdin first, before
     * any PAM prompt appears. */
    write_all(pk->helper_stdin_fd, cookie, strlen(cookie));
    write_all(pk->helper_stdin_fd, "\n", 1);

    dc_info("polkit: spawned %s for '%s' (pid %d)", helper, username, (int)pid);
}

static void flush_pending_password(dc_polkit *pk)
{
    if (!pk->has_pending_password || pk->helper_stdin_fd < 0)
        return;
    write_all(pk->helper_stdin_fd, pk->pending_password, strlen(pk->pending_password));
    write_all(pk->helper_stdin_fd, "\n", 1);
    pk->has_pending_password = false;
    memset(pk->pending_password, 0, sizeof(pk->pending_password));
}

static void handle_helper_line(dc_polkit *pk, const char *line)
{
    dc_debug("polkit: helper: %s", line);

    if (starts_with(line, "PAM_PROMPT_ECHO_OFF") || starts_with(line, "PAM_PROMPT_ECHO_ON")) {
        pk->prompt_ready = true;
        if (pk->has_pending_password)
            flush_pending_password(pk);
        else
            dc_polkit_modal_set_busy(pk->modal, false); /* let the user type */
        return;
    }
    if (starts_with(line, "PAM_ERROR_MSG")) {
        const char *msg = line + strlen("PAM_ERROR_MSG");
        while (*msg == ' ')
            msg++;
        dc_polkit_modal_set_error(pk->modal, *msg ? msg : "Authentication error.");
        pk->prompt_ready = false; /* a fresh ECHO_OFF should follow for the retry */
        return;
    }
    if (starts_with(line, "PAM_TEXT_INFO")) {
        const char *msg = line + strlen("PAM_TEXT_INFO");
        while (*msg == ' ')
            msg++;
        dc_info("polkit: %s", *msg ? msg : line);
        return;
    }
    if (strcmp(line, "SUCCESS") == 0) {
        dc_info("polkit: authentication succeeded for '%s'", pk->username);
        dc_polkit_modal_hide(pk->modal);
        session_finish(pk);
        return;
    }
    if (strcmp(line, "FAILURE") == 0) {
        dc_warn("polkit: authentication failed for '%s' (no attempts remain)", pk->username);
        dc_polkit_modal_set_error(pk->modal, "Authentication failed.");
        session_finish(pk);
        return;
    }
    /* Unrecognized line (helper version skew) -- ignore. */
}

static void helper_readable(int fd, uint32_t revents, void *data)
{
    DC_UNUSED(revents);
    dc_polkit *pk = data;
    for (;;) {
        if (!pk->session_active || pk->helper_stdout_fd != fd)
            return; /* a line handled above already tore this session down */
        if (pk->linebuf_len + 1 >= sizeof(pk->linebuf))
            pk->linebuf_len = 0; /* defensive: drop an absurdly long line */

        ssize_t n = read(fd, pk->linebuf + pk->linebuf_len, sizeof(pk->linebuf) - pk->linebuf_len - 1);
        if (n > 0) {
            pk->linebuf_len += (size_t)n;
            pk->linebuf[pk->linebuf_len] = '\0';
            char *start = pk->linebuf;
            char *nl;
            while (pk->session_active && (nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                handle_helper_line(pk, start);
                start = nl + 1;
            }
            if (!pk->session_active)
                return;
            size_t remaining = pk->linebuf_len - (size_t)(start - pk->linebuf);
            memmove(pk->linebuf, start, remaining);
            pk->linebuf_len = remaining;
            pk->linebuf[pk->linebuf_len] = '\0';
            continue;
        }
        if (n == 0) { /* helper exited without a final status line */
            if (pk->session_active) {
                dc_warn("polkit: helper exited unexpectedly for '%s'", pk->username);
                dc_polkit_modal_set_error(pk->modal, "Authentication helper exited unexpectedly.");
                session_finish(pk);
            }
            return;
        }
        return; /* EAGAIN */
    }
}

static void session_finish(dc_polkit *pk)
{
    if (pk->helper_stdout_fd >= 0) {
        dc_loop_remove_fd(pk->loop, pk->helper_stdout_fd);
        close(pk->helper_stdout_fd);
    }
    if (pk->helper_stdin_fd >= 0)
        close(pk->helper_stdin_fd);
    pk->helper_stdout_fd = -1;
    pk->helper_stdin_fd = -1;
    pk->helper_pid = 0;
    pk->session_active = false;
    pk->prompt_ready = false;
    pk->has_pending_password = false;
    memset(pk->pending_password, 0, sizeof(pk->pending_password));
    pk->linebuf_len = 0;
    pk->linebuf[0] = '\0';
    pk->cookie[0] = '\0';
}

/* --- modal callbacks -------------------------------------------------------
 */

static void pk_on_submit(const char *password, void *user_data)
{
    dc_polkit *pk = user_data;
    if (!pk->session_active)
        return;
    snprintf(pk->pending_password, sizeof(pk->pending_password), "%s", password ? password : "");
    pk->has_pending_password = true;
    dc_polkit_modal_set_busy(pk->modal, true);
    if (pk->prompt_ready)
        flush_pending_password(pk);
    /* else: wait for the helper's next PAM_PROMPT_ECHO_OFF line, which will
     * flush it (handle_helper_line()) -- the user typed faster than the PAM
     * conversation started. */
}

static void pk_on_cancel(void *user_data)
{
    dc_polkit *pk = user_data;
    if (pk->session_active) {
        dc_info("polkit: user cancelled authentication for '%s'", pk->username);
        if (pk->helper_pid > 0)
            kill(pk->helper_pid, SIGTERM);
        session_finish(pk);
    }
    dc_polkit_modal_hide(pk->modal);
}

/* --- AuthenticationAgent D-Bus methods --------------------------------------
 */

/* Skip past a{ss} (Details) without needing the contents -- same
 * enter/skip/exit-per-entry idiom as services/mpris.c's a{sv} walk. */
static void skip_details(sd_bus_message *msg)
{
    if (sd_bus_message_enter_container(msg, 'a', "{ss}") < 0)
        return;
    while (sd_bus_message_enter_container(msg, 'e', "ss") > 0) {
        sd_bus_message_skip(msg, "ss");
        sd_bus_message_exit_container(msg);
    }
    sd_bus_message_exit_container(msg);
}

/* Walk Identities (a(sa{sv})) looking for a "unix-user" identity's "uid"
 * variant. Returns true and sets *uid if found (first match wins -- polkitd
 * typically only ever lists one identity for an interactive session). */
static bool read_identity_uid(sd_bus_message *msg, uint32_t *uid)
{
    bool found = false;
    if (sd_bus_message_enter_container(msg, 'a', "(sa{sv})") < 0)
        return false;
    while (sd_bus_message_enter_container(msg, 'r', "sa{sv}") > 0) {
        const char *kind = NULL;
        sd_bus_message_read_basic(msg, 's', &kind);
        if (sd_bus_message_enter_container(msg, 'a', "{sv}") > 0) {
            while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
                const char *key = NULL;
                sd_bus_message_read_basic(msg, 's', &key);
                if (!found && kind && strcmp(kind, "unix-user") == 0 && key &&
                    strcmp(key, "uid") == 0 &&
                    sd_bus_message_enter_container(msg, 'v', "u") > 0) {
                    sd_bus_message_read_basic(msg, 'u', uid);
                    sd_bus_message_exit_container(msg);
                    found = true;
                } else {
                    sd_bus_message_skip(msg, "v");
                }
                sd_bus_message_exit_container(msg); /* e */
            }
            sd_bus_message_exit_container(msg); /* a{sv} */
        }
        sd_bus_message_exit_container(msg); /* r */
    }
    sd_bus_message_exit_container(msg); /* a(sa{sv}) */
    return found;
}

static int method_begin_authentication(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    DC_UNUSED(err);
    dc_polkit *pk = userdata;

    const char *action_id = NULL, *message = NULL, *icon = NULL, *cookie = NULL;
    if (sd_bus_message_read(msg, "sss", &action_id, &message, &icon) < 0)
        return sd_bus_reply_method_return(msg, "");
    skip_details(msg);
    if (sd_bus_message_read(msg, "s", &cookie) < 0)
        return sd_bus_reply_method_return(msg, "");

    uint32_t uid = 0;
    bool got_uid = read_identity_uid(msg, &uid);

    /* Reply immediately -- BeginAuthentication's own reply carries no
     * outcome; the real result is reported later, asynchronously, by the PAM
     * helper (see the file header). Doing this before we've even shown the
     * modal keeps polkitd from timing out the call while the user is
     * thinking. */
    int rr = sd_bus_reply_method_return(msg, "");

    if (pk->session_active) {
        dc_warn("polkit: BeginAuthentication arrived while another request is in progress "
                "(action=%s); ignoring",
                action_id ? action_id : "?");
        return rr;
    }

    struct passwd *pw = got_uid ? getpwuid((uid_t)uid) : NULL;
    const char *username = pw ? pw->pw_name : NULL;
    if (!username)
        username = getenv("USER");
    if (!username)
        username = "root";

    snprintf(pk->username, sizeof(pk->username), "%s", username);
    snprintf(pk->cookie, sizeof(pk->cookie), "%s", cookie ? cookie : "");
    pk->session_active = true;
    pk->prompt_ready = false;
    pk->has_pending_password = false;

    dc_info("polkit: BeginAuthentication action=%s user=%s", action_id ? action_id : "?",
            pk->username);
    dc_polkit_modal_show(pk->modal, pk_first_output(pk->wl), message, pk->username, pk_on_submit,
                        pk_on_cancel, pk);
    spawn_helper(pk, pk->username, pk->cookie);
    return rr;
}

static int method_cancel_authentication(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    DC_UNUSED(err);
    dc_polkit *pk = userdata;
    const char *cookie = NULL;
    sd_bus_message_read(msg, "s", &cookie);

    if (pk->session_active && cookie && strcmp(cookie, pk->cookie) == 0) {
        dc_info("polkit: authority cancelled the in-progress request");
        if (pk->helper_pid > 0)
            kill(pk->helper_pid, SIGTERM);
        session_finish(pk);
        dc_polkit_modal_hide(pk->modal);
    }
    return sd_bus_reply_method_return(msg, "");
}

static const sd_bus_vtable polkit_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("BeginAuthentication", "sssa{ss}sa(sa{sv})", "", method_begin_authentication,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CancelAuthentication", "s", "", method_cancel_authentication,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

/* --- registration -----------------------------------------------------------
 */

/* RegisterAuthenticationAgent(Subject subject, String locale, String
 * object_path), Subject = (s kind, a{sv} details). Built by hand rather than
 * via sd_bus_call_method()'s simple vararg format string -- the nested
 * struct/array/dict-entry/variant needs sd_bus_message_open_container() at
 * each level, same as how services/bluez.c and services/mpris.c *parse*
 * a{sv} on the way in (this is the mirror-image on the way out). */
static bool register_agent(dc_polkit *pk, const char *session_id)
{
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(pk->bus, &m, DC_POLKIT_AUTHORITY_DEST,
                                           DC_POLKIT_AUTHORITY_PATH, DC_POLKIT_AUTHORITY_IFACE,
                                           "RegisterAuthenticationAgent");
    if (r < 0)
        return false;

    sd_bus_message_open_container(m, 'r', "sa{sv}");
    sd_bus_message_append(m, "s", "unix-session");
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "session-id");
    sd_bus_message_open_container(m, 'v', "s");
    sd_bus_message_append(m, "s", session_id);
    sd_bus_message_close_container(m); /* v */
    sd_bus_message_close_container(m); /* e */
    sd_bus_message_close_container(m); /* a{sv} */
    sd_bus_message_close_container(m); /* r */
    sd_bus_message_append(m, "s", "");                  /* locale: default */
    sd_bus_message_append(m, "s", DC_POLKIT_AGENT_PATH); /* our object path */

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    r = sd_bus_call(pk->bus, m, 0, &err, &reply);
    sd_bus_message_unref(m);
    if (reply)
        sd_bus_message_unref(reply);

    if (r < 0) {
        /* Expected/benign: another agent (a DE's own, or a second dankc
         * instance) already registered for this session -- polkitd allows
         * exactly one per session and rejects the rest. */
        dc_warn("polkit: RegisterAuthenticationAgent failed (%s) -- another authentication agent "
                "is probably already running for this session; dankc's stays inactive",
                err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return false;
    }
    sd_bus_error_free(&err);
    return true;
}

dc_polkit *dc_polkit_create(struct dc_dbus *dbus, struct dc_loop *loop, struct dc_wayland *wl,
                            struct dc_polkit_modal *modal)
{
    if (!dbus || !dbus->system) {
        dc_warn("polkit: no system bus; authentication agent disabled");
        return NULL;
    }

    dc_polkit *pk = calloc(1, sizeof(*pk));
    pk->bus = dbus->system;
    pk->loop = loop;
    pk->wl = wl;
    pk->modal = modal;
    pk->helper_stdin_fd = -1;
    pk->helper_stdout_fd = -1;

    int r = sd_bus_add_object_vtable(pk->bus, &pk->vtable_slot, DC_POLKIT_AGENT_PATH,
                                     DC_POLKIT_AGENT_IFACE, polkit_vtable, pk);
    if (r < 0) {
        dc_warn("polkit: sd_bus_add_object_vtable failed: %s", strerror(-r));
        free(pk);
        return NULL;
    }

    char *sd_session = NULL;
    char session_id[64] = {0};
    if (sd_pid_get_session(0, &sd_session) >= 0 && sd_session) {
        snprintf(session_id, sizeof(session_id), "%s", sd_session);
        free(sd_session);
    } else {
        const char *env = getenv("XDG_SESSION_ID");
        if (env && env[0])
            snprintf(session_id, sizeof(session_id), "%s", env);
    }

    if (!session_id[0]) {
        dc_warn("polkit: could not determine the logind session id (not a systemd session?); "
                "agent object exported but not registered");
        pk->active = false;
        return pk;
    }

    pk->active = register_agent(pk, session_id);
    if (pk->active)
        dc_info("polkit: registered as authentication agent for session %s", session_id);
    return pk;
}

bool dc_polkit_active(dc_polkit *pk)
{
    return pk && pk->active;
}

void dc_polkit_destroy(dc_polkit *pk)
{
    if (!pk)
        return;
    if (pk->session_active) {
        if (pk->helper_pid > 0)
            kill(pk->helper_pid, SIGTERM);
        session_finish(pk);
    }
    if (pk->vtable_slot)
        sd_bus_slot_unref(pk->vtable_slot);
    free(pk);
}
