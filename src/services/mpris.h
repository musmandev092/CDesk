/* mpris.h — now-playing info from the first MPRIS player on the session bus.
 *
 * Light polling client (cached ~1s) for a bar media widget. See
 * docs/03-SERVICES.md §8.
 */
#ifndef DC_SERVICES_MPRIS_H
#define DC_SERVICES_MPRIS_H

#include <stdbool.h>

struct dc_dbus;

typedef struct dc_mpris_info {
    bool active;  /* a player is present on the bus */
    bool playing; /* PlaybackStatus == "Playing" */
    char title[256];
    char artist[128];
} dc_mpris_info;

/* Bind the session bus (from dc_dbus). Call once at startup. */
void dc_mpris_init(struct dc_dbus *dbus);

/* Read cached now-playing state (refreshed at most once a second). Returns true
 * if a player is present. */
bool dc_mpris_read(dc_mpris_info *out);

/* Transport controls for the bar's media widget (docs/12-BAR-SPEC.md sec.4
 * music). No-op if no MPRIS player is present; fire-and-forget (the reply, if
 * any, is discarded — the next dc_mpris_read() picks up the new state). */
void dc_mpris_play_pause(void);
void dc_mpris_next(void);
void dc_mpris_previous(void);

#endif /* DC_SERVICES_MPRIS_H */
