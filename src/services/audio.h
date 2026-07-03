/* audio.h — default-sink volume/mute via WirePlumber's `wpctl`.
 *
 * A light stand-in until the libpipewire service lands (M3); dependency-free
 * beyond the wpctl binary and good enough for the bar indicator.
 *
 * docs/16-PERF2-PLAN.md T1.2: `wpctl get-volume` used to run via a
 * synchronous popen()/pclose(), blocking the single-threaded event loop for
 * ~35-40ms every cache refresh (and once, unavoidably, on the very first
 * render). dc_audio_init() wires this module to the event loop so refreshes
 * run as a fork+pipe+non-blocking-fd child (same shape as services/net.c's
 * wifi scan / services/clipboard.c's transfer_read) instead.
 */
#ifndef DC_SERVICES_AUDIO_H
#define DC_SERVICES_AUDIO_H

#include <stdbool.h>

struct dc_loop;

typedef struct dc_audio_info {
    bool available;
    int volume; /* 0-100 */
    bool muted;
} dc_audio_info;

/* Bind the event loop so cache refreshes can be dispatched as a non-blocking
 * child (dc_loop_add_fd()) instead of a synchronous popen(). Call once at
 * startup, same convention as dc_net_init()/dc_bluez_init(). Safe to skip:
 * dc_audio_read() without a bound loop just never kicks off a refresh (always
 * reports "no reading yet"), it never blocks either way. */
void dc_audio_init(struct dc_loop *loop);

/* Read the default sink's volume + mute from the cache -- never blocks. If
 * the cache is empty or older than DC_AUDIO_CACHE_SECONDS, kicks off an async
 * refresh (no-op if one is already in flight, or if dc_audio_init() was never
 * called) and returns whatever is cached right now (possibly "not available"
 * on the very first call, until the async result lands). Returns true if the
 * cached reading is a successful one. */
bool dc_audio_read(dc_audio_info *out);

/* Set the default sink's volume to `percent` (0-100, clamped), fire-and-forget
 * (forks `wpctl set-volume`, non-blocking — used from pointer-drag motion,
 * which fires every frame and can't afford to wait on a child process). Also
 * forces the next dc_audio_read() to treat the cache as stale and kicks an
 * async refresh right away, so the change is reflected within about one
 * wpctl round-trip instead of waiting up to DC_AUDIO_CACHE_SECONDS. */
void dc_audio_set_volume(int percent);

#endif /* DC_SERVICES_AUDIO_H */
