#include "ui/tray_menu.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/nvg.h"
#include "services/dbus.h"
#include "services/tray.h"
#include "theme/theme.h"
#include "ui/connected.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <systemd/sd-bus.h>

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_TM_WIDTH 240
#define DC_TM_ROW_H 30.0f
#define DC_TM_SEP_H 9.0f
#define DC_TM_PAD 6.0f    /* room for the drop shadow, same convention as battery_popout.c */
#define DC_TM_MARGIN 8.0f /* content inset from the card edge */
#define DC_TM_INDENT 14.0f /* per submenu-depth-level indent (flattened, not nested popups) */
#define DC_TM_MAX_ITEMS 48
#define DC_TM_MAX_DEPTH 4
#define DC_SCALE_BASE 120
#define DC_TM_SIDE_MARGIN 12 /* approximation of "near the tray cluster" -- see popout.h */

#define DC_DBUSMENU_IFACE "com.canonical.dbusmenu"

/* Logical surface width. DC_TM_WIDTH already bakes in the floating chrome's
 * flat 6px pad on every side; connected_frame widens the lateral (side) pad
 * to 12 for the connector fillets (dc_popout_chrome_pads()), so the surface
 * needs 2*(pad_side-6) more logical px to keep the card CONTENT rect --
 * inset by pad_side + DC_TM_MARGIN, see tm_row_rect()/tm_render() -- exactly
 * where it sits when floating (mirrors controlcenter.c's cc_surface_width()).
 * connected_frame off: pad_side==6, so this is just DC_TM_WIDTH, unchanged. */
static int tm_surface_width(void)
{
    int pad_side = 6;
    dc_popout_chrome_pads(dc_config_current, NULL, &pad_side, NULL);
    return DC_TM_WIDTH + 2 * (pad_side - 6);
}

typedef struct {
    int32_t id;
    char label[96];
    bool is_separator;
    bool enabled;
    int depth;
} dc_tm_item;

struct dc_tray_menu {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    struct dc_dbus *dbus;
    struct dc_tray *tray;
    dc_output *output;

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

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;
    float anim_ox, anim_oy;

    bool visible;
    bool configured;
    bool egl_ready;

    /* Which item's menu is open, so Event() targets the right bus name/path. */
    char service[DC_TRAY_STR];
    char menu_path[DC_TRAY_STR];

    dc_tm_item items[DC_TM_MAX_ITEMS];
    int count;
};

static void tm_render(dc_tray_menu *m);
static void tm_teardown(dc_tray_menu *m);

static void tm_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_tray_menu *m = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    m->frame_cb = NULL;
    if (!m->visible)
        return;
    if (dc_anim_active(&m->anim))
        tm_render(m);
    else if (m->closing)
        tm_teardown(m);
}

static const struct wl_callback_listener tm_frame_listener = {.done = tm_frame_done};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

/* --- dbusmenu (com.canonical.dbusmenu) ------------------------------------ */

/* Parse one `(ia{sv}av)` item, already positioned inside its 'r' container.
 * Appends to m->items (bounded) and recurses into children with depth+1,
 * indenting rather than opening nested popups (task note: "flatten or
 * indent" -- indent keeps ordering/click-routing simple with one flat rect
 * list). Depths beyond DC_TM_MAX_DEPTH stop descending (still consumes the
 * message correctly, just doesn't render that deep). */
