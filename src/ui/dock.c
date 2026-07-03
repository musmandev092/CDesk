#include "ui/dock.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "niri/niri.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/apps.h"
#include "services/icons.h"
#include "theme/theme.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* docs/POLISH.md P5 / docs/11-UX-FLOW.md sec.5: pinned + running apps, DMS's
 * default dockIconSize=40. Reveal/hide slide timing lives in core/anim.h
 * (DC_DUR_DOCK_SLIDE = 225ms, matching "Dock slide | 225 ms OutCubic" in
 * docs/11 sec.7's duration table). */
#define DC_DOCK_MAX_ITEMS 24
#define DC_DOCK_ICON_CACHE_MAX 32
#define DC_DOCK_SCALE_BASE 120

/* Reveal-hold after pointer-leave before the hide slide starts (docs/11
 * sec.5: "revealHold 250 ms sticky after unhover"). */
#define DC_DOCK_HOLD_MS 250

/* Hot-zone strip thickness (surface-local px) that stays hoverable while the
 * dock is slid away, so the mouse can still find it and reveal the dock
 * again -- mirrors Dock.qml's `mask: Region { item: maskItem }`, which
 * shrinks the layer-surface's wl input region to a sliver at the bar-facing
 * edge when `!expanded` instead of running a second layer-shell surface. */
#define DC_DOCK_HOTZONE_PX 2

typedef struct {
    char id[64]; /* app_id / desktop-entry basename */
    bool pinned;
    bool running;
    bool focused;
    uint64_t window_id; /* valid iff running */
} dc_dock_item;

typedef struct {
    char id[64];
    int image; /* nanovg image handle, 0 = resolve failed */
} dc_dock_icon_cache_entry;

/* Layout for the current item_count/icon_size, in "revealed" (unslid)
 * surface-local coordinates. The layer-surface itself is taller than the
 * visible pill (`h` includes `slide_travel` headroom) so the hide animation
 * has somewhere to translate the content to before it's clipped by the GL
 * viewport -- see dock_render()'s nvgTranslate. */
typedef struct {
    float item_x[DC_DOCK_MAX_ITEMS];
    float icon_size;
    float icon_y;
    float dot_y;
    float dot_h;
    float pill_x, pill_y, pill_w, pill_h;
    float w, h;
    float slide_travel;
} dock_layout;

struct dc_dock {
    struct dc_wayland *wl;
    struct dc_egl *egl;
    struct dc_render *render;
    struct dc_niri *niri;
    struct dc_apps *apps;
    struct dc_output *output;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width, logical_height;
    int scale120, phys_width, phys_height;
    bool configured, egl_ready, mapped;
    struct wl_callback *frame_cb;

    dc_dock_item items[DC_DOCK_MAX_ITEMS];
    int item_count;

    dc_dock_icon_cache_entry icon_cache[DC_DOCK_ICON_CACHE_MAX];
    int icon_cache_n;

    /* Auto-hide state (docs/11 sec.5/8): `revealed` is the target state;
     * `reveal_anim` animates the slide toward it. `holding` implements the
     * 250ms sticky delay between pointer-leave and the hide starting. */
    bool revealed;
    dc_anim reveal_anim;
    bool holding;
    int64_t hold_until_ms;

    /* Per-icon hover "lift" (docs/11 sec.5, DockAppButton.qml's bounce --
     * DMS translates the icon+indicator up on hover rather than scaling it;
     * see dock.c's header comment / the task report for why this isn't a
     * magnify effect). */
    int hover_idx;
    dc_anim lift_anim[DC_DOCK_MAX_ITEMS];
    bool lift_target[DC_DOCK_MAX_ITEMS];
};

static void dock_render(dc_dock *d);
static void dock_teardown(dc_dock *d);
static void dock_apply_geometry(dc_dock *d);
static void dock_apply_input_region(dc_dock *d);

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

/* --- layout / hit-testing ------------------------------------------------ */

static dock_layout dock_get_layout(const dc_dock *d)
{
    dock_layout l = {0};
    const float pad = 10.0f, gap = 10.0f, dot_gap = 6.0f, dot_h = 5.0f;
    l.icon_size = (float)dc_config_current->dock_icon_size;
    l.dot_h = dot_h;

    int n = d->item_count;
    float content_w = n > 0 ? (pad * 2.0f + (float)n * l.icon_size + (float)(n - 1) * gap)
                            : (pad * 2.0f + l.icon_size);
    float content_h = pad * 2.0f + l.icon_size + dot_gap + dot_h;
    l.slide_travel = content_h + 10.0f;
    l.w = content_w;
    l.h = content_h + l.slide_travel;

    bool near_bottom = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    l.pill_x = 0.0f;
    l.pill_y = near_bottom ? (l.h - content_h) : 0.0f;
    l.pill_w = content_w;
    l.pill_h = content_h;

    l.icon_y = l.pill_y + pad;
    l.dot_y = l.icon_y + l.icon_size + dot_gap;
    for (int i = 0; i < n && i < DC_DOCK_MAX_ITEMS; i++)
        l.item_x[i] = pad + (float)i * (l.icon_size + gap);
    return l;
}

/* Icon hit-test in the fixed "revealed" layout -- input events only reach us
 * at all when the wl input region admits them (dock_apply_input_region), so
 * this never needs to account for the in-flight slide offset. */
static int dock_hit_test(const dc_dock *d, const dock_layout *l, double x, double y)
{
    if (y < (double)l->icon_y || y > (double)(l->icon_y + l->icon_size))
        return -1;
    for (int i = 0; i < d->item_count; i++) {
        double x0 = (double)l->item_x[i];
        if (x >= x0 && x <= x0 + (double)l->icon_size)
            return i;
    }
    return -1;
}

/* --- item list (pinned + running, deduped by app_id) --------------------- */

static bool niri_find_by_appid(const dc_niri_window *wins, int wcount, const char *app_id,
                               uint64_t *out_id, bool *out_focused)
{
    bool found = false;
    uint64_t id = 0;
    bool focused = false;
    for (int i = 0; i < wcount; i++) {
        if (strcasecmp(wins[i].app_id, app_id) != 0)
            continue;
        if (!found)
            id = wins[i].id;
        found = true;
        if (wins[i].is_focused) {
            id = wins[i].id;
            focused = true;
        }
    }
    *out_id = id;
    *out_focused = focused;
    return found;
}

static bool item_list_has_id(const dc_dock *d, const char *app_id)
{
    for (int i = 0; i < d->item_count; i++)
        if (strcasecmp(d->items[i].id, app_id) == 0)
            return true;
    return false;
}

/* Rebuild d->items: pinned apps (dc_config_current->dock_pinned, in order)
 * first, then any running-but-unpinned windows deduped by app_id -- a
 * minimal grouping, not DMS's full multi-window group/cycle behavior (see
 * the task report for that deviation). */
static void dock_build_items(dc_dock *d)
{
    const dc_config *cfg = dc_config_current;
    d->item_count = 0;

    int wcount = 0;
    const dc_niri_window *wins = dc_niri_windows(d->niri, &wcount);

    for (int i = 0; i < cfg->dock_pinned_n && d->item_count < DC_DOCK_MAX_ITEMS; i++) {
        dc_dock_item *it = &d->items[d->item_count++];
        snprintf(it->id, sizeof(it->id), "%s", cfg->dock_pinned[i]);
        it->pinned = true;
        it->running = niri_find_by_appid(wins, wcount, it->id, &it->window_id, &it->focused);
    }

    for (int i = 0; i < wcount && d->item_count < DC_DOCK_MAX_ITEMS; i++) {
        const char *app_id = wins[i].app_id;
        if (!app_id[0] || item_list_has_id(d, app_id))
            continue;
        dc_dock_item *it = &d->items[d->item_count++];
        snprintf(it->id, sizeof(it->id), "%s", app_id);
        it->pinned = false;
        it->running = niri_find_by_appid(wins, wcount, app_id, &it->window_id, &it->focused);
    }
}

/* --- icons (cached, same linear-scan-then-resolve shape as launcher.c) --- */

static int dock_icon_for(dc_dock *d, const char *app_id, int size)
{
    for (int i = 0; i < d->icon_cache_n; i++)
        if (strcmp(d->icon_cache[i].id, app_id) == 0)
            return d->icon_cache[i].image;

    char *icon_path = dc_icon_resolve(app_id, size, 1);
    int img = 0;
    if (icon_path) {
        img = dc_render_load_icon(d->render, icon_path, size);
        free(icon_path);
    }
    if (d->icon_cache_n < DC_DOCK_ICON_CACHE_MAX) {
        snprintf(d->icon_cache[d->icon_cache_n].id, sizeof(d->icon_cache[d->icon_cache_n].id), "%s",
                app_id);
        d->icon_cache[d->icon_cache_n].image = img;
        d->icon_cache_n++;
    }
    return img;
}

/* --- reveal / auto-hide state machine ------------------------------------ */

static bool dock_needs_frame(const dc_dock *d)
{
    if (dc_anim_active(&d->reveal_anim) || d->holding)
        return true;
    for (int i = 0; i < d->item_count; i++)
        if (dc_anim_active(&d->lift_anim[i]))
            return true;
    return false;
}

static void dock_set_revealed(dc_dock *d, bool target)
{
    if (d->revealed == target)
        return;
    d->revealed = target;
    dc_anim_start(&d->reveal_anim, DC_DUR_DOCK_SLIDE, DC_EASE_STANDARD);
    dock_apply_input_region(d);
    dock_render(d);
}

static void dock_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_dock *d = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    d->frame_cb = NULL;
    if (!d->mapped)
        return;

    if (d->holding && dc_anim_now_ms() >= d->hold_until_ms) {
        d->holding = false;
        dock_set_revealed(d, false);
        return;
    }
    if (dock_needs_frame(d))
        dock_render(d);
}

