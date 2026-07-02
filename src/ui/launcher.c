#include "ui/launcher.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/apps.h"
#include "services/icons.h"
#include "theme/theme.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Sized to comfortably fit the reference screenshot's search + header + ~7
 * rows + footer (docs/13-POPOUTS-SPEC.md sec.6). */
#define DC_LAUNCHER_WIDTH 620
#define DC_LAUNCHER_HEIGHT 620
#define DC_SCALE_BASE 120

#define DC_LAUNCHER_PAD 6.0f    /* shadow room */
#define DC_LAUNCHER_INSET 18.0f /* inner content margin */

#define DC_LAUNCHER_SEARCH_Y 20.0f
#define DC_LAUNCHER_SEARCH_H 46.0f
#define DC_LAUNCHER_HEADER_GAP 14.0f /* search -> section header */
#define DC_LAUNCHER_HEADER_H 26.0f
#define DC_LAUNCHER_LIST_GAP 8.0f /* section header -> row list */
#define DC_LAUNCHER_FOOTER_H 44.0f

/* List-mode row: icon + name(14px) + description(12px, dim) — "Row height
 * ~56px per reference" (docs/13-POPOUTS-SPEC.md sec.6). */
#define DC_LAUNCHER_ROW_H 56.0f
/* Grid-mode cell: icon + single-line name below, 4 columns (sec.6: "grid only
 * if quick: 4-col icon grid"). */
#define DC_LAUNCHER_GRID_COLS 4
#define DC_LAUNCHER_GRID_CELL_H 84.0f

#define DC_LAUNCHER_SCROLL_STEP 56.0f
#define DC_LAUNCHER_QUERY_MAX 128
/* Inset from the screen's left edge when bar-adjacent (docs/13-POPOUTS-SPEC.md
 * sec.0/6: DMS anchors the launcher bottom-left above the bar, not centered). */
#define DC_LAUNCHER_SIDE_MARGIN 12

/* View-mode toggle in the section header (sec.6: "view-mode toggles right
 * (list/grid/compact)"). Only LIST and GRID are implemented; COMPACT is
 * rendered dim and does nothing when clicked (matches the reference's own
 * disabled-looking third icon). */
enum {
    DC_LAUNCHER_VIEW_LIST = 0,
    DC_LAUNCHER_VIEW_GRID = 1,
};

struct dc_launcher {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;
    dc_apps *apps;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width;
    int logical_height;
    int scale120;
    int phys_width;
    int phys_height;

    char query[DC_LAUNCHER_QUERY_MAX];
    const dc_app *results[64];
    int result_count;
    int selected;
    float scroll; /* pixel offset into the (list- or grid-shaped) result list */
    int view_mode;

    /* Header view-toggle hit rects, recorded while drawing (same convention
     * as clip_picker.c's header button rects). */
    float list_btn_x0, list_btn_y0, list_btn_x1, list_btn_y1;
    float grid_btn_x0, grid_btn_y0, grid_btn_x1, grid_btn_y1;

    dc_anim anim;                 /* entrance / exit (fade + scale) */
    struct wl_callback *frame_cb; /* pending frame callback while animating */
    bool closing;                 /* playing the exit animation, then teardown */

    /* Entrance/exit scale-and-fade pivot, bar-position-aware — see
     * controlcenter.c's identical field for the full rationale. */
    float anim_ox, anim_oy;

    bool visible;
    bool configured;
    bool egl_ready;
};

/* Shared layout so render, click hit-test, and hover motion all agree — same
 * convention as clip_picker.c's cp_layout. */
typedef struct {
    float pad, ix, iw;
    float search_y, search_h;
    float header_y, header_h;
    float list_y0, list_y1, list_h;
    float footer_y, footer_h;
} launcher_layout;

static launcher_layout launcher_get_layout(float w, float h)
{
    launcher_layout l;
    l.pad = DC_LAUNCHER_PAD;
    l.ix = DC_LAUNCHER_INSET;
    l.iw = w - 2.0f * l.ix;
    l.search_y = DC_LAUNCHER_SEARCH_Y;
    l.search_h = DC_LAUNCHER_SEARCH_H;
    l.header_y = l.search_y + l.search_h + DC_LAUNCHER_HEADER_GAP;
    l.header_h = DC_LAUNCHER_HEADER_H;
    l.footer_h = DC_LAUNCHER_FOOTER_H;
    l.footer_y = h - l.pad - l.footer_h;
    l.list_y0 = l.header_y + l.header_h + DC_LAUNCHER_LIST_GAP;
    l.list_y1 = l.footer_y - DC_LAUNCHER_LIST_GAP;
    l.list_h = l.list_y1 - l.list_y0;
    return l;
}

