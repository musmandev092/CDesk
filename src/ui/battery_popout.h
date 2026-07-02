/* battery_popout.h — the Battery popout (docs/13-POPOUTS-SPEC.md sec.2).
 *
 * A small wlr-layer-shell overlay surface, bar-adjacent like the control
 * center (ui/popout.h), showing the big percent/status header, Health/
 * Capacity stat cards (services/battery.h sysfs data), and a Power Saver /
 * Balanced / Performance segmented control (power-profiles-daemon via
 * `powerprofilesctl`). Opened by clicking the bar's battery chip -- see
 * main.c's DC_BAR_REGION_BATTERY routing.
 */
#ifndef DC_UI_BATTERY_POPOUT_H
#define DC_UI_BATTERY_POPOUT_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;

typedef struct dc_battery_popout dc_battery_popout;

dc_battery_popout *dc_battery_popout_create(struct dc_wayland *wl, struct dc_egl *egl,
                                            struct dc_render *render);
void dc_battery_popout_destroy(dc_battery_popout *bp);

/* Show on `output` if hidden, hide if shown. */
void dc_battery_popout_toggle(dc_battery_popout *bp, struct dc_output *output);
void dc_battery_popout_hide(dc_battery_popout *bp);
bool dc_battery_popout_visible(dc_battery_popout *bp);

/* The popup's wl_surface (for matching pointer events). */
struct wl_surface *dc_battery_popout_surface(dc_battery_popout *bp);

/* Handle a left click at surface-local logical coordinates (close button,
 * power-profile segments). */
void dc_battery_popout_handle_click(dc_battery_popout *bp, double x, double y);

#endif /* DC_UI_BATTERY_POPOUT_H */