static const struct wl_callback_listener dock_frame_listener = {.done = dock_frame_done};

/* --- rendering ------------------------------------------------------------ */

static void dock_recompute_physical(dc_dock *d)
{
    d->phys_width = (d->logical_width * d->scale120 + DC_DOCK_SCALE_BASE / 2) / DC_DOCK_SCALE_BASE;
    d->phys_height = (d->logical_height * d->scale120 + DC_DOCK_SCALE_BASE / 2) / DC_DOCK_SCALE_BASE;
}

static void dock_render(dc_dock *d)
{
    if (!d->configured || d->phys_width <= 0)
        return;

    if (!d->egl_ready) {
        if (!dc_egl_window_init(&d->egl_window, d->egl, d->surface, d->phys_width, d->phys_height))
            return;
        d->egl_ready = true;
    } else {
        dc_egl_window_resize(&d->egl_window, d->phys_width, d->phys_height);
    }
    if (!dc_egl_make_current(d->egl, &d->egl_window))
        return;
    if (!dc_render_ensure(d->render))
        return;
    if (d->viewport)
        wp_viewport_set_destination(d->viewport, d->logical_width, d->logical_height);

    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    dock_layout l = dock_get_layout(d);
    bool near_bottom = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;

    glViewport(0, 0, d->phys_width, d->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, (float)d->logical_width, (float)d->logical_height,
                 (float)d->scale120 / DC_DOCK_SCALE_BASE);

    if (d->item_count > 0) {
        /* Slide offset: `revealed` is the target, reveal_anim's progress (0..1
         * from whenever the target last flipped) drives the transition
         * between full-travel (hidden) and zero (shown). Same
         * progress-with-a-direction-flag shape as battery_popout.c's
         * `closing` handling. */
        float p = dc_anim_progress(&d->reveal_anim);
        float slide = d->revealed ? l.slide_travel * (1.0f - p) : l.slide_travel * p;
        float dy = near_bottom ? slide : -slide;

        nvgSave(vg);
        nvgTranslate(vg, 0.0f, dy);

        NVGpaint shadow = nvgBoxGradient(vg, l.pill_x, l.pill_y + 2.0f, l.pill_w, l.pill_h, 16.0f,
                                         14.0f, nvgRGBA(0, 0, 0, 80), nvgRGBA(0, 0, 0, 0));
        nvgBeginPath(vg);
        nvgRect(vg, l.pill_x - 20.0f, l.pill_y - 20.0f, l.pill_w + 40.0f, l.pill_h + 40.0f);
        nvgRoundedRect(vg, l.pill_x, l.pill_y, l.pill_w, l.pill_h, 16.0f);
        nvgPathWinding(vg, NVG_HOLE);
        nvgFillPaint(vg, shadow);
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, l.pill_x, l.pill_y, l.pill_w, l.pill_h, 16.0f);
        nvgFillColor(vg, nvgRGBA(t->surface_container.r, t->surface_container.g, t->surface_container.b,
                                 255));
        nvgFill(vg);
        nvgStrokeColor(vg, nvgRGBA(t->outline.r, t->outline.g, t->outline.b, 40));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);

        for (int i = 0; i < d->item_count; i++) {
            const dc_dock_item *it = &d->items[i];

            float ip = dc_anim_progress(&d->lift_anim[i]);
            float amount = l.icon_size * 0.22f;
            float lift = d->lift_target[i] ? amount * ip : amount * (1.0f - ip);

            float ix = l.item_x[i];
            float iy = l.icon_y - lift;
            float cx = ix + l.icon_size / 2.0f;
            float cy = iy + l.icon_size / 2.0f;

            int img = dock_icon_for(d, it->id, (int)l.icon_size);
            if (img > 0) {
                NVGpaint imgp = nvgImagePattern(vg, ix, iy, l.icon_size, l.icon_size, 0.0f, img, 1.0f);
                nvgBeginPath(vg);
                nvgRect(vg, ix, iy, l.icon_size, l.icon_size);
                nvgFillPaint(vg, imgp);
                nvgFill(vg);
            } else {
                dc_render_icon(d->render, DC_ICON_APPS, cx, cy, l.icon_size * 0.7f, t->surface_text,
                              NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            }

            if (it->running) {
                dc_color dot_color = it->focused ? t->primary : t->surface_variant_text;
                float dot_w = 6.0f;
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx - dot_w / 2.0f, l.dot_y - lift, dot_w, l.dot_h, l.dot_h / 2.0f);
                nvgFillColor(vg, tc(dot_color));
                nvgFill(vg);
            }
        }

        nvgRestore(vg);
    }

    nvgEndFrame(vg);

    if (dock_needs_frame(d) && !d->frame_cb) {
        d->frame_cb = wl_surface_frame(d->surface);
        wl_callback_add_listener(d->frame_cb, &dock_frame_listener, d);
    }
    dc_egl_swap(d->egl, &d->egl_window);
}

