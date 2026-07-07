#include "ui/hover.h"

dc_color dc_hover_bg_color(dc_color base, dc_color primary, float widget_alpha)
{
    /* Material state layer: every caller paints this LAST, on top of the
     * already-drawn element (icon + label included), so it must be a
     * translucent tint -- never an opaque fill. The original DMS formula
     * here -- withAlpha(blend(base, primary, 0.10), max(0.30,
     * widgetTransparency)) -- is a *background replacement* color; painted
     * over content at widgetTransparency ~1.0 it fully covered the hovered
     * element, making every icon "fade out" on hover (reported live
     * 2026-07-07). A ~12%-alpha primary tint gives the same perceived
     * background shift while keeping the content readable underneath. */
    (void)base;
    (void)widget_alpha;
    dc_color out = primary;
    out.a = 31; /* ~12% state-layer alpha */
    return out;
}
