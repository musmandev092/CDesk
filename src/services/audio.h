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
#include <stddef.h>

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

/* Read the default *source* (mic) mute state -- never blocks the caller for
 * more than one `wpctl get-volume @DEFAULT_AUDIO_SOURCE@` popen() round trip
 * (~35-40ms), and even that only once every DC_AUDIO_SOURCE_CACHE_SECONDS
 * (audio.c); every other call just serves the cache. Kept a plain
 * synchronous popen() (not the async fork+pipe machinery dc_audio_read()
 * uses for the sink) since it's polled far less often than the sink used to
 * be before that optimization -- same tradeoff ui/controlcenter.c's and
 * ui/settings.c's own private `audio_source_read()` copies already make.
 * `out->volume` is unused by callers that only care about mute (the OSD),
 * populated anyway since it's free from the same parse. Returns true if the
 * cached reading is a successful one. */
bool dc_audio_read_source(dc_audio_info *out);

/* Read the default sink's human-readable device name (e.g. "Built-in Audio
 * Analog Stereo", from `wpctl inspect @DEFAULT_AUDIO_SINK@`'s
 * node.description) into `out` (truncated to out_sz). Same synchronous-
 * popen()-with-cache shape and cadence as dc_audio_read_source() above.
 * Returns true if a name was found; `out` is always NUL-terminated (empty
 * string on failure). */
bool dc_audio_read_sink_name(char *out, size_t out_sz);

#endif /* DC_SERVICES_AUDIO_H */