/* --- input region (the hot-zone) ----------------------------------------- */

/* wl input region: the full pill rect while shown/animating, or a thin strip
 * flush at the bar-facing edge (where the pill's near edge sits when
 * revealed) while hidden -- see the DC_DOCK_HOTZONE_PX comment above. */
static void dock_apply_input_region(dc_dock *d)
{
    if (!d->surface || d->logical_width <= 0 || d->logical_height <= 0)
        return;

    struct wl_region *region = wl_compositor_create_region(d->wl->compositor);
    bool full = !dc_config_current->dock_auto_hide || d->revealed || dc_anim_active(&d->reveal_anim);
    if (full) {
        wl_region_add(region, 0, 0, d->logical_width, d->logical_height);
    } else {
        bool near_bottom = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
        int hz = DC_DOCK_HOTZONE_PX;
        int y = near_bottom ? d->logical_height - hz : 0;
        if (y < 0)
            y = 0;
        wl_region_add(region, 0, y, d->logical_width, hz);
    }
    wl_surface_set_input_region(d->surface, region);
    wl_region_destroy(region);
}

/* --- surface lifecycle ----------------------------------------------------*/

static void dock_apply_geometry(dc_dock *d)
{
    if (!d->layer_surface)
        return;

    dock_layout l = dock_get_layout(d);
    d->logical_width = (int)ceilf(l.w) > 0 ? (int)ceilf(l.w) : 1;
    d->logical_height = (int)ceilf(l.h) > 0 ? (int)ceilf(l.h) : 1;

    dc_popout_anchor pa = dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_CENTER, 0);
    zwlr_layer_surface_v1_set_anchor(d->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(d->layer_surface, (uint32_t)d->logical_width,
                                   (uint32_t)d->logical_height);
    zwlr_layer_surface_v1_set_margin(d->layer_surface, pa.margin_top, pa.margin_right, pa.margin_bottom,
                                     pa.margin_left);
    /* Floats over content rather than reserving space, like every other
     * dankc panel (docs/13-POPOUTS-SPEC.md's popouts all use -1 too) --
     * simpler than DMS's conditional dockReserveZone/dock-exclusion surface
     * for a v1 pass; see the task report. */
    zwlr_layer_surface_v1_set_exclusive_zone(d->layer_surface, -1);

    dock_apply_input_region(d);
    wl_surface_commit(d->surface);
}

