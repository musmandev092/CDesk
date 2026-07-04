#include "ui/notepad.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "render/shape.h"
#include "services/notepad_storage.h"
#include "theme/theme.h"
#include "ui/connected.h"
#include "ui/hover.h"
#include "ui/popout.h"
#include "ui/text_edit.h"
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

/* Sized per docs/22-NOTEPAD-PLAN.md NT3 task spec (480x600), independent of
 * clip_picker's 420x560 -- a text editor wants more room than a clipboard
 * row list. */
#define DC_NP_WIDTH 480
#define DC_NP_HEIGHT 600
#define DC_SCALE_BASE 120
/* Inset from the screen's right edge when bar-adjacent (same convention as
 * clip_picker.c's DC_CP_SIDE_MARGIN). */
#define DC_NP_SIDE_MARGIN 12

#define DC_NP_PAD 6.0f    /* outer gutter for the drop shadow */
#define DC_NP_RADIUS 14.0f
#define DC_NP_INSET 16.0f /* left/right content inset inside the card */

/* Logical surface width. DC_NP_WIDTH already bakes in the floating chrome's
 * flat 6px pad on every side; connected_frame widens the lateral (side) pad
 * to 12 for the connector fillets (dc_popout_chrome_pads()), so the surface
 * needs 2*(pad_side-6) more logical px to keep the card CONTENT rect --
 * inset by pad_side + DC_NP_INSET, see np_get_layout() -- exactly where it
 * sits when floating (mirrors controlcenter.c's cc_surface_width()).
 * connected_frame off: pad_side==6, so this is just DC_NP_WIDTH, unchanged. */
static int np_surface_width(void)
{
    int pad_side = 6;
    dc_popout_chrome_pads(dc_config_current, NULL, &pad_side, NULL);
    return DC_NP_WIDTH + 2 * (pad_side - 6);
}

#define DC_NP_HEADER_TOP 14.0f
#define DC_NP_HEADER_H 28.0f
#define DC_NP_TABSTRIP_GAP 10.0f
#define DC_NP_TABSTRIP_H 36.0f
#define DC_NP_EDITOR_GAP 10.0f
#define DC_NP_EDITOR_INNER_PAD 10.0f
#define DC_NP_FOOTER_H 18.0f
#define DC_NP_BOTTOM_PAD 12.0f

/* Tab chips: <=128px each per task spec, shrinking to fit if more tabs are
 * open than fit at max width (floor DC_NP_TAB_CHIP_MIN_W -- v1 has no
 * horizontal scroll/overflow for the tab strip, see the comment at its
 * layout site). */
#define DC_NP_TAB_CHIP_MAX_W 128.0f
#define DC_NP_TAB_CHIP_MIN_W 48.0f
#define DC_NP_TAB_CHIP_GAP 6.0f
#define DC_NP_PLUS_CHIP_W 32.0f
#define DC_NP_MAX_TAB_HITS 64 /* mirrors DC_NOTEPAD_CAP in notepad_storage.c */

/* Autosave debounce (docs/22 sec.3/NT3): flush once the current tab has been
 * dirty for this long with no further edit. */
#define DC_NOTEPAD_AUTOSAVE_MS 2000

/* ~3 text rows per wheel notch (TE_FONT_SIZE 14 * TE_LINE_HEIGHT_MULT 1.4 in
 * text_edit.c, so one row is ~19.6px). */
#define DC_NP_SCROLL_STEP 58.0f

/* Shared layout so np_render (draw), handle_click (hit-test), and the
 * chip-sizing math all agree -- same convention as clip_picker.c's
 * cp_layout. */
typedef struct {
    float pad_side, ix, iw;
    float header_y, header_h;
    float tabstrip_y, tabstrip_h;
    float editor_x, editor_y, editor_w, editor_h; /* outer box, pre inner-pad */
    float footer_y, footer_h;
} np_layout;