static void tm_parse_item(dc_tray_menu *m, sd_bus_message *msg, int depth)
{
    int32_t id = 0;
    sd_bus_message_read(msg, "i", &id);

    char label[96] = {0};
    bool enabled = true, visible = true, is_sep = false;

    if (sd_bus_message_enter_container(msg, 'a', "{sv}") > 0) {
        while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
            const char *key = NULL;
            sd_bus_message_read_basic(msg, 's', &key);
            if (key && strcmp(key, "label") == 0) {
                if (sd_bus_message_enter_container(msg, 'v', "s") > 0) {
                    const char *s = NULL;
                    sd_bus_message_read_basic(msg, 's', &s);
                    if (s)
                        snprintf(label, sizeof(label), "%s", s);
                    sd_bus_message_exit_container(msg);
                }
            } else if (key && strcmp(key, "enabled") == 0) {
                if (sd_bus_message_enter_container(msg, 'v', "b") > 0) {
                    int b = 1;
                    sd_bus_message_read_basic(msg, 'b', &b);
                    enabled = b != 0;
                    sd_bus_message_exit_container(msg);
                }
            } else if (key && strcmp(key, "visible") == 0) {
                if (sd_bus_message_enter_container(msg, 'v', "b") > 0) {
                    int b = 1;
                    sd_bus_message_read_basic(msg, 'b', &b);
                    visible = b != 0;
                    sd_bus_message_exit_container(msg);
                }
            } else if (key && strcmp(key, "type") == 0) {
                if (sd_bus_message_enter_container(msg, 'v', "s") > 0) {
                    const char *s = NULL;
                    sd_bus_message_read_basic(msg, 's', &s);
                    if (s && strcmp(s, "separator") == 0)
                        is_sep = true;
                    sd_bus_message_exit_container(msg);
                }
            } else {
                sd_bus_message_skip(msg, "v");
            }
            sd_bus_message_exit_container(msg); /* e */
        }
        sd_bus_message_exit_container(msg); /* a{sv} */
    }

    if (visible && depth <= DC_TM_MAX_DEPTH && m->count < DC_TM_MAX_ITEMS) {
        dc_tm_item *e = &m->items[m->count++];
        e->id = id;
        snprintf(e->label, sizeof(e->label), "%s", label);
        e->is_separator = is_sep;
        e->enabled = enabled;
        e->depth = depth;
    }

    int r = sd_bus_message_enter_container(msg, 'a', "v");
    if (r > 0) {
        while (sd_bus_message_enter_container(msg, 'v', "(ia{sv}av)") > 0) {
            if (sd_bus_message_enter_container(msg, 'r', "ia{sv}av") > 0) {
                tm_parse_item(m, msg, depth + 1);
                sd_bus_message_exit_container(msg); /* r */
            }
            sd_bus_message_exit_container(msg); /* v */
        }
        sd_bus_message_exit_container(msg); /* a */
    }
}

/* AboutToShow (best-effort -- some implementations lazily populate the menu
 * on this call) + GetLayout, flattened into m->items. Returns the item count
 * (0 if the item has no menu, the call failed, or the menu was empty). */