static void dock_layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                                uint32_t serial, uint32_t width, uint32_t height)
{
    dc_dock *d = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    if (width > 0)
        d->logical_width = (int)width;
    if (height > 0)
        d->logical_height = (int)height;
    d->configured = true;
    dock_recompute_physical(d);
    dock_render(d);
}

static void dock_layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_dock *d = data;
    DC_UNUSED(surface);
    d->configured = false;
}

static const struct zwlr_layer_surface_v1_listener dock_layer_surface_listener = {
    .configure = dock_layer_surface_handle_configure,
    .closed = dock_layer_surface_handle_closed,
};

static void dock_fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                                    uint32_t scale)
{
    dc_dock *d = data;
    DC_UNUSED(fs);
    d->scale120 = (int)scale;
    dock_recompute_physical(d);
    dock_render(d);
}

static const struct wp_fractional_scale_v1_listener dock_fractional_scale_listener = {
    .preferred_scale = dock_fractional_scale_handle_preferred,
};

static void dock_teardown(dc_dock *d)
{
    if (d->frame_cb) {
        wl_callback_destroy(d->frame_cb);
        d->frame_cb = NULL;
    }
    if (d->egl_ready)
        dc_egl_window_finish(&d->egl_window, d->egl);
    if (d->viewport)
        wp_viewport_destroy(d->viewport);
    if (d->fractional_scale)
        wp_fractional_scale_v1_destroy(d->fractional_scale);
    if (d->layer_surface)
        zwlr_layer_surface_v1_destroy(d->layer_surface);
    if (d->surface)
        wl_surface_destroy(d->surface);
    d->egl_ready = false;
    d->configured = false;
    d->viewport = NULL;
    d->fractional_scale = NULL;
    d->layer_surface = NULL;
    d->surface = NULL;
    d->mapped = false;
}

