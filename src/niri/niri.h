/* niri.h — niri IPC client (EventStream).
 *
 * Connects to $NIRI_SOCKET, subscribes to the event stream, and keeps a live
 * view of the compositor's workspaces. See docs/03-SERVICES.md §12.
 */
#ifndef DC_NIRI_NIRI_H
#define DC_NIRI_NIRI_H

#include <stdbool.h>
#include <stdint.h>

struct dc_loop;

#define DC_NIRI_MAX_WORKSPACES 64
#define DC_NIRI_NAME_MAX 64

typedef struct dc_niri_workspace {
    uint64_t id;
    uint8_t idx; /* 1-based index within its output */
    char output[DC_NIRI_NAME_MAX];
    char name[DC_NIRI_NAME_MAX];
    bool is_focused;
    bool is_active;
    bool is_urgent;
} dc_niri_workspace;

typedef void (*dc_niri_changed_cb)(void *user_data);

typedef struct dc_niri dc_niri;

/* Connect and subscribe. Returns NULL if niri is unavailable (workspaces then
 * simply do not render). Caller owns the result. */
dc_niri *dc_niri_connect(void);
void dc_niri_destroy(dc_niri *niri);

void dc_niri_integrate(dc_niri *niri, struct dc_loop *loop);
void dc_niri_set_changed_cb(dc_niri *niri, dc_niri_changed_cb cb, void *user_data);

/* Read-only snapshot of the current workspaces. */
const dc_niri_workspace *dc_niri_workspaces(const dc_niri *niri, int *count);

#endif /* DC_NIRI_NIRI_H */