static int tm_fetch_layout(dc_tray_menu *m, const char *service, const char *path)
{
    m->count = 0;
    if (!m->dbus || !m->dbus->user || !service[0] || !path[0])
        return 0;

    {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        sd_bus_call_method(m->dbus->user, service, path, DC_DBUSMENU_IFACE, "AboutToShow", &err,
                          &reply, "i", (int32_t)0);
        sd_bus_error_free(&err);
        if (reply)
            sd_bus_message_unref(reply);
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(m->dbus->user, service, path, DC_DBUSMENU_IFACE, "GetLayout", &err,
                               &reply, "iias", (int32_t)0, (int32_t)-1, 0);
    if (r < 0) {
        dc_debug("tray_menu: GetLayout failed on %s%s: %s", service, path,
                err.message ? err.message : "?");
        sd_bus_error_free(&err);
        return 0;
    }

    uint32_t revision = 0;
    sd_bus_message_read(reply, "u", &revision);
    DC_UNUSED(revision);
    if (sd_bus_message_enter_container(reply, 'r', "ia{sv}av") > 0) {
        /* The root item (id 0, usually unlabeled) isn't itself a row --
         * only descend into its children so the popup starts at depth 0. */
        int32_t root_id = 0;
        sd_bus_message_read(reply, "i", &root_id);
        DC_UNUSED(root_id);
        if (sd_bus_message_enter_container(reply, 'a', "{sv}") > 0) {
            while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                sd_bus_message_skip(reply, "sv"); /* key (s) + value (v) -- not just the value */
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
        }
        if (sd_bus_message_enter_container(reply, 'a', "v") > 0) {
            while (sd_bus_message_enter_container(reply, 'v', "(ia{sv}av)") > 0) {
                if (sd_bus_message_enter_container(reply, 'r', "ia{sv}av") > 0) {
                    tm_parse_item(m, reply, 0);
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);
    return m->count;
}

/* Fire-and-forget dbusmenu Event("clicked"). */
static void tm_fire_event(dc_tray_menu *m, int32_t id)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(m->dbus->user, m->service, m->menu_path, DC_DBUSMENU_IFACE, "Event",
                               &err, &reply, "isvu", id, "clicked", "s", "", (uint32_t)0);
    if (r < 0) {
        dc_debug("tray_menu: Event failed: %s", err.message ? err.message : "?");
        sd_bus_error_free(&err);
    }
    if (reply)
        sd_bus_message_unref(reply);
}

/* --- layout: row rects, shared by tm_render (draw) and handle_click ------- */

static float tm_content_height(dc_tray_menu *m)
{
    float h = 2.0f * DC_TM_MARGIN;
    for (int i = 0; i < m->count; i++)
        h += m->items[i].is_separator ? DC_TM_SEP_H : DC_TM_ROW_H;
    return h;
}

/* Bar-facing (near) edge pad (docs/27-CONNECTED-FRAME-PLAN.md T5): 0 in
 * connected mode, DC_TM_PAD (6) floating -- mirrors dc_popout_chrome_pads(),
 * self-contained here the same way controlcenter.c's cc_get_layout() reads
 * dc_config_current directly rather than threading it through callers. */
static float tm_pad_top(void)
{
    int pad_near, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, NULL, &pad_far);
    const bool bottom_bar = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    return bottom_bar ? (float)pad_far : (float)pad_near;
}

static void tm_row_rect(dc_tray_menu *m, int index, float *out_y0, float *out_y1)
{
    float y = tm_pad_top() + DC_TM_MARGIN;
    for (int i = 0; i < index; i++)
        y += m->items[i].is_separator ? DC_TM_SEP_H : DC_TM_ROW_H;
    *out_y0 = y;
    *out_y1 = y + (m->items[index].is_separator ? DC_TM_SEP_H : DC_TM_ROW_H);
}

static void recompute_physical(dc_tray_menu *m)
{
    m->phys_width = (m->logical_width * m->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    m->phys_height = (m->logical_height * m->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void tm_render(dc_tray_menu *m)
{
    if (!m->configured || m->phys_width <= 0)
        return;

    if (!m->egl_ready) {
        if (!dc_egl_window_init(&m->egl_window, m->egl, m->surface, m->phys_width, m->phys_height))
            return;
        m->egl_ready = true;
    } else {
        dc_egl_window_resize(&m->egl_window, m->phys_width, m->phys_height);
    }

    if (!dc_egl_make_current(m->egl, &m->egl_window))
        return;
    if (!dc_render_ensure(m->render))
        return;

    if (m->viewport)
        wp_viewport_set_destination(m->viewport, m->logical_width, m->logical_height);

    NVGcontext *vg = m->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = (float)m->logical_width;
    const float h = (float)m->logical_height;
    const bool bottom_bar = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    int pad_near, pad_side, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, &pad_side, &pad_far);
    const float pad_top = bottom_bar ? (float)pad_far : (float)pad_near;
    const float pad_bottom = bottom_bar ? (float)pad_near : (float)pad_far;
    const float pad_side_f = (float)pad_side;

    glViewport(0, 0, m->phys_width, m->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)m->scale120 / DC_SCALE_BASE);

    float p = dc_anim_progress(&m->anim);
    if (m->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.94f + 0.06f * p;
    float ox = pad_side_f + (w - 2.0f * pad_side_f) * m->anim_ox;
    float oy = pad_top + (h - pad_top - pad_bottom) * m->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Card chrome: shadow + fill + outline, floating or stitched into the
     * bar depending on connected_frame -- see ui/connected.h. Byte-identical
     * to the old inline floating-chrome block when the toggle is off. */
    dc_connected_card_chrome(vg, m->render, w, h, bottom_bar);

    for (int i = 0; i < m->count; i++) {
        const dc_tm_item *it = &m->items[i];
        float y0, y1;
        tm_row_rect(m, i, &y0, &y1);

        if (it->is_separator) {
            float ly = (y0 + y1) / 2.0f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, pad_side_f + DC_TM_MARGIN, ly);
            nvgLineTo(vg, w - pad_side_f - DC_TM_MARGIN, ly);
            nvgStrokeColor(vg, tc_alpha(t->outline, 60));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
            continue;
        }

        float text_x = pad_side_f + DC_TM_MARGIN + (float)it->depth * DC_TM_INDENT;
        float cy = (y0 + y1) / 2.0f;

        nvgSave(vg);
        nvgScissor(vg, pad_side_f + DC_TM_MARGIN, y0, w - 2.0f * (pad_side_f + DC_TM_MARGIN),
                  y1 - y0);
        nvgFontFaceId(vg, m->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, it->enabled ? tc(t->surface_text) : tc_alpha(t->surface_text, 100));
        nvgText(vg, text_x, cy, it->label[0] ? it->label : "-", NULL);
        nvgRestore(vg);
    }

    if (m->count == 0) {
        nvgFontFaceId(vg, m->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 140));
        nvgText(vg, pad_side_f + DC_TM_MARGIN, pad_top + DC_TM_MARGIN + DC_TM_ROW_H / 2.0f,
               "(empty)", NULL);
    }

    nvgEndFrame(vg);

    if ((dc_anim_active(&m->anim) || m->closing) && !m->frame_cb) {
        m->frame_cb = wl_surface_frame(m->surface);
        wl_callback_add_listener(m->frame_cb, &tm_frame_listener, m);
    }
    dc_egl_swap(m->egl, &m->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_tray_menu *m = data;
    DC_UNUSED(fs);
    m->scale120 = (int)scale;
    recompute_physical(m);
    tm_render(m);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_tray_menu *m = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    m->logical_width = width > 0 ? (int)width : tm_surface_width();
    m->logical_height = height > 0 ? (int)height : m->logical_height;
    m->configured = true;
    recompute_physical(m);
    tm_render(m);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_tray_menu *m = data;
    DC_UNUSED(surface);
    m->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_tray_menu *dc_tray_menu_create(dc_wayland *wl, dc_egl *egl, dc_render *render, struct dc_dbus *dbus,
                                  struct dc_tray *tray)
{
    dc_tray_menu *m = calloc(1, sizeof(*m));
    m->wl = wl;
    m->egl = egl;
    m->render = render;
    m->dbus = dbus;
    m->tray = tray;
    m->logical_width = tm_surface_width();
    m->scale120 = DC_SCALE_BASE;
    return m;
}

static void tm_teardown(dc_tray_menu *m)
{
    if (m->frame_cb) {
        wl_callback_destroy(m->frame_cb);
        m->frame_cb = NULL;
    }
    if (m->egl_ready)
        dc_egl_window_finish(&m->egl_window, m->egl);
    if (m->viewport)
        wp_viewport_destroy(m->viewport);
    if (m->fractional_scale)
        wp_fractional_scale_v1_destroy(m->fractional_scale);
    if (m->layer_surface)
        zwlr_layer_surface_v1_destroy(m->layer_surface);
    if (m->surface)
        wl_surface_destroy(m->surface);
    m->egl_ready = false;
    m->configured = false;
    m->viewport = NULL;
    m->fractional_scale = NULL;
    m->layer_surface = NULL;
    m->surface = NULL;
    m->visible = false;
    m->closing = false;
    dc_debug("tray menu hidden");
}

static void tm_begin_close(dc_tray_menu *m)
{
    if (!m->visible || m->closing)
        return;
    dc_anim_start(&m->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    m->closing = true;
    if (!dc_anim_active(&m->anim)) {
        tm_teardown(m);
        return;
    }
    tm_render(m);
}

void dc_tray_menu_open(dc_tray_menu *m, dc_output *output, int tray_index, int x, int y)
{
    if (!m || !m->tray)
        return;

    const dc_tray_item *items[DC_TRAY_MAX];
    int n = dc_tray_items(m->tray, items, DC_TRAY_MAX);
    if (tray_index < 0 || tray_index >= n) {
        tm_begin_close(m);
        return;
    }
    const dc_tray_item *item = items[tray_index];

    if (!item->menu_path[0]) {
        /* No dbusmenu -- ContextMenu(x,y) fallback (docs/POLISH.md P4). */
        tm_begin_close(m);
        dc_tray_context_menu(m->tray, tray_index, x, y);
        return;
    }

    /* Replace any currently-open menu outright rather than toggling -- a
     * second right-click while one is open (same or a different item)
     * should just show the freshly-fetched layout for the new target. */
    if (m->visible)
        tm_teardown(m);

    snprintf(m->service, sizeof(m->service), "%s", item->service);
    snprintf(m->menu_path, sizeof(m->menu_path), "%s", item->menu_path);

    int count = tm_fetch_layout(m, m->service, m->menu_path);
    if (count == 0) {
        /* Empty/failed layout -- ContextMenu(x,y) fallback too. */
        dc_tray_context_menu(m->tray, tray_index, x, y);
        return;
    }

    m->output = output;
    m->configured = false;
    m->egl_ready = false;
    m->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    m->logical_width = tm_surface_width();
    m->logical_height = (int)tm_content_height(m);
    dc_anim_start(&m->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    m->surface = wl_compositor_create_surface(m->wl->compositor);
    if (m->wl->fractional_scale_mgr) {
        m->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            m->wl->fractional_scale_mgr, m->surface);
        wp_fractional_scale_v1_add_listener(m->fractional_scale, &fractional_scale_listener, m);
    }
    if (m->wl->viewporter)
        m->viewport = wp_viewporter_get_viewport(m->wl->viewporter, m->surface);

    m->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        m->wl->layer_shell, m->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:tray-menu");

    /* Bar-adjacent, right-aligned -- systemTray sits in the bar's right
     * cluster (docs/12-BAR-SPEC.md sec.0), same "near the cluster, not
     * pixel-exact under the clicked chip" approximation battery_popout.c and
     * controlcenter.c already make (dc_popout_bar_adjacent() only anchors to
     * a screen edge). */
    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_END, DC_TM_SIDE_MARGIN);
    m->anim_ox = pa.origin_x;
    m->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(m->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(m->layer_surface, m->logical_width, m->logical_height);
    zwlr_layer_surface_v1_set_margin(m->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_exclusive_zone(m->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(m->layer_surface, &layer_surface_listener, m);

    wl_surface_commit(m->surface);
    m->visible = true;
    m->closing = false;
    dc_debug("tray menu shown (%d item(s))", m->count);
}

bool dc_tray_menu_visible(dc_tray_menu *m)
{
    return m && m->visible;
}

void dc_tray_menu_hide(dc_tray_menu *m)
{
    if (m)
        tm_begin_close(m);
}

struct wl_surface *dc_tray_menu_surface(dc_tray_menu *m)
{
    return m ? m->surface : NULL;
}

void dc_tray_menu_handle_click(dc_tray_menu *m, double x, double y)
{
    if (!m || !m->visible || m->closing)
        return;

    for (int i = 0; i < m->count; i++) {
        const dc_tm_item *it = &m->items[i];
        if (it->is_separator)
            continue;
        float y0, y1;
        tm_row_rect(m, i, &y0, &y1);
        if (y < (double)y0 || y > (double)y1)
            continue;
        if (it->enabled)
            tm_fire_event(m, it->id);
        tm_begin_close(m);
        return;
    }
    DC_UNUSED(x);
}

void dc_tray_menu_destroy(dc_tray_menu *m)
{
    if (!m)
        return;
    if (m->visible)
        tm_teardown(m);
    free(m);
}
