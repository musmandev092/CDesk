/* hover.h — shared pointer-hover background color for popout surfaces.
 *
 * bar.c's draw_hover_overlay() established the DMS hover formula:
 * withAlpha(blend(base, primary, 0.10), max(0.30, widgetTransparency)).
 * Control center / notification center / clipboard picker all need the same
 * formula for their own tiles/cards/buttons — pulled out here once rather
 * than re-derived per file (docs/12-BAR-SPEC.md sec.3, docs/13-POPOUTS-
 * SPEC.md).
 */
#ifndef DC_UI_HOVER_H
#define DC_UI_HOVER_H

#include "theme/theme.h"

/* `base` is typically the panel's own resting background for the hovered
 * element (e.g. surfaceContainerHigh); `primary` is the theme accent;
 * `widget_alpha` is the config's widget-transparency knob
 * (dc_config.bar_widget_transparency), reused here for popouts too since
 * they share the same design token. Returns straight RGBA bytes; callers
 * convert to their own color type (nanovg's NVGcolor, etc). */
dc_color dc_hover_bg_color(dc_color base, dc_color primary, float widget_alpha);

#endif /* DC_UI_HOVER_H */
