#include "ui/connected.h"

#include "core/config.h"
#include "theme/theme.h"
#include "ui/material_bg.h"

#include "nanovg.h"

/* Must match bar.c's DC_BAR_CORNER_RADIUS (static there, not exported) --
 * the connector fillets and the card's far-corner rounding both need to
 * equal the bar's own corner radius for the "emerges from the bar" illusion
 * to read as one continuous curve (docs/27-CONNECTED-FRAME-PLAN.md G2/G3). */
#define DC_CONNECTED_CORNER_RADIUS 12.0f

/* Fill a `radius` x `radius` square minus an inscribed circle of the same
 * radius, hole center placed `radius` px inward on each axis from the
 * square's outer corner (ox, oy); (dx, dy) give that corner's sign so the
 * same routine works for all four screen/card corners. The remaining
 * (square minus circle) sliver is a concave quarter-fillet -- same
 * technique frame.c's draw_corner() uses to mask screen corners round,
 * repurposed here as a *filled* connector piece instead of a punched-out
 * mask, so it reads as solid card material bridging the bar's rounded
 * corner into the popout's square near corner. */
static void fill_corner_connector(NVGcontext *vg, float ox, float oy, float dx, float dy,
                                  float radius)
{
    float sx = (dx > 0.0f) ? ox : ox - radius;
    float sy = (dy > 0.0f) ? oy : oy - radius;
    float hole_cx = sx + ((dx > 0.0f) ? radius : 0.0f);
    float hole_cy = sy + ((dy > 0.0f) ? radius : 0.0f);

    nvgBeginPath(vg);
    nvgRect(vg, sx, sy, radius, radius);
    nvgCircle(vg, hole_cx, hole_cy, radius);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFill(vg);
}

/* Today's per-panel floating chrome (pad=6 shadow + material fill(12) + 1px
 * outline), reproduced byte-for-byte so this is a true no-op when
 * connected_frame is off (see controlcenter.c's ~L1721-1762 block, the
 * reference this was extracted from -- controlcenter.c itself is untouched
 * by this task; only new callers in future tasks converge on this copy). */
