/* osd.h — transient on-screen display (volume/brightness), bottom-center, auto-hides.
 *
 * A short-lived wlr-layer-shell overlay shown when volume or brightness changes,
 * matching DMS's VolumeOSD. See docs/04-FEATURES §4 and docs/POLISH.md P5.
 */
#ifndef DC_UI_OSD_H
#define DC_UI_OSD_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct dc_loop;

typedef struct dc_osd dc_osd;

/* Screen position (docs/14-COMPLETION-PLAN.md W2.3, config.h's osd_position).
 * Only the 4 corners/edges reachable with a plain layer-shell anchor (no
 * extra horizontal-centering math) are implemented, vs. DMS's 8-way enum. */
typedef enum {
    DC_OSD_POS_BOTTOM_CENTER = 0, /* default, matches the old hardcoded behavior */
    DC_OSD_POS_BOTTOM_LEFT,
    DC_OSD_POS_BOTTOM_RIGHT,
    DC_OSD_POS_TOP_CENTER,
} dc_osd_position;

dc_osd *dc_osd_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render);
void dc_osd_destroy(dc_osd *osd);

/* Register the auto-hide timer with the event loop. */
void dc_osd_integrate(dc_osd *osd, struct dc_loop *loop);

/* Show the volume OSD on `output` and (re)start the auto-hide timer. */
void dc_osd_show_volume(dc_osd *osd, struct dc_output *output, int volume, bool muted);

/* Show the brightness OSD on `output` with the given brightness percent (0-100)
 * and (re)start the auto-hide timer. */
void dc_osd_show_brightness(dc_osd *osd, struct dc_output *output, int brightness);

/* The variants below reuse the exact same layer-surface/positioning/timeout/
 * animation infra as volume and brightness above, just with an icon + short
 * text label instead of a progress bar + percent (docs task: "extend OSD for
 * mic mute / media / power profile / output switch"). */

/* Mic (default source) mute toggled. */
void dc_osd_show_mic_mute(dc_osd *osd, struct dc_output *output, bool muted);

/* Media playback status transitioned (playing<->paused). `title` may be NULL
 * or empty (falls back to a generic "Playing"/"Paused" label); long titles
 * are ellipsized to fit, same as elsewhere in the codebase. */
void dc_osd_show_media(dc_osd *osd, struct dc_output *output, bool playing, const char *title);

/* Power profile switched to a new mode. `label` is a human string, e.g. from
 * services/power.h's dc_power_mode_label(). */
void dc_osd_show_power_profile(dc_osd *osd, struct dc_output *output, const char *label);

/* Default audio output device changed. `device_name` is the human-readable
 * sink name (e.g. from `wpctl inspect`'s node.description). */
void dc_osd_show_output_switch(dc_osd *osd, struct dc_output *output, const char *device_name);

#endif /* DC_UI_OSD_H */