static np_layout np_get_layout(float w, float h)
{
    /* Card-fill padding (docs/27-CONNECTED-FRAME-PLAN.md T5-equivalent):
     * floating chrome reserves a flat 6px of shadow room on all four sides;
     * connected chrome widens the lateral (side) pad to 12 for the
     * connector fillets and drops the bar-facing (near) pad to 0, leaving
     * the far pad at 6 -- see dc_popout_chrome_pads(). Which physical edge
     * is "near" vs "far" swaps with bar_position. */
    int pad_near, pad_side, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, &pad_side, &pad_far);
    const bool bottom_bar = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    const float pad_top = bottom_bar ? (float)pad_far : (float)pad_near;
    const float pad_bottom = bottom_bar ? (float)pad_near : (float)pad_far;
    const float pad_side_f = (float)pad_side;

    np_layout l;
    l.pad_side = pad_side_f;
    l.ix = pad_side_f + DC_NP_INSET;
    l.iw = w - 2.0f * l.ix;
    l.header_y = pad_top + DC_NP_HEADER_TOP;
    l.header_h = DC_NP_HEADER_H;
    l.tabstrip_y = l.header_y + l.header_h + DC_NP_TABSTRIP_GAP;
    l.tabstrip_h = DC_NP_TABSTRIP_H;
    l.footer_h = DC_NP_FOOTER_H;
    l.footer_y = h - pad_bottom - DC_NP_BOTTOM_PAD - l.footer_h;
    l.editor_x = l.ix;
    l.editor_y = l.tabstrip_y + l.tabstrip_h + DC_NP_EDITOR_GAP;
    l.editor_w = l.iw;
    l.editor_h = (l.footer_y - DC_NP_EDITOR_GAP) - l.editor_y;
    return l;
}

/* Per-tab-chip hit-test rects, recomputed every render (same "record while
 * drawing" convention as clip_picker.c's cp_row_hit). */
typedef struct {
    int tab_index; /* index into notepad_storage as of this render */
    float x0, y0, x1, y1;                     /* chip body */
    float close_x0, close_y0, close_x1, close_y1; /* close dot */
} np_tab_hit;

/* Hover ids (docs/22 NT3: hover bg on the header close button, the trailing
 * "+" chip, and each tab chip's body/close-dot). Per-tab sub-regions are
 * packed as NP_HOVER_TAB_BASE + i*2 + {0:body,1:close}, same dynamic-list
 * convention as clip_picker.c's CP_HOVER_ROW_BASE. */
#define NP_HOVER_NONE 0
#define NP_HOVER_CLOSE 1
#define NP_HOVER_NEW_TAB 2
#define NP_HOVER_TAB_BASE 10

struct dc_notepad {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;

    /* Loaded lazily on first show() rather than in create() -- mirrors
     * clip_picker.c's create()-doesn't-touch-the-service-yet split from
     * cp_show()'s refresh_all(). dc_notepad_tick()/flush() are no-ops until
     * then (guarded on storage != NULL). */
    dc_notepad_storage *storage;
    dc_text_edit editor;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width, logical_height, scale120, phys_width, phys_height;

    /* Header hit-test rect (close X). */
    float close_x0, close_y0, close_x1, close_y1;
    /* Trailing "+" (new tab) chip hit-test rect. */
    float new_tab_x0, new_tab_y0, new_tab_x1, new_tab_y1;

    np_tab_hit tab_hits[DC_NP_MAX_TAB_HITS];
    int tab_hit_count;

    /* Editor content rect (post inner-pad, matching exactly what was passed
     * to dc_text_edit_draw()), used to route clicks/scroll into it. */
    float editor_x, editor_y, editor_w, editor_h;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;
    bool visible, configured, egl_ready;

    /* Entrance/exit scale-and-fade pivot, bar-position-aware -- see
     * controlcenter.c's identical field for the full rationale. */
    float anim_ox, anim_oy;

    /* Hover tracking, same guard pattern as bar.c's dc_bar_pointer_motion(). */
    int hover_id;
};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

static void np_render(dc_notepad *np);
static void np_teardown(dc_notepad *np);
static void np_begin_close(dc_notepad *np);

static void np_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_notepad *np = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    np->frame_cb = NULL;
    if (!np->visible)
        return;
    if (dc_anim_active(&np->anim))
        np_render(np);
    else if (np->closing)
        np_teardown(np);
}
static const struct wl_callback_listener np_frame_listener = {.done = np_frame_done};