/* --- public API ------------------------------------------------------------*/

dc_dock *dc_dock_create(dc_wayland *wl, dc_egl *egl, dc_render *render, dc_niri *niri)
{
    dc_dock *d = calloc(1, sizeof(*d));
    d->wl = wl;
    d->egl = egl;
    d->render = render;
    d->niri = niri;
    d->apps = dc_apps_load();
    d->hover_idx = -1;
    d->scale120 = DC_DOCK_SCALE_BASE;
    return d;
}

void dc_dock_destroy(dc_dock *d)
{
    if (!d)
        return;
    if (d->mapped)
        dock_teardown(d);
    dc_apps_destroy(d->apps);
    free(d);
}

void dc_dock_show(dc_dock *d, dc_output *output)
{
    if (d->mapped || !output)
        return;

    d->output = output;
    d->configured = false;
    d->egl_ready = false;
    d->scale120 = (output->scale > 0 ? output->scale : 1) * DC_DOCK_SCALE_BASE;
    d->hover_idx = -1;
    d->holding = false;
    d->reveal_anim = (dc_anim){0};
    /* Start already at the target state (no animation) -- matches every
     * other panel's create-on-show; only later reveal/hide transitions
     * animate. */
    d->revealed = !dc_config_current->dock_auto_hide;

    dock_build_items(d);

    d->surface = wl_compositor_create_surface(d->wl->compositor);
    if (d->wl->fractional_scale_mgr) {
        d->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            d->wl->fractional_scale_mgr, d->surface);
        wp_fractional_scale_v1_add_listener(d->fractional_scale, &dock_fractional_scale_listener, d);
    }
    if (d->wl->viewporter)
        d->viewport = wp_viewporter_get_viewport(d->wl->viewporter, d->surface);

    d->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        d->wl->layer_shell, d->surface, output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "dankc:dock");
    zwlr_layer_surface_v1_add_listener(d->layer_surface, &dock_layer_surface_listener, d);

    dock_apply_geometry(d);
    d->mapped = true;
    dc_info("dock shown on output %s (%d item%s, autoHide=%d)", output->model ? output->model : "?",
            d->item_count, d->item_count == 1 ? "" : "s", dc_config_current->dock_auto_hide);
}