static void launcher_render(dc_launcher *l);
static void launcher_teardown(dc_launcher *l);

/* Frame callback: advance the entrance/exit animation one frame. */
static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_launcher *l = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    l->frame_cb = NULL;
    if (!l->visible)
        return;
    if (dc_anim_active(&l->anim)) {
        launcher_render(l);
    } else if (l->closing) {
        launcher_teardown(l); /* exit animation finished */
    }
}

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}
static inline bool in_rect(double x, double y, float x0, float y0, float x1, float y1)
{
    return x1 > x0 && x >= x0 && x <= x1 && y >= y0 && y <= y1;
}
/* dc_render_icon() takes a dc_color (not the NVGcolor tc_alpha() above
 * produces) — same dim/disabled-icon convention as clip_picker.c's local
 * `dc_color dim = ...; dim.a = ...;` pattern, just as a reusable helper. */
static inline dc_color dc_alpha(dc_color c, int a)
{
    c.a = (uint8_t)a;
    return c;
}

static void recompute_physical(dc_launcher *l)
{
    l->phys_width = (l->logical_width * l->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    l->phys_height = (l->logical_height * l->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Total pixel/row extent of the current result set in the active view mode —
 * shared by scroll clamping and the wheel handler. */
static float launcher_content_height(const dc_launcher *l)
{
    if (l->result_count == 0)
        return 0.0f;
    if (l->view_mode == DC_LAUNCHER_VIEW_GRID) {
        int rows =
            (l->result_count + DC_LAUNCHER_GRID_COLS - 1) / DC_LAUNCHER_GRID_COLS;
        return (float)rows * DC_LAUNCHER_GRID_CELL_H;
    }
    return (float)l->result_count * DC_LAUNCHER_ROW_H;
}

/* Result index under logical (x, y), or -1 if outside the list/grid area or
 * past the last result. Mode-aware so click and hover share one formula. */
static int launcher_row_at(const dc_launcher *l, const launcher_layout *lay, double x, double y)
{
    if (y < (double)lay->list_y0 || y > (double)lay->list_y1)
        return -1;
    float ly = (float)y - lay->list_y0 + l->scroll;

    if (l->view_mode == DC_LAUNCHER_VIEW_GRID) {
        if (x < (double)lay->ix || x > (double)(lay->ix + lay->iw))
            return -1;
        float cell_w = lay->iw / DC_LAUNCHER_GRID_COLS;
        int col = (int)(((float)x - lay->ix) / cell_w);
        int row = (int)(ly / DC_LAUNCHER_GRID_CELL_H);
        if (col < 0 || col >= DC_LAUNCHER_GRID_COLS || row < 0)
            return -1;
        int idx = row * DC_LAUNCHER_GRID_COLS + col;
        return (idx >= 0 && idx < l->result_count) ? idx : -1;
    }

    int idx = (int)(ly / DC_LAUNCHER_ROW_H);
    return (idx >= 0 && idx < l->result_count) ? idx : -1;
}

static void run_search(dc_launcher *l)
{
    l->result_count =
        dc_apps_search(l->apps, l->query, l->results,
                       (int)(sizeof(l->results) / sizeof(l->results[0])));
    l->selected = 0;
    l->scroll = 0.0f;
}

/* Keep the selected row/cell scrolled into view (mode-aware). */
static void clamp_scroll(dc_launcher *l)
{
    if (l->selected < 0)
        l->selected = 0;
    if (l->selected >= l->result_count)
        l->selected = l->result_count - 1;
    if (l->selected < 0) {
        l->selected = 0;
        l->scroll = 0.0f;
        return;
    }

    launcher_layout lay = launcher_get_layout((float)l->logical_width, (float)l->logical_height);
    float content_h = launcher_content_height(l);
    float scroll_max = content_h > lay.list_h ? content_h - lay.list_h : 0.0f;

    float row_top, row_bot;
    if (l->view_mode == DC_LAUNCHER_VIEW_GRID) {
        int row = l->selected / DC_LAUNCHER_GRID_COLS;
        row_top = (float)row * DC_LAUNCHER_GRID_CELL_H;
        row_bot = row_top + DC_LAUNCHER_GRID_CELL_H;
    } else {
        row_top = (float)l->selected * DC_LAUNCHER_ROW_H;
        row_bot = row_top + DC_LAUNCHER_ROW_H;
    }
    if (row_top < l->scroll)
        l->scroll = row_top;
    else if (row_bot > l->scroll + lay.list_h)
        l->scroll = row_bot - lay.list_h;
    if (l->scroll < 0.0f)
        l->scroll = 0.0f;
    if (l->scroll > scroll_max)
        l->scroll = scroll_max;
}

/* One list row: icon, name (14px), description (12px, dim) — sec.6. */
static void draw_list_row(dc_launcher *l, const dc_app *app, float x, float y, float w,
                          bool selected)
{
    NVGcontext *vg = l->render->vg;
    const dc_theme *t = dc_theme_current;

    if (selected) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y + 2.0f, w, DC_LAUNCHER_ROW_H - 4.0f, 10.0f);
        nvgFillColor(vg, tc_alpha(t->primary, 46));
        nvgFill(vg);
    }

    float row_cy = y + DC_LAUNCHER_ROW_H / 2.0f;

    /* App icon (PNG/SVG), loaded per-render and freed after the frame. */
    int img = 0;
    char *icon_path = dc_icon_resolve(app->id, 36, 1);
    if (icon_path) {
        img = dc_render_load_icon(l->render, icon_path, 36);
        free(icon_path);
    }
    const float isz = 36.0f;
    const float icx = x + 12.0f;
    if (img > 0) {
        NVGpaint pat = nvgImagePattern(vg, icx, row_cy - isz / 2.0f, isz, isz, 0.0f, img, 1.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, icx, row_cy - isz / 2.0f, isz, isz, 8.0f);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
    } else {
        dc_render_icon(l->render, DC_ICON_APPS, icx + isz / 2.0f, row_cy, 26.0f, t->surface_text,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    }
    if (img > 0)
        nvgDeleteImage(vg, img);

    const float text_x = icx + isz + 14.0f;
    const float text_w = x + w - 10.0f - text_x;
    if (text_w <= 8.0f)
        return;

    nvgSave(vg);
    nvgScissor(vg, text_x, y, text_w, DC_LAUNCHER_ROW_H);

    bool has_desc = app->desc[0] != '\0';
    float name_cy = has_desc ? y + 21.0f : row_cy;

    nvgFontFaceId(vg, l->render->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, text_x, name_cy, app->name, NULL);

    if (has_desc) {
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, tc_alpha(t->surface_text, 140));
        nvgText(vg, text_x, y + 40.0f, app->desc, NULL);
    }

    nvgRestore(vg);
}

/* One grid cell: icon + single-line name below (sec.6, "grid only if quick"). */
static void draw_grid_cell(dc_launcher *l, const dc_app *app, float x, float y, float w, float h,
                           bool selected)
{
    NVGcontext *vg = l->render->vg;
    const dc_theme *t = dc_theme_current;

    if (selected) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 3.0f, y + 3.0f, w - 6.0f, h - 6.0f, 10.0f);
        nvgFillColor(vg, tc_alpha(t->primary, 46));
        nvgFill(vg);
    }

    const float isz = 40.0f;
    const float icx = x + w / 2.0f - isz / 2.0f;
    const float icy = y + 12.0f;

    int img = 0;
    char *icon_path = dc_icon_resolve(app->id, 40, 1);
    if (icon_path) {
        img = dc_render_load_icon(l->render, icon_path, 40);
        free(icon_path);
    }
    if (img > 0) {
        NVGpaint pat = nvgImagePattern(vg, icx, icy, isz, isz, 0.0f, img, 1.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, icx, icy, isz, isz, 9.0f);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
        nvgDeleteImage(vg, img);
    } else {
        dc_render_icon(l->render, DC_ICON_APPS, x + w / 2.0f, icy + isz / 2.0f, 28.0f,
                       t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    }

    nvgSave(vg);
    nvgScissor(vg, x + 4.0f, icy + isz + 4.0f, w - 8.0f, h - (isz + 8.0f));
    nvgFontFaceId(vg, l->render->font_ui);
    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x + w / 2.0f, icy + isz + 6.0f, app->name, NULL);
    nvgRestore(vg);
}

