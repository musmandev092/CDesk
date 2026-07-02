#include "ui/hover.h"

#include <math.h>

dc_color dc_hover_bg_color(dc_color base, dc_color primary, float widget_alpha)
{
    const float blend_t = 0.10f;
    float a = widget_alpha > 0.30f ? widget_alpha : 0.30f;
    if (a > 1.0f)
        a = 1.0f;

    dc_color out;
    out.r = (uint8_t)lroundf((float)base.r + ((float)primary.r - (float)base.r) * blend_t);
    out.g = (uint8_t)lroundf((float)base.g + ((float)primary.g - (float)base.g) * blend_t);
    out.b = (uint8_t)lroundf((float)base.b + ((float)primary.b - (float)base.b) * blend_t);
    out.a = (uint8_t)lroundf(a * 255.0f);
    return out;
}
