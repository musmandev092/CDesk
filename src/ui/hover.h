/* hover.h — shared pointer-hover state-layer color for the bar + popouts.
 *
 * Every caller paints this on top of the already-drawn element, so it is a
 * translucent Material state-layer tint (primary @ ~12% alpha), NOT the old
 * DMS background-replacement formula — that one, painted over content at
 * widgetTransparency ~1.0, hid the hovered element entirely (see hover.c).
 * Shared here once rather than re-derived per file (docs/12-BAR-SPEC.md
 * sec.3, docs/13-POPOUTS-SPEC.md).
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