static void recompute_physical(dc_notepad *np)
{
    np->phys_width = (np->logical_width * np->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    np->phys_height = (np->logical_height * np->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* EGL current-context helper for event handlers that call into text_edit's
 * layout-cache-building entry points (dc_text_edit_key/click) -- required
 * per docs/22 sec.2.2/text_edit.h's "EGL CONTEXT RULE", same guarded
 * precedent as clip_picker.c's cp_purge_thumbnails(). Only false before the
 * panel's first render this show (egl_ready still false), which is
 * unreachable in practice: the panel grabs keyboard/pointer input only once
 * visible, and becoming visible always renders at least once first. */
static bool np_make_current(dc_notepad *np)
{
    return np->egl_ready && dc_egl_make_current(np->egl, &np->egl_window);
}

/* Write the live editor buffer to tab `idx` and mark it clean. Uses only
 * dc_text_edit's "pure buffer ops" (get_text/mark_clean) plus plain file IO
 * -- no EGL context required, so this is safe to call from anywhere,
 * including tick()/destroy() with no surface ever created. */
static void np_flush_tab(dc_notepad *np, int idx)
{
    if (!np->storage)
        return;
    int n = dc_notepad_storage_count(np->storage);
    if (idx < 0 || idx >= n)
        return;
    size_t len = 0;
    const char *text = dc_text_edit_get_text(&np->editor, &len);
    dc_notepad_storage_write(np->storage, idx, text);
    /* write() only updates the tab's in-memory lastModified stamp; persist
     * it (and the tab list) now. */
    dc_notepad_storage_save_meta(np->storage);
    dc_text_edit_mark_clean(&np->editor);
}

void dc_notepad_flush(dc_notepad *np)
{
    if (!np || !np->storage)
        return;
    np_flush_tab(np, dc_notepad_storage_current(np->storage));
}

/* Load the current tab's on-disk content into the editor. set_text() is a
 * pure buffer op (no EGL needed) and also resets cursor/scroll/dirty, which
 * is exactly what we want both on first show() and after switching tabs. */
static void np_load_current(dc_notepad *np)
{
    if (!np->storage) {
        dc_text_edit_set_text(&np->editor, "", 0);
        return;
    }
    int cur = dc_notepad_storage_current(np->storage);
    char *text = dc_notepad_storage_read(np->storage, cur);
    dc_text_edit_set_text(&np->editor, text, text ? strlen(text) : 0);
    free(text);
}

static void np_switch_tab(dc_notepad *np, int new_index)
{
    if (!np->storage)
        return;
    int cur = dc_notepad_storage_current(np->storage);
    int n = dc_notepad_storage_count(np->storage);
    if (new_index < 0 || new_index >= n || new_index == cur)
        return;
    if (dc_text_edit_is_dirty(&np->editor))
        np_flush_tab(np, cur);
    dc_notepad_storage_set_current(np->storage, new_index);
    dc_notepad_storage_save_meta(np->storage); /* set_current() doesn't persist by itself */
    np_load_current(np);
    dc_text_edit_take_focus(&np->editor);
    np_render(np);
}

static void np_new_tab(dc_notepad *np)
{
    if (!np->storage)
        return;
    if (dc_text_edit_is_dirty(&np->editor))
        np_flush_tab(np, dc_notepad_storage_current(np->storage));
    int idx = dc_notepad_storage_create_tab(np->storage); /* appends + persists */
    dc_notepad_storage_set_current(np->storage, idx);
    dc_notepad_storage_save_meta(np->storage);
    np_load_current(np);
    dc_text_edit_take_focus(&np->editor);
    np_render(np);
}

/* Close tab `i` (any tab, not necessarily the current one -- clicking a
 * background tab's close dot reaches this too). If it's the current tab,
 * flush it first so no pending edit is lost before its file (and slot) go
 * away; a background tab's on-disk content is already the last-flushed
 * version (it's only ever live-edited while it's the current tab), so no
 * flush is needed for that case. */
static void np_close_tab_index(dc_notepad *np, int i)
{
    if (!np->storage)
        return;
    int n = dc_notepad_storage_count(np->storage);
    if (i < 0 || i >= n)
        return;
    int cur = dc_notepad_storage_current(np->storage);
    if (i == cur && dc_text_edit_is_dirty(&np->editor))
        np_flush_tab(np, cur);
    /* Re-indexes remaining tabs, clamps currentTabIndex, recreates a default
     * tab if this was the last one, and persists -- all handled inside. */
    dc_notepad_storage_delete_tab(np->storage, i);
    np_load_current(np);
    dc_text_edit_take_focus(&np->editor);
    np_render(np);
}

static void np_close_current_tab(dc_notepad *np)
{
    if (!np->storage)
        return;
    np_close_tab_index(np, dc_notepad_storage_current(np->storage));
}

static void np_cycle_tab(dc_notepad *np, int dir)
{
    if (!np->storage)
        return;
    int n = dc_notepad_storage_count(np->storage);
    if (n <= 1)
        return;
    int cur = dc_notepad_storage_current(np->storage);
    int next = ((cur + dir) % n + n) % n;
    np_switch_tab(np, next);
}

/* Hover bg (formula from bar.c's draw_hover_overlay(), shared via hover.h):
 * painted last, on top of whatever's already drawn at that hit rect. Same
 * convention as clip_picker.c's draw_cp_hover(). */
static void draw_np_hover(dc_notepad *np)
{
    if (np->hover_id == NP_HOVER_NONE)
        return;

    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, radius = 6.0f;

    if (np->hover_id == NP_HOVER_CLOSE) {
        x0 = np->close_x0;
        y0 = np->close_y0;
        x1 = np->close_x1;
        y1 = np->close_y1;
        radius = (x1 - x0) / 2.0f;
    } else if (np->hover_id == NP_HOVER_NEW_TAB) {
        x0 = np->new_tab_x0;
        y0 = np->new_tab_y0;
        x1 = np->new_tab_x1;
        y1 = np->new_tab_y1;
        radius = 10.0f;
    } else if (np->hover_id >= NP_HOVER_TAB_BASE) {
        int rel = np->hover_id - NP_HOVER_TAB_BASE;
        int i = rel / 2, kind = rel % 2;
        if (i < 0 || i >= np->tab_hit_count)
            return;
        const np_tab_hit *hit = &np->tab_hits[i];
        if (kind == 0) {
            x0 = hit->x0;
            y0 = hit->y0;
            x1 = hit->x1;
            y1 = hit->y1;
            radius = 10.0f;
        } else {
            x0 = hit->close_x0;
            y0 = hit->close_y0;
            x1 = hit->close_x1;
            y1 = hit->close_y1;
            radius = (x1 - x0) / 2.0f;
        }
    } else {
        return;
    }
    if (x1 <= x0 || y1 <= y0)
        return;

    NVGcontext *vg = np->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    dc_color hc =
        dc_hover_bg_color(t->surface_container_high, t->primary, cfg->bar_widget_transparency);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x0, y0, x1 - x0, y1 - y0, radius);
    nvgFillColor(vg, nvgRGBA(hc.r, hc.g, hc.b, hc.a));
    nvgFill(vg);
}

static void np_render(dc_notepad *np)
{
    if (!np->configured || np->phys_width <= 0)
        return;
    if (!np->egl_ready) {
        if (!dc_egl_window_init(&np->egl_window, np->egl, np->surface, np->phys_width,
                                np->phys_height))
            return;
        np->egl_ready = true;
    } else {
        dc_egl_window_resize(&np->egl_window, np->phys_width, np->phys_height);
    }
    if (!dc_egl_make_current(np->egl, &np->egl_window))
        return;
    if (!dc_render_ensure(np->render))
        return;
    if (np->viewport)
        wp_viewport_set_destination(np->viewport, np->logical_width, np->logical_height);

    NVGcontext *vg = np->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = np->logical_width, h = np->logical_height;
    const bool bottom_bar = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    int pad_near, pad_side, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, &pad_side, &pad_far);
    const float pad_top = bottom_bar ? (float)pad_far : (float)pad_near;
    const float pad_bottom = bottom_bar ? (float)pad_near : (float)pad_far;
    const float pad_side_f = (float)pad_side;

    glViewport(0, 0, np->phys_width, np->phys_height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)np->scale120 / DC_SCALE_BASE);

    float pr = dc_anim_progress(&np->anim);
    if (np->closing)
        pr = 1.0f - (pr > 1.0f ? 1.0f : pr);
    float alpha = pr > 1.0f ? 1.0f : pr;
    float scale = 0.94f + 0.06f * pr;
    float ox = pad_side_f + (w - 2.0f * pad_side_f) * np->anim_ox;
    float oy = pad_top + (h - pad_top - pad_bottom) * np->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Card chrome: shadow + fill + outline, floating or stitched into the
     * bar depending on connected_frame -- see ui/connected.h. Byte-identical
     * to the old inline floating-chrome block when the toggle is off. */
    dc_connected_card_chrome(vg, np->render, w, h, bottom_bar);

    np_layout l = np_get_layout(w, h);

    /* --- Header: edit icon + "Notepad" title; close X ------------------- */
    const float header_cy = l.header_y + l.header_h / 2.0f;
    dc_render_icon(np->render, DC_ICON_EDIT, l.ix, header_cy, 18.0f, t->primary,
                  NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, np->render->font_ui);
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, l.ix + 26.0f, header_cy, "Notepad", NULL);

    const float hbtn_r = 12.0f;
    float hbtn_cx = l.ix + l.iw - hbtn_r;
    dc_render_icon(np->render, DC_ICON_CLOSE, hbtn_cx, header_cy, 16.0f, t->surface_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    np->close_x0 = hbtn_cx - hbtn_r - 6.0f;
    np->close_y0 = header_cy - hbtn_r - 6.0f;
    np->close_x1 = hbtn_cx + hbtn_r + 6.0f;
    np->close_y1 = header_cy + hbtn_r + 6.0f;

    /* --- Tab strip: per-tab chip + trailing "+" chip --------------------- */
    np->tab_hit_count = 0;
    int n = np->storage ? dc_notepad_storage_count(np->storage) : 0;
    int cur = np->storage ? dc_notepad_storage_current(np->storage) : -1;

    float avail = l.iw - DC_NP_PLUS_CHIP_W - DC_NP_TAB_CHIP_GAP;
    float chip_w = DC_NP_TAB_CHIP_MAX_W;
    if (n > 0) {
        float fit = (avail - (float)(n - 1) * DC_NP_TAB_CHIP_GAP) / (float)n;
        if (fit < chip_w)
            chip_w = fit;
        if (chip_w < DC_NP_TAB_CHIP_MIN_W)
            chip_w = DC_NP_TAB_CHIP_MIN_W;
        /* v1: no horizontal scroll/reorder for the tab strip -- with more
         * tabs than fit even at the floor width, the trailing chips (and
         * the "+" chip) simply run past the panel's right edge. Rare in
         * practice; a later polish pass (docs/22 NT8) can add scrolling. */
    }

    float cx = l.ix;
    for (int i = 0; i < n && np->tab_hit_count < DC_NP_MAX_TAB_HITS; i++) {
        bool active = (i == cur);
        float x0 = cx, y0 = l.tabstrip_y, x1 = cx + chip_w, y1 = l.tabstrip_y + l.tabstrip_h;

        np_tab_hit *hit = &np->tab_hits[np->tab_hit_count++];
        hit->tab_index = i;
        hit->x0 = x0;
        hit->y0 = y0;
        hit->x1 = x1;
        hit->y1 = y1;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x0, y0, chip_w, l.tabstrip_h, 10.0f);
        nvgFillColor(vg, tc(active ? t->surface_container_highest : t->surface_container_high));
        nvgFill(vg);
        if (active) {
            nvgStrokeColor(vg, tc_alpha(t->primary, 140));
            nvgStrokeWidth(vg, 1.2f);
            nvgStroke(vg);
        }

        const float dot_r = 5.0f;
        float dot_cx = x1 - 12.0f;
        float dot_cy = y0 + l.tabstrip_h / 2.0f;
        hit->close_x0 = dot_cx - dot_r - 4.0f;
        hit->close_y0 = dot_cy - dot_r - 4.0f;
        hit->close_x1 = dot_cx + dot_r + 4.0f;
        hit->close_y1 = dot_cy + dot_r + 4.0f;
        dc_render_icon(np->render, DC_ICON_CLOSE, dot_cx, dot_cy, 11.0f, t->surface_variant_text,
                      NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        const char *title = np->storage ? dc_notepad_storage_title(np->storage, i) : "";
        float text_x0 = x0 + 10.0f;
        float text_w = hit->close_x0 - text_x0;
        if (text_w > 8.0f) {
            char ell[64];
            dc_shape_ellipsize(np->render, title, text_w, ell, sizeof(ell));
            nvgSave(vg);
            nvgScissor(vg, x0, y0, chip_w, l.tabstrip_h);
            nvgFontFaceId(vg, np->render->font_ui);
            nvgFontSize(vg, 12.0f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, tc(active ? t->surface_text : t->surface_variant_text));
            dc_shape_draw_text(np->render, text_x0, y0 + l.tabstrip_h / 2.0f, ell, NULL);
            nvgRestore(vg);
        }

        cx += chip_w + DC_NP_TAB_CHIP_GAP;
    }

    np->new_tab_x0 = cx;
    np->new_tab_y0 = l.tabstrip_y;
    np->new_tab_x1 = cx + DC_NP_PLUS_CHIP_W;
    np->new_tab_y1 = l.tabstrip_y + l.tabstrip_h;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, np->new_tab_x0, np->new_tab_y0, DC_NP_PLUS_CHIP_W, l.tabstrip_h, 10.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    dc_render_icon(np->render, DC_ICON_ADD, np->new_tab_x0 + DC_NP_PLUS_CHIP_W / 2.0f,
                  l.tabstrip_y + l.tabstrip_h / 2.0f, 16.0f, t->surface_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    /* --- Editor -------------------------------------------------------- */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l.editor_x, l.editor_y, l.editor_w, l.editor_h, 10.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    np->editor_x = l.editor_x + DC_NP_EDITOR_INNER_PAD;
    np->editor_y = l.editor_y + DC_NP_EDITOR_INNER_PAD;
    np->editor_w = l.editor_w - 2.0f * DC_NP_EDITOR_INNER_PAD;
    np->editor_h = l.editor_h - 2.0f * DC_NP_EDITOR_INNER_PAD;
    if (np->editor_w > 0.0f && np->editor_h > 0.0f)
        dc_text_edit_draw(&np->editor, np->render, np->editor_x, np->editor_y, np->editor_w,
                          np->editor_h, "Start typing\xe2\x80\xa6");

    /* --- Footer: "Saved · N chars" / "Unsaved changes" ------------------ */
    size_t clen = 0;
    dc_text_edit_get_text(&np->editor, &clen);
    bool dirty = dc_text_edit_is_dirty(&np->editor);
    char status[64];
    if (dirty)
        snprintf(status, sizeof(status), "Unsaved changes");
    else
        snprintf(status, sizeof(status), "Saved \xc2\xb7 %zu chars", clen);
    nvgFontFaceId(vg, np->render->font_ui);
    nvgFontSize(vg, 11.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 130));
    nvgText(vg, l.ix, l.footer_y + l.footer_h / 2.0f, status, NULL);

    draw_np_hover(np);

    nvgEndFrame(vg);
    if ((dc_anim_active(&np->anim) || np->closing) && !np->frame_cb) {
        np->frame_cb = wl_surface_frame(np->surface);
        wl_callback_add_listener(np->frame_cb, &np_frame_listener, np);
    }
    dc_egl_swap(np->egl, &np->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_notepad *np = data;
    DC_UNUSED(fs);
    np->scale120 = (int)scale;
    recompute_physical(np);
    np_render(np);
}
static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                           uint32_t serial, uint32_t width, uint32_t height)
{
    dc_notepad *np = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    np->logical_width = width > 0 ? (int)width : np_surface_width();
    np->logical_height = height > 0 ? (int)height : DC_NP_HEIGHT;
    np->configured = true;
    recompute_physical(np);
    np_render(np);
}
static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_notepad *np = data;
    DC_UNUSED(surface);
    np->configured = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_notepad *dc_notepad_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_notepad *np = calloc(1, sizeof(*np));
    np->wl = wl;
    np->egl = egl;
    np->render = render;
    np->logical_width = np_surface_width();
    np->logical_height = DC_NP_HEIGHT;
    np->scale120 = DC_SCALE_BASE;
    dc_text_edit_init(&np->editor);
    return np;
}

