/* greetd.h — greetd IPC client (login greeter authentication).
 *
 * greetd (https://git.sr.ht/~kennylevinsen/greetd) is a minimal login
 * manager daemon: it owns the seat/VT, spawns this greeter, and hands control
 * of the session to whichever process authenticates through it. The wire
 * protocol (docs/28-GREETER-PLAN.md, upstream proto.md) is a Unix stream
 * socket at $GREETD_SOCK; each message is a native-endian uint32 byte length
 * followed by that many bytes of JSON.
 *
 * This module is a thin, non-blocking, single-connection client: it owns the
 * socket fd registered on the shell's event loop (see core/loop.h), frames
 * outgoing requests, reassembles incoming frames from a byte stream that may
 * arrive split across reads or batched multiple-frames-per-read, and reports
 * decoded responses to the caller via a single event callback. It carries no
 * greetd protocol *state machine* opinion beyond framing — the caller (the
 * greeter UI) decides what request to send next based on the event kind.
 */
#ifndef DC_SERVICES_GREETD_H
#define DC_SERVICES_GREETD_H

#include <stdbool.h>

struct dc_loop;

typedef struct dc_greetd dc_greetd;

/* auth_message_type values from a DC_GREETD_AUTH_MESSAGE event, mirroring
 * greetd's own enum (proto.md): a "visible" prompt should be echoed as the
 * user types (e.g. a username at a multi-factor prompt), a "secret" prompt
 * should be masked (password), "info"/"error" are not prompts at all —
 * greetd is just telling the greeter something to display, and the greeter
 * must still send a (null) post_auth_message_response to keep the exchange
 * moving. */
enum dc_greetd_auth_type {
    DC_GREETD_AUTH_VISIBLE = 0,
    DC_GREETD_AUTH_SECRET,
    DC_GREETD_AUTH_INFO,
    DC_GREETD_AUTH_ERROR,
};

enum dc_greetd_event_kind {
    DC_GREETD_SUCCESS = 0,  /* request acknowledged; session created/started or cancelled cleanly */
    DC_GREETD_AUTH_MESSAGE, /* greetd wants a post_auth_message_response */
    DC_GREETD_ERROR,        /* request failed; error_type/text describe why */
};

struct dc_greetd_event {
    enum dc_greetd_event_kind kind;
    enum dc_greetd_auth_type auth_type; /* valid when kind == DC_GREETD_AUTH_MESSAGE */
    char text[256];                     /* auth_message or error description, truncated */
    char error_type[32];                /* "auth_error" or "error"; valid when kind == DC_GREETD_ERROR */
};

typedef void (*dc_greetd_event_cb)(const struct dc_greetd_event *ev, void *user_data);

/* Connect to $GREETD_SOCK (or $DANKC_GREETD_SOCK_PATH, if set — an override
 * used to point the client at a mock server for testing) as a non-blocking
 * Unix stream socket and register it on `loop`. Returns NULL if neither
 * environment variable is set, or the socket can't be opened/connected
 * (greetd not running, wrong path, permissions). `cb` is invoked from within
 * the loop for every decoded response frame; it must not be NULL. */
dc_greetd *dc_greetd_create(struct dc_loop *loop, dc_greetd_event_cb cb, void *user_data);

/* Tear down the connection: unregisters the fd from the loop, closes it, and
 * frees all state. Safe to call with NULL. */
void dc_greetd_destroy(dc_greetd *g);

/* Requests below map 1:1 to greetd's wire messages (docs/28-GREETER-PLAN.md
 * §greetd-wire-proto). Each returns false on a local framing/write failure
 * (the request was not sent — greetd's actual answer, if any, always arrives
 * later as an event, never as a return value here). */

/* {"type":"create_session","username":username} */
bool dc_greetd_create_session(dc_greetd *g, const char *username);

/* {"type":"post_auth_message_response","response":response} — pass NULL for
 * a JSON null response (required for "info"/"error" auth messages, and valid
 * for any prompt the greeter chooses not to answer). */
bool dc_greetd_respond(dc_greetd *g, const char *response);

/* {"type":"start_session","cmd":cmd,"env":env} — both arrays are NULL
 * terminated, C-style (like execve's argv/envp). `env` may be NULL for an
 * empty env array. */
bool dc_greetd_start_session(dc_greetd *g, char *const cmd[], char *const env[]);

/* {"type":"cancel_session"} */
bool dc_greetd_cancel(dc_greetd *g);

#endif /* DC_SERVICES_GREETD_H */
