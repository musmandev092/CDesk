/* bar.h — the DankBar: one wlr-layer-shell surface per output, anchored to the
 * top edge, GPU-rendered via nanovg. Milestone 2 draws the themed background
 * plus a live clock; workspaces and more widgets follow.
 */
#ifndef DC_UI_BAR_BAR_H
#define DC_UI_BAR_BAR_H

struct dc_wayland;
struct dc_output;
struct dc_egl;
struct dc_render;
struct dc_niri;
struct dc_tray;
struct dc_notifications;

typedef struct dc_bar dc_bar;

/* Create a bar on `output`. `niri` may be NULL (workspaces then omitted).
 * Returns NULL on failure. Owned by the caller. */
dc_bar *dc_bar_create(struct dc_wayland *wl, struct dc_output *output, struct dc_egl *egl,
                      struct dc_render *render, struct dc_niri *niri);
void dc_bar_destroy(dc_bar *bar);

/* Attach the tray host so the bar renders StatusNotifier items. Optional. */
void dc_bar_set_tray(dc_bar *bar, struct dc_tray *tray);

/* Attach the notification server so notificationButton can show the unread
 * dot (docs/12-BAR-SPEC.md sec.4/6). Optional. */
void dc_bar_set_notifications(dc_bar *bar, struct dc_notifications *notifications);

/* Re-render the bar (e.g. on a clock tick or a compositor event). No-op until
 * the surface has been configured. */
void dc_bar_render(dc_bar *bar);

/* The bar's wl_surface (for matching pointer events). */
struct wl_surface *dc_bar_surface(dc_bar *bar);

/* The output this bar is on. */
struct dc_output *dc_bar_output(dc_bar *bar);

typedef enum {
    DC_BAR_REGION_NONE,
    DC_BAR_REGION_LAUNCHER,
    DC_BAR_REGION_CONTROL_CENTER,
    DC_BAR_REGION_NOTIFICATIONS,
    DC_BAR_REGION_CLIPBOARD,
    DC_BAR_REGION_CLOCK,
    DC_BAR_REGION_WORKSPACE,
    DC_BAR_REGION_TRAY,
    DC_BAR_REGION_MEDIA_PREV,
    DC_BAR_REGION_MEDIA_PLAY,
    DC_BAR_REGION_MEDIA_NEXT,
} dc_bar_region;

/* Which region a surface-local logical coordinate falls in, from the last
 * render's per-widget hit-rect array (docs/12-BAR-SPEC.md sec.5). `out_payload`
 * (may be NULL) receives region-specific extra data: DC_BAR_REGION_WORKSPACE
 * sets it to the clicked capsule's 1-based per-output workspace index;
 * DC_BAR_REGION_TRAY sets it to the item's 0-based index in the most recent
 * dc_tray_items() enumeration (clicking is still a no-op until S6). */
dc_bar_region dc_bar_hittest(dc_bar *bar, double x, double y, int *out_payload);

/* Pointer motion over this bar's surface, in surface-local logical
 * coordinates (docs/12-BAR-SPEC.md sec.3/5): hit-tests, updates the cursor
 * shape, and re-renders — but only when the hovered region actually changed,
 * not on every motion event. Wire to dc_wayland_set_motion_cb() via a
 * surface-matching dispatcher (see main.c's click handler for the pattern). */
void dc_bar_pointer_motion(dc_bar *bar, double x, double y);

/* Pointer left this bar's surface entirely: clears hover state/cursor and
 * re-renders if anything was actually hovered. */
void dc_bar_pointer_leave(dc_bar *bar);

#endif /* DC_UI_BAR_BAR_H */