static void np_show(dc_notepad *np, dc_output *output)
{
    np->output = output;
    np->configured = false;
    np->egl_ready = false;
    np->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;

    if (!np->storage)
        np->storage = dc_notepad_storage_load();
    np_load_current(np);
    dc_text_edit_take_focus(&np->editor);

    dc_anim_start(&np->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    np->surface = wl_compositor_create_surface(np->wl->compositor);
    if (np->wl->fractional_scale_mgr) {
        np->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            np->wl->fractional_scale_mgr, np->surface);
        wp_fractional_scale_v1_add_listener(np->fractional_scale, &fractional_scale_listener, np);
    }
    if (np->wl->viewporter)
        np->viewport = wp_viewporter_get_viewport(np->wl->viewporter, np->surface);

    np->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        np->wl->layer_shell, np->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:notepad");

    /* Bar-adjacent, right-aligned (matches clip_picker.c/notifcenter.c). */
    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_END, DC_NP_SIDE_MARGIN);
    np->anim_ox = pa.origin_x;
    np->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(np->layer_surface, pa.anchor);
    np->logical_width = np_surface_width();
    zwlr_layer_surface_v1_set_size(np->layer_surface, (uint32_t)np->logical_width, DC_NP_HEIGHT);
    zwlr_layer_surface_v1_set_margin(np->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        np->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(np->layer_surface, &layer_surface_listener, np);
    wl_surface_commit(np->surface);
    np->visible = true;
    np->closing = false;
    dc_debug("notepad shown");
}