void dc_dock_hide(dc_dock *d)
{
    if (!d->mapped)
        return;
    dock_teardown(d);
    dc_debug("dock hidden");
}

void dc_dock_toggle(dc_dock *d, dc_output *output)
{
    if (d->mapped)
        dc_dock_hide(d);
    else
        dc_dock_show(d, output);
}

bool dc_dock_visible(dc_dock *d)
{
    return d->mapped;
}

struct wl_surface *dc_dock_surface(dc_dock *d)
{
    return d->mapped ? d->surface : NULL;
}

void dc_dock_refresh(dc_dock *d)
{
    dock_build_items(d);
    if (!d->mapped)
        return;
    /* Item count changes the pill width; set_size again (harmless no-op if
     * unchanged) then repaint directly -- an unchanged size won't trigger a
     * fresh configure, same reasoning as dc_bar_reconfigure(). */
    dock_apply_geometry(d);
    dock_render(d);
}

void dc_dock_handle_click(dc_dock *d, double x, double y)
{
    if (!d->mapped)
        return;
    dock_layout l = dock_get_layout(d);
    int idx = dock_hit_test(d, &l, x, y);
    if (idx < 0 || idx >= d->item_count)
        return;

    dc_dock_item *it = &d->items[idx];
    if (it->running && it->window_id != 0) {
        dc_niri_focus_window(it->window_id);
        return;
    }

    const dc_app *app = d->apps ? dc_apps_find(d->apps, it->id) : NULL;
    if (app) {
        dc_app_launch(app);
    } else {
        dc_info("dock: '%s' has no indexed desktop entry; exec'ing literally", it->id);
        dc_app_launch_exec(it->id);
    }
}

void dc_dock_handle_motion(dc_dock *d, double x, double y)
{
    if (!d->mapped)
        return;

    d->holding = false;
    dock_set_revealed(d, true);

    dock_layout l = dock_get_layout(d);
    int idx = dock_hit_test(d, &l, x, y);
    if (idx == d->hover_idx)
        return;

    if (d->hover_idx >= 0) {
        d->lift_target[d->hover_idx] = false;
        dc_anim_start(&d->lift_anim[d->hover_idx], DC_DUR_SHORT, DC_EASE_STANDARD);
    }
    d->hover_idx = idx;
    if (idx >= 0) {
        d->lift_target[idx] = true;
        dc_anim_start(&d->lift_anim[idx], DC_DUR_SHORT, DC_EASE_EXPRESSIVE);
    }
    dock_render(d);
}

void dc_dock_handle_leave(dc_dock *d)
{
    if (!d->mapped)
        return;

    if (d->hover_idx >= 0) {
        d->lift_target[d->hover_idx] = false;
        dc_anim_start(&d->lift_anim[d->hover_idx], DC_DUR_SHORT, DC_EASE_STANDARD);
        d->hover_idx = -1;
    }
    if (dc_config_current->dock_auto_hide) {
        d->holding = true;
        d->hold_until_ms = dc_anim_now_ms() + DC_DOCK_HOLD_MS;
    }
    dock_render(d);
}