static void draw_floating_chrome(NVGcontext *vg, struct dc_render *render, float w, float h)
{
    const dc_theme *t = dc_theme_current;
    const float pad = 6.0f;

    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2.0f * pad, h - 2.0f * pad, 12.0f,
                                     18.0f, nvgRGBA(0, 0, 0, 90), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0.0f, 0.0f, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2.0f * pad, h - 2.0f * pad, 12.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    dc_material_bg_fill_card(vg, render, pad, pad, w - 2.0f * pad, h - 2.0f * pad, 12.0f);
    nvgStrokeColor(vg, nvgRGBA(t->outline.r, t->outline.g, t->outline.b, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
}

/* Connected-mode chrome: square near corners flush with the bar (pad_near=0),
 * rounded far corners, two connector fillets at the near corners, a
 * scissored shadow, and a 3-side open outline ending at the connector tips.
 * Pads mirror dc_popout_chrome_pads()'s connected branch: near=0, side=12,
 * far=6. */
static void draw_stitched_chrome(NVGcontext *vg, struct dc_render *render, float w, float h,
                                 bool bottom_bar)
{
    const dc_theme *t = dc_theme_current;
    const float r = DC_CONNECTED_CORNER_RADIUS;
    const float pad_side = 12.0f;
    const float pad_far = 6.0f;

    const float cx = pad_side;
    const float cw = w - 2.0f * pad_side;
    const float cy = bottom_bar ? pad_far : 0.0f;
    const float ch = h - pad_far; /* pad_near is always 0 */

    /* far corners rounded, near corners square */
    const float r_tl = bottom_bar ? r : 0.0f;
    const float r_tr = bottom_bar ? r : 0.0f;
    const float r_br = bottom_bar ? 0.0f : r;
    const float r_bl = bottom_bar ? 0.0f : r;

    /* Shadow: identical 2-layer box-gradient technique as the floating
     * chrome/bar shadow, but scissored to exclude the near-edge band (height
     * = connector radius, full width) so the halo never darkens the seam or
     * the bar itself (docs/27-CONNECTED-FRAME-PLAN.md G4). */
    nvgSave(vg);
    if (bottom_bar)
        nvgScissor(vg, 0.0f, 0.0f, w, h - r);
    else
        nvgScissor(vg, 0.0f, r, w, h - r);

    struct {
        float blur, offset, alpha;
    } layers[2] = {
        {14.0f, 0.0f, 0.125f},
        {8.0f, bottom_bar ? -4.0f : 4.0f, 0.25f},
    };
    for (int i = 0; i < 2; i++) {
        float blur = layers[i].blur;
        float oy = cy + layers[i].offset;
        unsigned char a = (unsigned char)(layers[i].alpha * 255.0f);

        NVGpaint paint = nvgBoxGradient(vg, cx, oy, cw, ch, r, blur, nvgRGBA(0, 0, 0, a),
                                        nvgRGBA(0, 0, 0, 0));
        nvgBeginPath(vg);
        nvgRect(vg, cx - blur, oy - blur, cw + 2.0f * blur, ch + 2.0f * blur);
        nvgRoundedRectVarying(vg, cx, cy, cw, ch, r_tl, r_tr, r_br, r_bl);
        nvgPathWinding(vg, NVG_HOLE);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }
    nvgRestore(vg);

    /* Card fill (material bg when enabled, else flat surfaceContainer). */
    dc_material_bg_fill_card_varying(vg, render, cx, cy, cw, ch, r_tl, r_tr, r_br, r_bl);

    /* Connectors: flat surfaceContainer fill (T9 polish: lerp/align with the
     * material image when material_blur is on -- see docs/27-CONNECTED-
     * FRAME-PLAN.md task list; the flat approximation here already matches
     * the card's own non-material fallback exactly). */
    nvgFillColor(vg, nvgRGBA(t->surface_container.r, t->surface_container.g,
                             t->surface_container.b, 255));
    if (!bottom_bar) {
        fill_corner_connector(vg, 0.0f, 0.0f, 1.0f, 1.0f, r);
        fill_corner_connector(vg, w, 0.0f, -1.0f, 1.0f, r);
    } else {
        fill_corner_connector(vg, 0.0f, h, 1.0f, -1.0f, r);
        fill_corner_connector(vg, w, h, -1.0f, -1.0f, r);
    }

    /* 3-side open outline: starts/ends exactly at the connector tips (the
     * card's square near corners), leaving the near edge itself unstroked
     * since it merges into the bar. nvgArcTo degrades to a plain lineTo when
     * a radius is 0 (nanovg.c), so this also works unchanged if far-corner
     * radius were ever 0. */
    nvgBeginPath(vg);
    if (!bottom_bar) {
        nvgMoveTo(vg, cx, cy);
        nvgArcTo(vg, cx, cy + ch, cx + cw, cy + ch, r_bl);
        nvgArcTo(vg, cx + cw, cy + ch, cx + cw, cy, r_br);
        nvgLineTo(vg, cx + cw, cy);
    } else {
        nvgMoveTo(vg, cx, cy + ch);
        nvgArcTo(vg, cx, cy, cx + cw, cy, r_tl);
        nvgArcTo(vg, cx + cw, cy, cx + cw, cy + ch, r_tr);
        nvgLineTo(vg, cx + cw, cy + ch);
    }
    nvgStrokeColor(vg, nvgRGBA(t->outline.r, t->outline.g, t->outline.b, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);
}

void dc_connected_card_chrome(NVGcontext *vg, struct dc_render *render, float w, float h,
                              bool bottom_bar)
{
    if (!dc_config_current->connected_frame) {
        draw_floating_chrome(vg, render, w, h);
        return;
    }
    draw_stitched_chrome(vg, render, w, h, bottom_bar);
}