static void np_teardown(dc_notepad *np)
{
    if (np->frame_cb) {
        wl_callback_destroy(np->frame_cb);
        np->frame_cb = NULL;
    }
    if (np->egl_ready)
        dc_egl_window_finish(&np->egl_window, np->egl);
    if (np->viewport)
        wp_viewport_destroy(np->viewport);
    if (np->fractional_scale)
        wp_fractional_scale_v1_destroy(np->fractional_scale);
    if (np->layer_surface)
        zwlr_layer_surface_v1_destroy(np->layer_surface);
    if (np->surface)
        wl_surface_destroy(np->surface);
    np->egl_ready = false;
    np->configured = false;
    np->viewport = NULL;
    np->fractional_scale = NULL;
    np->layer_surface = NULL;
    np->surface = NULL;
    np->visible = false;
    np->closing = false;
    np->hover_id = NP_HOVER_NONE;
    dc_debug("notepad hidden");
}

static void np_begin_close(dc_notepad *np)
{
    if (!np->visible || np->closing)
        return;
    dc_notepad_flush(np);
    dc_text_edit_drop_focus(&np->editor);
    dc_anim_start(&np->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    np->closing = true;
    if (!dc_anim_active(&np->anim)) {
        np_teardown(np);
        return;
    }
    np_render(np);
}

void dc_notepad_toggle(dc_notepad *np, dc_output *output)
{
    if (np->visible)
        np_begin_close(np);
    else
        np_show(np, output);
}

void dc_notepad_hide(dc_notepad *np)
{
    np_begin_close(np);
}

bool dc_notepad_visible(dc_notepad *np)
{
    return np->visible;
}

struct wl_surface *dc_notepad_surface(dc_notepad *np)
{
    return np->surface;
}

void dc_notepad_handle_key(dc_notepad *np, uint32_t keysym, const char *utf8)
{
    if (!np->visible || np->closing)
        return;

    bool ctrl = dc_wayland_ctrl_down(np->wl);
    bool shift = dc_wayland_shift_down(np->wl);

    if (keysym == XKB_KEY_Escape) {
        np_begin_close(np);
        return;
    }

    if (ctrl) {
        switch (keysym) {
        case XKB_KEY_s:
        case XKB_KEY_S:
            dc_notepad_flush(np);
            np_render(np);
            return;
        case XKB_KEY_n:
        case XKB_KEY_N:
            np_new_tab(np);
            return;
        case XKB_KEY_w:
        case XKB_KEY_W:
            np_close_current_tab(np);
            return;
        case XKB_KEY_Tab:
            np_cycle_tab(np, shift ? -1 : 1);
            return;
        case XKB_KEY_ISO_Left_Tab:
            /* Some compositors report Shift+Tab this way instead of
             * plain Tab with the shift modifier bit set. */
            np_cycle_tab(np, -1);
            return;
        default:
            break;
        }
    }

    if (!np_make_current(np))
        return;
    if (dc_text_edit_key(&np->editor, np->render, keysym, utf8, ctrl, shift))
        np_render(np);
}

static inline bool in_rect(double x, double y, float x0, float y0, float x1, float y1)
{
    return x1 > x0 && x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

/* Which interactive element (if any) sits under (x, y) -- shares the exact
 * hit boundaries dc_notepad_handle_click() dispatches against (same
 * discipline as clip_picker.c's cp_hittest()): close first, then each tab
 * chip's close-dot before its own body, then the trailing "+" chip. */
static int np_hittest(dc_notepad *np, double x, double y)
{
    if (in_rect(x, y, np->close_x0, np->close_y0, np->close_x1, np->close_y1))
        return NP_HOVER_CLOSE;
    if (in_rect(x, y, np->new_tab_x0, np->new_tab_y0, np->new_tab_x1, np->new_tab_y1))
        return NP_HOVER_NEW_TAB;

    for (int i = 0; i < np->tab_hit_count; i++) {
        np_tab_hit *hit = &np->tab_hits[i];
        if (in_rect(x, y, hit->close_x0, hit->close_y0, hit->close_x1, hit->close_y1))
            return NP_HOVER_TAB_BASE + i * 2 + 1;
        if (in_rect(x, y, hit->x0, hit->y0, hit->x1, hit->y1))
            return NP_HOVER_TAB_BASE + i * 2 + 0;
    }
    return NP_HOVER_NONE;
}

void dc_notepad_handle_click(dc_notepad *np, double x, double y)
{
    if (!np->visible || np->closing)
        return;

    if (in_rect(x, y, np->close_x0, np->close_y0, np->close_x1, np->close_y1)) {
        np_begin_close(np);
        return;
    }
    if (in_rect(x, y, np->new_tab_x0, np->new_tab_y0, np->new_tab_x1, np->new_tab_y1)) {
        np_new_tab(np);
        return;
    }

    for (int i = 0; i < np->tab_hit_count; i++) {
        np_tab_hit *hit = &np->tab_hits[i];
        if (in_rect(x, y, hit->close_x0, hit->close_y0, hit->close_x1, hit->close_y1)) {
            np_close_tab_index(np, hit->tab_index);
            return;
        }
        if (in_rect(x, y, hit->x0, hit->y0, hit->x1, hit->y1)) {
            np_switch_tab(np, hit->tab_index);
            return;
        }
    }

    if (x >= (double)np->editor_x && x <= (double)(np->editor_x + np->editor_w) &&
        y >= (double)np->editor_y && y <= (double)(np->editor_y + np->editor_h)) {
        if (!np_make_current(np))
            return;
        dc_text_edit_take_focus(&np->editor);
        dc_text_edit_click(&np->editor, np->render, (float)(x - np->editor_x),
                           (float)(y - np->editor_y));
        np_render(np);
    }
}

/* Pointer motion over the panel: hover tracking, re-rendering only when the
 * hovered id changes (same guard pattern as clip_picker.c's
 * dc_clip_picker_handle_motion()). The editor itself has no hover state of
 * its own (no IBEAM cursor shape available, see wl.h's dc_cursor_shape). */
void dc_notepad_handle_motion(dc_notepad *np, double x, double y)
{
    if (!np->visible || np->closing)
        return;

    int id = np_hittest(np, x, y);
    if (id == np->hover_id)
        return;

    np->hover_id = id;
    dc_wayland_set_cursor(np->wl, id != NP_HOVER_NONE ? DC_CURSOR_POINTER : DC_CURSOR_DEFAULT);
    np_render(np);
}

void dc_notepad_handle_leave(dc_notepad *np)
{
    if (np->hover_id == NP_HOVER_NONE)
        return;
    np->hover_id = NP_HOVER_NONE;
    dc_wayland_set_cursor(np->wl, DC_CURSOR_DEFAULT);
    np_render(np);
}

void dc_notepad_handle_scroll(dc_notepad *np, int steps_v)
{
    if (!np->visible || np->closing || steps_v == 0)
        return;
    /* The editor is the panel's one scrollable region (v1 tab strip has no
     * overflow scroll), so wheel input always routes to it -- same
     * no-coordinates convention as clip_picker.c's
     * dc_clip_picker_handle_scroll(). dc_text_edit_scroll() is a pure op (no
     * EGL needed). */
    dc_text_edit_scroll(&np->editor, (float)steps_v * DC_NP_SCROLL_STEP);
    np_render(np);
}

void dc_notepad_tick(dc_notepad *np)
{
    if (!np || !np->storage)
        return;
    if (!dc_text_edit_is_dirty(&np->editor))
        return;
    uint64_t now = (uint64_t)dc_anim_now_ms();
    uint64_t last = dc_text_edit_last_edit_ms(&np->editor);
    if (now < last || now - last < DC_NOTEPAD_AUTOSAVE_MS)
        return;
    np_flush_tab(np, dc_notepad_storage_current(np->storage));
    if (np->visible && !np->closing)
        np_render(np); /* footer "Unsaved changes" -> "Saved..." */
}

void dc_notepad_destroy(dc_notepad *np)
{
    if (!np)
        return;
    dc_notepad_flush(np); /* pure ops only -- safe even with no surface/EGL ever created */
    if (np->visible)
        np_teardown(np);
    dc_text_edit_free(&np->editor);
    if (np->storage)
        dc_notepad_storage_destroy(np->storage);
    free(np);
}