/* Section header: apps icon + "Applications N" (count dim) + right-aligned
 * list/grid/compact view-mode toggles (sec.6). Records the toggle hit rects. */
static void draw_header(dc_launcher *l, const launcher_layout *lay)
{
    NVGcontext *vg = l->render->vg;
    const dc_theme *t = dc_theme_current;
    const float hcy = lay->header_y + lay->header_h / 2.0f;

    dc_render_icon(l->render, DC_ICON_APPS, lay->ix, hcy, 16.0f, dc_alpha(t->surface_text, 180),
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    nvgFontFaceId(vg, l->render->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    const float label_x = lay->ix + 22.0f;
    nvgText(vg, label_x, hcy, "Applications", NULL);

    float bounds[4];
    nvgTextBounds(vg, label_x, hcy, "Applications", NULL, bounds);
    char count[16];
    snprintf(count, sizeof(count), "%d", l->result_count);
    nvgFillColor(vg, tc_alpha(t->surface_text, 130));
    nvgText(vg, bounds[2] + 8.0f, hcy, count, NULL);

    /* Right-aligned view-mode toggles: list (implemented), grid (implemented),
     * compact (TODO — dim, disabled). */
    const float btn = 22.0f, gap = 6.0f;
    float bx = lay->ix + lay->iw - btn;

    /* compact (rightmost, disabled) */
    nvgBeginPath(vg);
    dc_render_icon(l->render, DC_ICON_VIEW_MODULE, bx + btn / 2.0f, hcy, 15.0f,
                   dc_alpha(t->surface_text, 60), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    bx -= btn + gap;
    l->grid_btn_x0 = bx;
    l->grid_btn_y0 = hcy - btn / 2.0f;
    l->grid_btn_x1 = bx + btn;
    l->grid_btn_y1 = hcy + btn / 2.0f;
    if (l->view_mode == DC_LAUNCHER_VIEW_GRID) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, l->grid_btn_x0, l->grid_btn_y0, btn, btn, 6.0f);
        nvgFillColor(vg, tc_alpha(t->primary, 46));
        nvgFill(vg);
    }
    dc_render_icon(l->render, DC_ICON_GRID_VIEW, bx + btn / 2.0f, hcy, 15.0f,
                   l->view_mode == DC_LAUNCHER_VIEW_GRID ? t->primary
                                                          : dc_alpha(t->surface_text, 160),
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    bx -= btn + gap;
    l->list_btn_x0 = bx;
    l->list_btn_y0 = hcy - btn / 2.0f;
    l->list_btn_x1 = bx + btn;
    l->list_btn_y1 = hcy + btn / 2.0f;
    if (l->view_mode == DC_LAUNCHER_VIEW_LIST) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, l->list_btn_x0, l->list_btn_y0, btn, btn, 6.0f);
        nvgFillColor(vg, tc_alpha(t->primary, 46));
        nvgFill(vg);
    }
    dc_render_icon(l->render, DC_ICON_VIEW_LIST, bx + btn / 2.0f, hcy, 15.0f,
                   l->view_mode == DC_LAUNCHER_VIEW_LIST ? t->primary
                                                          : dc_alpha(t->surface_text, 160),
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

/* Footer: source-filter pills ("All / Apps / Files / Plugins", only Apps is
 * live — the rest are TODO/disabled) + right-aligned keybind hints (sec.6). */
static void draw_footer(dc_launcher *l, const launcher_layout *lay)
{
    NVGcontext *vg = l->render->vg;
    const dc_theme *t = dc_theme_current;
    const float fcy = lay->footer_y + lay->footer_h / 2.0f;

    /* Divider above the footer. */
    nvgBeginPath(vg);
    nvgMoveTo(vg, lay->ix, lay->footer_y);
    nvgLineTo(vg, lay->ix + lay->iw, lay->footer_y);
    nvgStrokeColor(vg, tc_alpha(t->outline, 50));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    nvgFontFaceId(vg, l->render->font_ui);
    nvgFontSize(vg, 12.5f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    float px = lay->ix;

    /* "All" — dim, not a filled pill (matches reference: unhighlighted). */
    dc_render_icon(l->render, DC_ICON_SEARCH, px + 8.0f, fcy, 13.0f, dc_alpha(t->surface_text, 150),
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 150));
    nvgText(vg, px + 22.0f, fcy, "All", NULL);
    float b[4];
    nvgTextBounds(vg, px + 22.0f, fcy, "All", NULL, b);
    px = b[2] + 18.0f;

    /* "Apps" — active, solid primary pill. */
    const char *apps_label = "Apps";
    nvgFontSize(vg, 12.5f);
    nvgTextBounds(vg, 0.0f, 0.0f, apps_label, NULL, b);
    float apps_text_w = b[2] - b[0];
    float apps_pill_w = 22.0f + apps_text_w + 16.0f;
    float apps_pill_h = 26.0f;
    float apps_pill_y = fcy - apps_pill_h / 2.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, px, apps_pill_y, apps_pill_w, apps_pill_h, apps_pill_h / 2.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);
    dc_render_icon(l->render, DC_ICON_GRID_VIEW, px + 16.0f, fcy, 13.0f, t->primary_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->primary_text));
    nvgText(vg, px + 26.0f, fcy, apps_label, NULL);
    px += apps_pill_w + 16.0f;

    /* "Files" — dim, disabled/TODO. */
    dc_render_icon(l->render, DC_ICON_FOLDER, px + 8.0f, fcy, 13.0f, dc_alpha(t->surface_text, 110),
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 110));
    nvgText(vg, px + 22.0f, fcy, "Files", NULL);
    nvgTextBounds(vg, px + 22.0f, fcy, "Files", NULL, b);
    px = b[2] + 18.0f;

    /* "Plugins" — dim, disabled/TODO. */
    dc_render_icon(l->render, DC_ICON_EXTENSION, px + 8.0f, fcy, 13.0f,
                   dc_alpha(t->surface_text, 110), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 110));
    nvgText(vg, px + 22.0f, fcy, "Plugins", NULL);

    /* Right-aligned keybind hints. */
    nvgFontSize(vg, 11.5f);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 120));
    nvgText(vg, lay->ix + lay->iw, fcy, "\xe2\x86\x91\xe2\x86\x93 nav \xc2\xb7 \xe2\x8f\x8e open \xc2\xb7 Tab actions",
            NULL);
}

