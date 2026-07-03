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

dc_osd *dc_osd_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render);
void dc_osd_destroy(dc_osd *osd);

/* Register the auto-hide timer with the event loop. */
void dc_osd_integrate(dc_osd *osd, struct dc_loop *loop);

/* Show the volume OSD on `output` and (re)start the auto-hide timer. */
void dc_osd_show_volume(dc_osd *osd, struct dc_output *output, int volume, bool muted);

/* Show the brightness OSD on `output` with the given brightness percent (0-100)
 * and (re)start the auto-hide timer. */
void dc_osd_show_brightness(dc_osd *osd, struct dc_output *output, int brightness);

#endif /* DC_UI_OSD_H */
