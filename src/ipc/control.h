/* control.h — the dankc control socket (dankctl IPC).
 *
 * A line-based UNIX socket at $XDG_RUNTIME_DIR/dankc.sock. External clients
 * (the `dankc ctl <cmd>` client, bound to niri keys) send one command per line;
 * the running shell dispatches it to a handler. See docs/06-ROADMAP (dankctl).
 */
#ifndef DC_IPC_CONTROL_H
#define DC_IPC_CONTROL_H

struct dc_loop;

typedef struct dc_control dc_control;

/* Called with a received command line (whitespace-trimmed, no newline). */
typedef void (*dc_control_cmd_cb)(const char *cmd, void *user_data);

/* Bind the control socket and register it with the loop. Returns NULL on
 * failure (e.g. socket path unavailable). */
dc_control *dc_control_create(struct dc_loop *loop, dc_control_cmd_cb cb, void *user_data);
void dc_control_destroy(dc_control *c);

/* Connect to a running shell's control socket and send `cmd` + newline.
 * Returns 0 on success. Used by the `dankc ctl` client mode. */
int dc_control_send(const char *cmd);

#endif /* DC_IPC_CONTROL_H */