static void launcher_render(dc_launcher *l)
{
    if (!l->configured || l->phys_width <= 0)
        return;
    if (!l->egl_ready) {
        if (!dc_egl_window_init(&l->egl_window, l->egl, l->surface, l->phys_width, l->phys_height))
            return;
        l->egl_ready = true;
    } else {
        dc_egl_window_resize(&l->egl_window, l->phys_width, l->phys_height);
    }
    if (!dc_egl_make_current(l->egl, &l->egl_window))
        return;
    if (!dc_render_ensure(l->render))
        return;
    if (l->viewport)
        wp_viewport_set_destination(l->viewport, l->logical_width, l->logical_height);

    NVGcontext *vg = l->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = l->logical_width;
    const float h = l->logical_height;
    const float pad = DC_LAUNCHER_PAD;

    glViewport(0, 0, l->phys_width, l->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)l->scale120 / DC_SCALE_BASE);

    /* Entrance/exit animation: fade + scale from the bar-facing edge
     * (docs/13-POPOUTS-SPEC.md sec.0/6 — bar-adjacent bottom-left, not a
     * centered spotlight). While closing, the progress runs in reverse
     * (1 -> 0). */
    float p = dc_anim_progress(&l->anim);
    if (l->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.92f + 0.08f * p;
    float ox = pad + (w - 2.0f * pad) * l->anim_ox;
    float oy = pad + (h - 2.0f * pad) * l->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Drop shadow + card. */
    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 16.0f, 20.0f,
                                     nvgRGBA(0, 0, 0, 110), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 16.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 16.0f);
    nvgFillColor(vg, tc(t->surface_container));
    nvgFill(vg);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    launcher_layout lay = launcher_get_layout(w, h);

    /* Search field: rounded, magnifier icon, primary border while focused —
     * matches clip_picker.c's search field (the layer surface grabs keyboard
     * exclusively while open, so it's always in the "focused" state). */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, lay.ix, lay.search_y, lay.iw, lay.search_h, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    nvgStrokeColor(vg, tc(t->primary));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);

    const float scy = lay.search_y + lay.search_h / 2.0f;
    dc_render_icon(l->render, DC_ICON_SEARCH, lay.ix + 16.0f, scy, 20.0f, t->surface_variant_text,
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    const float tx = lay.ix + 46.0f;
    nvgFontFaceId(vg, l->render->font_ui);
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    if (l->query[0]) {
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, tx, scy, l->query, NULL);
        float bounds[4];
        nvgTextBounds(vg, tx, scy, l->query, NULL, bounds);
        nvgBeginPath(vg);
        nvgRect(vg, bounds[2] + 2.0f, scy - 10.0f, 2.0f, 20.0f);
        nvgFillColor(vg, tc(t->primary));
        nvgFill(vg);
    } else {
        nvgFillColor(vg, tc_alpha(t->surface_text, 110));
        nvgText(vg, tx, scy, "Search applications\xe2\x80\xa6", NULL);
    }

    draw_header(l, &lay);

    /* Result list/grid, scrollable + scissored to the list area. */
    float content_h = launcher_content_height(l);
    float scroll_max = content_h > lay.list_h ? content_h - lay.list_h : 0.0f;
    if (l->scroll < 0.0f)
        l->scroll = 0.0f;
    if (l->scroll > scroll_max)
        l->scroll = scroll_max;

    if (l->result_count == 0) {
        nvgFontFaceId(vg, l->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 120));
        nvgText(vg, w / 2.0f, lay.list_y0 + lay.list_h / 2.0f, "No matching applications", NULL);
    } else {
        nvgSave(vg);
        nvgScissor(vg, lay.ix, lay.list_y0, lay.iw, lay.list_h);

        if (l->view_mode == DC_LAUNCHER_VIEW_GRID) {
            float cell_w = lay.iw / DC_LAUNCHER_GRID_COLS;
            for (int i = 0; i < l->result_count; i++) {
                int row = i / DC_LAUNCHER_GRID_COLS;
                int col = i % DC_LAUNCHER_GRID_COLS;
                float cx = lay.ix + (float)col * cell_w;
                float cy = lay.list_y0 + (float)row * DC_LAUNCHER_GRID_CELL_H - l->scroll;
                if (cy + DC_LAUNCHER_GRID_CELL_H < lay.list_y0 || cy > lay.list_y1)
                    continue;
                draw_grid_cell(l, l->results[i], cx, cy, cell_w, DC_LAUNCHER_GRID_CELL_H,
                               i == l->selected);
            }
        } else {
            for (int i = 0; i < l->result_count; i++) {
                float ry = lay.list_y0 + (float)i * DC_LAUNCHER_ROW_H - l->scroll;
                if (ry + DC_LAUNCHER_ROW_H < lay.list_y0 || ry > lay.list_y1)
                    continue;
                draw_list_row(l, l->results[i], lay.ix, ry, lay.iw, i == l->selected);
            }
        }

        nvgRestore(vg);

        if (scroll_max > 0.0f) {
            float track_x = lay.ix + lay.iw - 3.0f;
            float thumb_h = lay.list_h * (lay.list_h / content_h);
            if (thumb_h < 24.0f)
                thumb_h = 24.0f;
            float thumb_y = lay.list_y0 + (lay.list_h - thumb_h) * (l->scroll / scroll_max);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, track_x, thumb_y, 3.0f, thumb_h, 1.5f);
            nvgFillColor(vg, tc_alpha(t->outline, 140));
            nvgFill(vg);
        }
    }

    draw_footer(l, &lay);

    nvgEndFrame(vg);

    /* While animating (or finishing a close), ask for a frame callback so the
     * next frame is drawn; the loop drives it via frame_done. The extra frame
     * when closing lets frame_done run the teardown after the last frame. */
    if ((dc_anim_active(&l->anim) || l->closing) && !l->frame_cb) {
        l->frame_cb = wl_surface_frame(l->surface);
        wl_callback_add_listener(l->frame_cb, &frame_listener, l);
    }
    dc_egl_swap(l->egl, &l->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_launcher *l = data;
    DC_UNUSED(fs);
    l->scale120 = (int)scale;
    recompute_physical(l);
    launcher_render(l);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_launcher *l = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    l->logical_width = width > 0 ? (int)width : DC_LAUNCHER_WIDTH;
    l->logical_height = height > 0 ? (int)height : DC_LAUNCHER_HEIGHT;
    l->configured = true;
    recompute_physical(l);
    launcher_render(l);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_launcher *l = data;
    DC_UNUSED(surface);
    l->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_launcher *dc_launcher_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_launcher *l = calloc(1, sizeof(*l));
    l->wl = wl;
    l->egl = egl;
    l->render = render;
    l->apps = dc_apps_load();
    l->logical_width = DC_LAUNCHER_WIDTH;
    l->logical_height = DC_LAUNCHER_HEIGHT;
    l->scale120 = DC_SCALE_BASE;
    l->view_mode = DC_LAUNCHER_VIEW_LIST;
    return l;
}

