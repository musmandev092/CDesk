/* audio.h — audio device enumeration + default-sink/source access via
 * WirePlumber's `pw-dump` (enumeration/reads) and `wpctl` (writes).
 *
 * docs/25-AUDIO-PERDEVICE-PLAN.md T1: the module used to be 100% wpctl
 * subprocess-based (one async fork+pipe for the default sink's volume/mute,
 * two private synchronous popen() readers for the default source's mute and
 * the default sink's display name). It's now backed by a single async
 * `pw-dump` enumeration (fork + non-blocking fd + growable buffer, same
 * event-loop shape as before, cJSON-parsed) that lists every Audio/Sink and
 * Audio/Source node in one shot, tagging whichever one WirePlumber's
 * `metadata.name == "default"` object currently points at as `is_default`.
 * dc_audio_read()/dc_audio_read_source()/dc_audio_read_sink_name() keep their
 * exact original signatures (main.c's OSD tick and the bar depend on them)
 * but now just look up the is_default entry in that same cache instead of
 * running their own wpctl round trip. Volume writes are still fire-and-forget
 * `wpctl set-volume`/`set-mute`/`set-default` forks; there's no synchronous
 * write path.
 */
#ifndef DC_SERVICES_AUDIO_H
#define DC_SERVICES_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dc_loop;

typedef struct dc_audio_info {
    bool available;
    int volume; /* 0-100 */
    bool muted;
} dc_audio_info;

/* One enumerated Audio/Sink or Audio/Source node from `pw-dump`.
 * `name` is the stable `node.name` (e.g.
 * "alsa_output.pci-0000_00_1f.3.analog-stereo"), `desc` the human-readable
 * `node.description` (e.g. "Built-in Audio Analog Stereo"). `volume` is a
 * cubic-scale percent (cbrtf(max channelVolume) * 100, rounded) matching what
 * wpctl/WirePlumber's own UI shows -- NOT a linear channelVolume percent. */
typedef struct {
    uint32_t id;
    char name[96];
    char desc[64];
    int volume;
    bool muted;
    bool is_default;
} dc_audio_device;

/* Bind the event loop so the pw-dump enumeration (and legacy reads that are
 * now backed by it) can be dispatched as a non-blocking child
 * (dc_loop_add_fd()) instead of a synchronous popen(). Call once at startup,
 * same convention as dc_net_init()/dc_bluez_init(). Safe to skip: without a
 * bound loop nothing here ever kicks off a refresh (always reports "no
 * reading yet"/0 devices), it never blocks either way. */
void dc_audio_init(struct dc_loop *loop);

/* Read the default sink's volume + mute from the device cache -- never
 * blocks. If the cache is empty or stale, kicks off an async pw-dump refresh
 * (no-op if one is already in flight, or if dc_audio_init() was never called)
 * and returns whatever is cached right now (possibly "not available" on the
 * very first call, until the async result lands). Returns true if a default
 * sink is currently known. */
bool dc_audio_read(dc_audio_info *out);

/* Set the default sink's volume to `percent` (0-100, clamped), fire-and-forget
 * (forks `wpctl set-volume @DEFAULT_AUDIO_SINK@`, non-blocking — used from
 * pointer-drag motion, which fires every frame and can't afford to wait on a
 * child process). Also forces the device cache stale and kicks an async
 * pw-dump refresh right away, so the change is reflected within about one
 * pw-dump round-trip instead of waiting out the cache window. */
void dc_audio_set_volume(int percent);

/* Read the default *source* (mic) mute state from the device cache -- never
 * blocks. `out->volume` is unused by callers that only care about mute (the
 * OSD), populated anyway since it's free from the same cache entry. Returns
 * true if a default source is currently known. */
bool dc_audio_read_source(dc_audio_info *out);

/* Read the default sink's human-readable device name (node.description, e.g.
 * "Built-in Audio Analog Stereo"; falls back to node.name if no description
 * was reported) into `out` (truncated to out_sz), from the same device
 * cache. Returns true if a name was found; `out` is always NUL-terminated
 * (empty string on failure). */
bool dc_audio_read_sink_name(char *out, size_t out_sz);

/* --- per-device API (docs/25-AUDIO-PERDEVICE-PLAN.md T1) ------------------
 *
 * Enumerated from the same pw-dump cache as the legacy readers above
 * (~5s window, async refresh, never blocks). Skips `*.monitor` Audio/Source
 * nodes (those are monitor taps, not real capture devices) and any
 * "Stream" media.class nodes (per-app streams are a separate future task,
 * T7 -- not implemented here). */

/* Copy up to `max` known sink devices into `out`. Returns the count written
 * (0 if the cache has never been populated or pw-dump is unavailable/failed
 * to parse). Never blocks; kicks an async refresh if the cache is stale. */
int dc_audio_sinks(dc_audio_device *out, int max);

/* Same as dc_audio_sinks() but for Audio/Source devices. */
int dc_audio_sources(dc_audio_device *out, int max);

/* Set device `id`'s volume to `percent`, fire-and-forget (forks
 * `wpctl set-volume <id> <percent>%`, does not wait). Clamped to
 * [0, dc_config_audio_max(that device's node.name)] (docs/25-AUDIO-
 * PERDEVICE-PLAN.md D4, T3) -- falls back to the plain 100 default if `id`
 * isn't found in the cache. Forces the device cache stale and kicks an
 * async pw-dump refresh, same as dc_audio_set_volume() above (this replaces
 * the settings.c g_audio_dirty_until self-invalidate flag -- the service now
 * invalidates its own cache). */
void dc_audio_device_set_volume(uint32_t id, int percent);

/* Toggle device `id`'s mute state, fire-and-forget (forks
 * `wpctl set-mute <id> toggle`, does not wait). Same cache self-invalidate as
 * dc_audio_device_set_volume() above. */
void dc_audio_device_toggle_mute(uint32_t id);

/* Make device `id` the system default sink/source, fire-and-forget (forks
 * `wpctl set-default <id>`, does not wait). Same cache self-invalidate as
 * dc_audio_device_set_volume() above. */
void dc_audio_set_default(uint32_t id);

/* Display name for `dev`: its configured alias (dc_config_audio_alias(),
 * docs/25-AUDIO-PERDEVICE-PLAN.md D3, dankc-config-only -- no wireplumber
 * file is touched, so this never audibly interrupts audio) if one is set and
 * non-empty, else its raw pw-dump `desc`. Every dankc surface (OSD, settings,
 * control-center) should call this instead of reading `dev->desc` directly.
 * `dev` may be NULL (returns ""). */
const char *dc_audio_display_name(const dc_audio_device *dev);

#endif /* DC_SERVICES_AUDIO_H */
