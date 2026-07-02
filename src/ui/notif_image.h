/* notif_image.h — cached nanovg textures for notification images.
 *
 * toasts.c (popup cards) and notifcenter.c (history cards) both need to draw
 * a notification's avatar image (image-data hint pixels, or an image-path/
 * app_icon file). Since dc_render's GL context is shared across every bar/
 * panel (render/nvg.h), a single id-keyed cache here avoids decoding/
 * uploading the same texture twice and gives one place to free it correctly.
 */
#ifndef DC_UI_NOTIF_IMAGE_H
#define DC_UI_NOTIF_IMAGE_H

#include "render/nvg.h"
#include "services/notifications.h"

/* Resolve `n`'s avatar image to a cached nanovg image handle: the image-data
 * hint's decoded pixels if present, else the image-path hint's file, else
 * app_icon if it's an absolute path (a bare XDG icon name isn't a usable file
 * path here). Returns 0 if there's nothing to show -- callers fall back to
 * the initial-letter avatar circle. Writes the image's pixel dimensions to
 * out_w/out_h (for aspect-correct placement) when a handle is returned.
 * Must be called with `render`'s GL context current (i.e. from inside a
 * *_render() draw pass, same as every other nvgCreateImage* call in this
 * codebase -- see dashboard.c's ensure_art()). */
int dc_notif_image_get(dc_render *render, const dc_notification *n, int *out_w, int *out_h);

/* Evict + nvgDeleteImage() every cache entry whose notification id is no
 * longer Current or History in `notifications` (dismissed/cleared/evicted),
 * and any entry whose backing pixels/path just changed away from what's
 * cached. Call once per render pass after drawing, GL context still current
 * -- see toasts_render()/nc_render(). */
void dc_notif_image_gc(dc_render *render, dc_notifications *notifications);

#endif /* DC_UI_NOTIF_IMAGE_H */