static void launcher_show(dc_launcher *l, dc_output *output)
{
    l->output = output;
    l->configured = false;
    l->egl_ready = false;
    l->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    l->query[0] = '\0';
    run_search(l);
    dc_anim_start(&l->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    l->surface = wl_compositor_create_surface(l->wl->compositor);
    if (l->wl->fractional_scale_mgr) {
        l->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            l->wl->fractional_scale_mgr, l->surface);
        wp_fractional_scale_v1_add_listener(l->fractional_scale, &fractional_scale_listener, l);
    }
    if (l->wl->viewporter)
        l->viewport = wp_viewporter_get_viewport(l->wl->viewporter, l->surface);

    l->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        l->wl->layer_shell, l->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:launcher");

    /* Bar-adjacent, left-aligned (docs/13-POPOUTS-SPEC.md sec.0/6): DMS opens
     * the launcher bottom-left above the bar, not centered, for both bar
     * positions. */
    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_START, DC_LAUNCHER_SIDE_MARGIN);
    l->anim_ox = pa.origin_x;
    l->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(l->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(l->layer_surface, DC_LAUNCHER_WIDTH, DC_LAUNCHER_HEIGHT);
    zwlr_layer_surface_v1_set_margin(l->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        l->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(l->layer_surface, &layer_surface_listener, l);

    wl_surface_commit(l->surface);
    l->visible = true;
    l->closing = false;
    dc_debug("launcher shown");
}

static void launcher_teardown(dc_launcher *l)
{
    if (l->frame_cb) {
        wl_callback_destroy(l->frame_cb);
        l->frame_cb = NULL;
    }
    if (l->egl_ready)
        dc_egl_window_finish(&l->egl_window, l->egl);
    if (l->viewport)
        wp_viewport_destroy(l->viewport);
    if (l->fractional_scale)
        wp_fractional_scale_v1_destroy(l->fractional_scale);
    if (l->layer_surface)
        zwlr_layer_surface_v1_destroy(l->layer_surface);
    if (l->surface)
        wl_surface_destroy(l->surface);
    l->egl_ready = false;
    l->configured = false;
    l->viewport = NULL;
    l->fractional_scale = NULL;
    l->layer_surface = NULL;
    l->surface = NULL;
    l->visible = false;
    l->closing = false;
    dc_debug("launcher hidden");
}

/* Begin the exit animation; teardown happens once it completes (or immediately
 * when animations are disabled). */
static void launcher_begin_close(dc_launcher *l)
{
    if (!l->visible || l->closing)
        return;
    dc_anim_start(&l->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    l->closing = true;
    if (!dc_anim_active(&l->anim)) {
        launcher_teardown(l);
        return;
    }
    launcher_render(l);
}

void dc_launcher_toggle(dc_launcher *l, dc_output *output)
{
    if (l->visible)
        launcher_begin_close(l);
    else
        launcher_show(l, output);
}

void dc_launcher_hide(dc_launcher *l)
{
    launcher_begin_close(l);
}

bool dc_launcher_visible(dc_launcher *l)
{
    return l->visible;
}

struct wl_surface *dc_launcher_surface(dc_launcher *l)
{
    return l->surface;
}

void dc_launcher_handle_key(dc_launcher *l, uint32_t keysym, const char *utf8)
{
    if (!l->visible || l->closing)
        return;

    switch (keysym) {
    case XKB_KEY_Escape:
        launcher_begin_close(l);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (l->selected >= 0 && l->selected < l->result_count) {
            dc_app_launch(l->results[l->selected]);
            launcher_begin_close(l);
        }
        return;
    case XKB_KEY_Tab:
        /* "Tab actions" footer hint is a TODO (sec.6) -- no-op for now,
         * rather than inserting a tab character into the search field. */
        return;
    case XKB_KEY_BackSpace: {
        size_t n = strlen(l->query);
        if (n > 0) {
            l->query[n - 1] = '\0';
            run_search(l);
        }
        break;
    }
    case XKB_KEY_Up:
        l->selected -= (l->view_mode == DC_LAUNCHER_VIEW_GRID) ? DC_LAUNCHER_GRID_COLS : 1;
        clamp_scroll(l);
        break;
    case XKB_KEY_Down:
        l->selected += (l->view_mode == DC_LAUNCHER_VIEW_GRID) ? DC_LAUNCHER_GRID_COLS : 1;
        clamp_scroll(l);
        break;
    case XKB_KEY_Left:
        if (l->view_mode == DC_LAUNCHER_VIEW_GRID) {
            l->selected--;
            clamp_scroll(l);
        }
        break;
    case XKB_KEY_Right:
        if (l->view_mode == DC_LAUNCHER_VIEW_GRID) {
            l->selected++;
            clamp_scroll(l);
        }
        break;
    default: {
        /* Append printable text (single-byte control chars filtered). */
        if (utf8 && utf8[0] && !((unsigned char)utf8[0] < 0x20) && (unsigned char)utf8[0] != 0x7f) {
            size_t n = strlen(l->query);
            size_t add = strlen(utf8);
            if (n + add < sizeof(l->query)) {
                memcpy(l->query + n, utf8, add + 1);
                run_search(l);
            }
        }
        break;
    }
    }
    launcher_render(l);
}

void dc_launcher_handle_click(dc_launcher *l, double x, double y)
{
    if (!l->visible || l->closing)
        return;

    if (in_rect(x, y, l->list_btn_x0, l->list_btn_y0, l->list_btn_x1, l->list_btn_y1)) {
        if (l->view_mode != DC_LAUNCHER_VIEW_LIST) {
            l->view_mode = DC_LAUNCHER_VIEW_LIST;
            l->scroll = 0.0f;
            launcher_render(l);
        }
        return;
    }
    if (in_rect(x, y, l->grid_btn_x0, l->grid_btn_y0, l->grid_btn_x1, l->grid_btn_y1)) {
        if (l->view_mode != DC_LAUNCHER_VIEW_GRID) {
            l->view_mode = DC_LAUNCHER_VIEW_GRID;
            l->scroll = 0.0f;
            launcher_render(l);
        }
        return;
    }

    launcher_layout lay = launcher_get_layout((float)l->logical_width, (float)l->logical_height);
    int idx = launcher_row_at(l, &lay, x, y);
    if (idx >= 0) {
        dc_app_launch(l->results[idx]);
        launcher_begin_close(l);
    }
}

void dc_launcher_handle_motion(dc_launcher *l, double x, double y)
{
    if (!l->visible || l->closing)
        return;
    launcher_layout lay = launcher_get_layout((float)l->logical_width, (float)l->logical_height);
    int idx = launcher_row_at(l, &lay, x, y);
    if (idx < 0 || idx == l->selected)
        return;
    l->selected = idx;
    launcher_render(l);
}

void dc_launcher_handle_scroll(dc_launcher *l, int steps_v)
{
    if (!l->visible || l->closing || steps_v == 0)
        return;
    launcher_layout lay = launcher_get_layout((float)l->logical_width, (float)l->logical_height);
    float content_h = launcher_content_height(l);
    float scroll_max = content_h > lay.list_h ? content_h - lay.list_h : 0.0f;
    float s = l->scroll + (float)steps_v * DC_LAUNCHER_SCROLL_STEP;
    if (s < 0.0f)
        s = 0.0f;
    if (s > scroll_max)
        s = scroll_max;
    if (s == l->scroll)
        return;
    l->scroll = s;
    launcher_render(l);
}

void dc_launcher_destroy(dc_launcher *l)
{
    if (!l)
        return;
    if (l->visible)
        launcher_teardown(l);
    dc_apps_destroy(l->apps);
    free(l);
}
