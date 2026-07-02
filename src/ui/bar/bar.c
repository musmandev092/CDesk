#include "ui/bar/bar.h"

#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "niri/niri.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/audio.h"
#include "services/battery.h"
#include "services/bluez.h"
#include "services/icons.h"
#include "services/mpris.h"
#include "services/net.h"
#include "services/tray.h"
#include "theme/theme.h"
#include "ui/bar/bar_tokens.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <ctype.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Fallback logical height before the first layer-surface configure lands
 * (docs/12-BAR-SPEC.md sec.1 overrides this via dc_bar_geometry()). */
#define DC_BAR_HEIGHT_FALLBACK 48

/* Corner radius of the floating bar rect (docs/12-BAR-SPEC.md sec.1). */
#define DC_BAR_CORNER_RADIUS 12.0f

/* Fractional-scale numerator base: preferred_scale is reported over 120. */
#define DC_SCALE_BASE 120

/* Bar container geometry, derived from config (docs/12-BAR-SPEC.md sec.1):
 *   widgetThickness       = max(20, 26 + innerPadding*0.6)     (bar_tokens.h)
 *   effectiveBarThickness = max(widgetThickness + innerPadding + 4,
 *                                48 - 4 - (8 - innerPadding))
 *   windowHeight           = effectiveBarThickness + spacing (layer-surface
 *                             size + exclusive zone; the extra `spacing` px
 *                             is the transparent gap toward the outer edge)
 */
typedef struct {
    float widget_thickness;
    float effective_thickness;
    int window_height;
} dc_bar_geometry;

static dc_bar_geometry bar_compute_geometry(const dc_config *cfg)
{
    float ip = (float)cfg->bar_inner_padding;
    dc_bar_geometry g;
    g.widget_thickness = dc_bar_widget_thickness(cfg);
    g.effective_thickness = fmaxf(g.widget_thickness + ip + 4.0f, 48.0f - 4.0f - (8.0f - ip));
    g.window_height = (int)lroundf(g.effective_thickness) + cfg->bar_spacing;
    return g;
}

/* dc_color -> nanovg color. */
static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline NVGcolor tc_alpha(dc_color c, int alpha)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)alpha);
}

/* A placed BasePill: every widget's container (docs/12-BAR-SPEC.md sec.3).
 * `content_x0` is where the widget's own drawing starts; `cy` is the bar's
 * vertical center, handed through so widgets never recompute it. */
typedef struct {
    float x, y, w, h;
    float content_x0;
    float cy;
} dc_pill;

/* One placed widget's click target, recorded by the layout pass and consumed
 * by dc_bar_hittest() (docs/12-BAR-SPEC.md sec.5). `payload` is region-
 * specific extra data (currently only the workspace index). */
typedef struct {
    float x0, y0, x1, y1;
    dc_bar_region region;
    int payload;
} dc_bar_hit;

#define DC_BAR_MAX_HITS 32

struct dc_bar {
    dc_wayland *wl;
    dc_output *output;
    dc_egl *egl;
    dc_render *render;
    dc_niri *niri;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    /* Focused-app icon cache (avoids re-decoding every frame). */
    char icon_app_id[64];
    int icon_image; /* nanovg image handle, 0 = none */

    /* System tray (StatusNotifier) + a per-name nanovg image cache. */
    struct dc_tray *tray;
    struct {
        char name[DC_TRAY_STR];
        int image;
    } tray_cache[DC_TRAY_MAX];
    int tray_cache_n;

    /* Per-widget click targets from the most recent render (widget host
     * layout pass; docs/12-BAR-SPEC.md sec.5). */
    dc_bar_hit hits[DC_BAR_MAX_HITS];
    int hit_count;

    int logical_width;  /* from the layer-surface configure */
    int logical_height;
    int scale120; /* fractional scale numerator (120 == 1.0x) */
    int phys_width; /* buffer size = logical * scale120 / 120 */
    int phys_height;

    /* The floating bar rect within the (larger, mostly transparent)
     * layer-surface buffer — see bar_compute_geometry() and
     * recompute_content_rect(). Widgets draw relative to this rect, not the
     * full surface. */
    float rect_x, rect_y, rect_w, rect_h;

    bool configured;
    bool egl_ready;
};

static void recompute_physical(dc_bar *bar)
{
    bar->phys_width = (bar->logical_width * bar->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    bar->phys_height = (bar->logical_height * bar->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Position the floating rect inside the surface: spacing-px gap on the outer
 * (screen) edge, flush against the inner (desktop-facing) edge, spacing-px
 * inset from the left/right sides. Call whenever logical size changes. */
static void recompute_content_rect(dc_bar *bar)
{
    const dc_config *cfg = dc_config_current;
    dc_bar_geometry geo = bar_compute_geometry(cfg);
    float spacing = (float)cfg->bar_spacing;

    bar->rect_x = spacing;
    bar->rect_w = (float)bar->logical_width - 2.0f * spacing;
    bar->rect_h = geo.effective_thickness;
    bar->rect_y = (cfg->bar_position == DC_BAR_POSITION_TOP) ? spacing : 0.0f;
}

/* Vertical center of the visible rect (widgets are y-centered in it). */
static inline float bar_cy(const dc_bar *bar)
{
    return bar->rect_y + bar->rect_h / 2.0f;
}

/* Left/right content edges: spec content inset = max(4, innerPadding*0.8). */
static inline float bar_content_inset(void)
{
    const dc_config *cfg = dc_config_current;
    return fmaxf(4.0f, (float)cfg->bar_inner_padding * 0.8f);
}

static inline float bar_content_x0(const dc_bar *bar)
{
    return bar->rect_x + bar_content_inset();
}

static inline float bar_content_x1(const dc_bar *bar)
{
    return bar->rect_x + bar->rect_w - bar_content_inset();
}

/* --- BasePill ------------------------------------------------------------ */

/* Compute (but don't draw) the pill for `content_w` px of content with its
 * left edge at `x` (docs/12-BAR-SPEC.md sec.3): height = widgetThickness,
 * width = content + 2*hpad, vertically centered in the bar rect. */
static dc_pill pill_at(const dc_bar *bar, float x, float content_w)
{
    const dc_config *cfg = dc_config_current;
    float h = dc_bar_widget_thickness(cfg);
    float hpad = dc_bar_hpad(cfg);
    dc_pill p;
    p.h = h;
    p.w = content_w + 2.0f * hpad;
    p.x = x;
    p.y = bar_cy(bar) - h / 2.0f;
    p.cy = bar_cy(bar);
    p.content_x0 = x + hpad;
    return p;
}

/* The stadium background: surfaceContainerHigh x widgetTransparency. Widgets
 * that want a transparent chip (e.g. systemTray, for now) simply skip this
 * call — see the widget table's `has_bg`. */
static void pill_draw_bg(dc_bar *bar, const dc_pill *p)
{
    NVGcontext *vg = bar->render->vg;
    const dc_config *cfg = dc_config_current;
    dc_color c = dc_theme_current->surface_container_high;
    unsigned char a = (unsigned char)((c.a / 255.0f) * cfg->bar_widget_transparency * 255.0f);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, p->x, p->y, p->w, p->h, p->h / 2.0f);
    nvgFillColor(vg, nvgRGBA(c.r, c.g, c.b, a));
    nvgFill(vg);
}

/* Record a click target for the current render (docs/12-BAR-SPEC.md sec.5).
 * Silently dropped past DC_BAR_MAX_HITS (generous headroom for one bar). */
static void bar_push_hit(dc_bar *bar, float x0, float x1, dc_bar_region region, int payload)
{
    if (bar->hit_count >= DC_BAR_MAX_HITS)
        return;
    dc_bar_hit *h = &bar->hits[bar->hit_count++];
    h->x0 = x0;
    h->x1 = x1;
    h->y0 = bar->rect_y;
    h->y1 = bar->rect_y + bar->rect_h;
    h->region = region;
    h->payload = payload;
}

/* --- launcherButton / clipboard / notificationButton: single centered icon */

static void draw_icon_centered(dc_bar *bar, const dc_pill *p, int codepoint, dc_color color)
{
    const dc_config *cfg = dc_config_current;
    dc_render_icon(bar->render, codepoint, p->x + p->w / 2.0f, p->cy, dc_bar_icon_size(cfg, -4),
                  color, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

static float measure_launcher(dc_bar *bar)
{
    DC_UNUSED(bar);
    return dc_bar_icon_size(dc_config_current, -4);
}

static void draw_launcher_pill(dc_bar *bar, const dc_pill *p)
{
    draw_icon_centered(bar, p, DC_ICON_APPS, dc_theme_current->surface_text);
}

static float measure_clipboard(dc_bar *bar)
{
    DC_UNUSED(bar);
    return dc_bar_icon_size(dc_config_current, -4);
}

static void draw_clipboard_pill(dc_bar *bar, const dc_pill *p)
{
    draw_icon_centered(bar, p, DC_ICON_CONTENT_PASTE, dc_theme_current->surface_text);
}

static float measure_notif(dc_bar *bar)
{
    DC_UNUSED(bar);
    return dc_bar_icon_size(dc_config_current, -4);
}

static void draw_notif_pill(dc_bar *bar, const dc_pill *p)
{
    draw_icon_centered(bar, p, DC_ICON_NOTIFICATIONS, dc_theme_current->surface_text);
}

/* --- workspaceSwitcher ---------------------------------------------------- */

/* True if the focused window lives on this bar's output. */
static bool focused_window_on_output(dc_bar *bar, const dc_niri_window *win)
{
    if (!bar->output->name)
        return true; /* no connector name to filter by */

    int count = 0;
    const dc_niri_workspace *workspaces = dc_niri_workspaces(bar->niri, &count);
    for (int i = 0; i < count; i++) {
        if (workspaces[i].id != win->workspace_id)
            continue;
        return workspaces[i].output[0] == '\0' ||
               strcmp(workspaces[i].output, bar->output->name) == 0;
    }
    return false;
}

/* Workspace capsules, filtered to this bar's output, starting at `x0`. One
 * function drives both measurement (`draw`=false, from x0=0) and painting
 * (`draw`=true) so the two can never drift apart; painting also records each
 * capsule's own hit rect (workspaceSwitcher owns finer-grained hits than the
 * rest of the widget host — docs/12-BAR-SPEC.md sec.4/5). Returns the x just
 * past the last capsule. */
static float layout_workspaces(dc_bar *bar, float x0, bool draw)
{
    if (!bar->niri)
        return x0;

    int count = 0;
    const dc_niri_workspace *workspaces = dc_niri_workspaces(bar->niri, &count);
    if (!workspaces)
        return x0;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const float cy = bar_cy(bar);
    const float gap = 8.0f;
    const float dot = 9.0f;     /* inactive workspace dot diameter */
    const float pill_w = 30.0f; /* focused workspace pill */
    const float pill_h = 11.0f;
    float x = x0;
    bool any = false;

    /* DMS style: focused = wide primary pill, others = dots (brighter if the
     * active/visible workspace on their output). */
    for (int i = 0; i < count; i++) {
        const dc_niri_workspace *ws = &workspaces[i];
        if (bar->output->name && ws->output[0] && strcmp(ws->output, bar->output->name) != 0)
            continue;

        float item_w = ws->is_focused ? pill_w : dot;
        if (draw) {
            if (ws->is_focused) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, x, cy - pill_h / 2.0f, pill_w, pill_h, pill_h / 2.0f);
                nvgFillColor(vg, tc(ws->is_urgent ? t->error : t->primary));
                nvgFill(vg);
            } else {
                nvgBeginPath(vg);
                nvgCircle(vg, x + dot / 2.0f, cy, dot / 2.0f);
                if (ws->is_urgent)
                    nvgFillColor(vg, tc(t->error));
                else if (ws->is_active)
                    nvgFillColor(vg, tc(t->surface_variant_text));
                else
                    nvgFillColor(vg, tc_alpha(t->outline, 150));
                nvgFill(vg);
            }
            bar_push_hit(bar, x, x + item_w, DC_BAR_REGION_WORKSPACE, ws->idx);
        }
        x += item_w + gap;
        any = true;
    }
    if (any)
        x -= gap; /* no trailing gap past the last capsule */
    return x;
}

static float measure_workspaces(dc_bar *bar)
{
    return layout_workspaces(bar, 0.0f, false);
}

static void draw_workspaces_pill(dc_bar *bar, const dc_pill *p)
{
    layout_workspaces(bar, p->content_x0, true);
}

/* --- focusedWindow --------------------------------------------------------- */

/* [app icon] "AppName · Title", clipped to a max content width of 456px
 * (docs/12-BAR-SPEC.md sec.4). Shared measure/draw, like layout_workspaces()
 * above. Returns 0 (the "absent this frame" sentinel — see find_widget()'s
 * callers) when there is no focused window on this output. */
static float layout_focused_window(dc_bar *bar, float x0, bool draw)
{
    const dc_niri_window *win = dc_niri_focused_window(bar->niri);
    if (!win || !focused_window_on_output(bar, win))
        return 0.0f;

    /* Pretty app name from the app_id (last dotted component, capitalised),
     * then "AppName · Title" like DMS. */
    char app_name[64] = {0};
    if (win->app_id[0]) {
        const char *base = strrchr(win->app_id, '.');
        base = base ? base + 1 : win->app_id;
        snprintf(app_name, sizeof(app_name), "%s", base);
        app_name[0] = (char)toupper((unsigned char)app_name[0]);
    }

    char label[DC_NIRI_TITLE_MAX + 96];
    if (app_name[0] && win->title[0])
        snprintf(label, sizeof(label), "%s \xc2\xb7 %s", app_name, win->title);
    else if (win->title[0])
        snprintf(label, sizeof(label), "%s", win->title);
    else
        snprintf(label, sizeof(label), "%s", app_name);
    if (!label[0])
        return 0.0f;

    NVGcontext *vg = bar->render->vg;
    const dc_config *cfg = dc_config_current;
    const float cy = bar_cy(bar);
    float x = x0;

    /* Refresh the cached app icon only when the focused app changes; done on
     * both the measure and draw pass (idempotent past the first) so the
     * width computed by measure already reflects this frame's icon. */
    if (strcmp(win->app_id, bar->icon_app_id) != 0) {
        if (bar->icon_image > 0)
            nvgDeleteImage(vg, bar->icon_image);
        bar->icon_image = 0;
        char *path = dc_icon_resolve(win->app_id, 24, 1);
        if (path) {
            bar->icon_image = dc_render_load_icon(bar->render, path, 22);
            free(path);
        }
        snprintf(bar->icon_app_id, sizeof(bar->icon_app_id), "%s", win->app_id);
    }

    const float isz = dc_bar_icon_size(cfg, -4);
    if (bar->icon_image > 0) {
        if (draw) {
            NVGpaint pat =
                nvgImagePattern(vg, x, cy - isz / 2.0f, isz, isz, 0.0f, bar->icon_image, 1.0f);
            nvgBeginPath(vg);
            nvgRect(vg, x, cy - isz / 2.0f, isz, isz);
            nvgFillPaint(vg, pat);
            nvgFill(vg);
        }
        x += isz + 8.0f;
    }

    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
    float text_w = bounds[2] - bounds[0];

    const float max_total = 456.0f;
    float avail = max_total - (x - x0);
    if (avail < 20.0f)
        avail = 20.0f;
    if (text_w > avail)
        text_w = avail; /* TODO(bar-s3): true ellipsis instead of a hard clip */

    if (draw) {
        nvgSave(vg);
        nvgScissor(vg, x, bar->rect_y, text_w, bar->rect_h);
        nvgFillColor(vg, tc(dc_theme_current->surface_text));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, x, cy, label, NULL);
        nvgRestore(vg);
    }
    x += text_w;
    return x - x0;
}

static float measure_focused_window(dc_bar *bar)
{
    return layout_focused_window(bar, 0.0f, false);
}

static void draw_focused_window_pill(dc_bar *bar, const dc_pill *p)
{
    layout_focused_window(bar, p->content_x0, true);
}

/* --- clock ------------------------------------------------------------- */

/* "HH:MM  Www D" (honours use24HourClock/showDate). Shared measure/draw. */
static float layout_clock(dc_bar *bar, float x0, bool draw)
{
    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    const float cy = bar_cy(bar);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char time_str[16];
    char date_str[32];
    strftime(time_str, sizeof(time_str), cfg->clock_24h ? "%H:%M" : "%-I:%M %p", &tm);
    strftime(date_str, sizeof(date_str), "%a %-d", &tm); /* e.g. "Wed 1" */

    const float gap = 10.0f;
    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, time_str, NULL, bounds);
    const float time_w = bounds[2] - bounds[0];
    float date_w = 0.0f;
    if (cfg->show_date) {
        nvgTextBounds(vg, 0.0f, 0.0f, date_str, NULL, bounds);
        date_w = bounds[2] - bounds[0];
    }
    const float total = cfg->show_date ? time_w + gap + date_w : time_w;

    if (draw) {
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, x0, cy, time_str, NULL);
        if (cfg->show_date) {
            nvgFillColor(vg, tc(t->surface_variant_text));
            nvgText(vg, x0 + time_w + gap, cy, date_str, NULL);
        }
    }
    return total;
}

static float measure_clock(dc_bar *bar)
{
    return layout_clock(bar, 0.0f, false);
}

static void draw_clock_pill(dc_bar *bar, const dc_pill *p)
{
    layout_clock(bar, p->content_x0, true);
}

/* --- battery ------------------------------------------------------------ */

/* Pictograph + "NN%", left-to-right. Shared measure/draw; returns 0 (absent)
 * when there is no battery. Spec sec.4 wants a Material glyph instead of the
 * hand-drawn pictograph — deferred to S3. */
static float layout_battery(dc_bar *bar, float x0, bool draw)
{
    dc_battery_info bat;
    if (!dc_battery_read(&bat) || !bat.present)
        return 0.0f;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const float cy = bar_cy(bar);
    const bool low = bat.percent <= 20 && !bat.charging;

    const NVGcolor text_color = low ? tc(t->error) : tc(t->surface_text);
    const NVGcolor fill_color = bat.charging ? tc(t->success) : text_color;
    const NVGcolor outline = tc_alpha(t->outline, 180);

    const float body_w = 22.0f, body_h = 11.0f, nub_w = 2.0f, nub_h = 5.0f, inset = 2.0f;
    float x = x0;

    if (draw) {
        float by = cy - body_h / 2.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, by, body_w, body_h, 2.5f);
        nvgStrokeColor(vg, outline);
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + body_w, cy - nub_h / 2.0f, nub_w, nub_h, 1.0f);
        nvgFillColor(vg, outline);
        nvgFill(vg);

        float fill_w = (body_w - 2.0f * inset) * (bat.percent / 100.0f);
        if (fill_w > 0.5f) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + inset, by + inset, fill_w, body_h - 2.0f * inset, 1.0f);
            nvgFillColor(vg, fill_color);
            nvgFill(vg);
        }
    }
    x += body_w + nub_w + 6.0f;

    char label[8];
    snprintf(label, sizeof(label), "%d%%", bat.percent);
    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
    float text_w = bounds[2] - bounds[0];

    if (draw) {
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, text_color);
        nvgText(vg, x, cy, label, NULL);
    }
    x += text_w;
    return x - x0;
}

static float measure_battery(dc_bar *bar)
{
    return layout_battery(bar, 0.0f, false);
}

static void draw_battery_pill(dc_bar *bar, const dc_pill *p)
{
    layout_battery(bar, p->content_x0, true);
}

/* --- systemTray ----------------------------------------------------------- */

/* Resolve+load a tray icon by name, cached per name (nanovg image handle). */
static int get_tray_image(dc_bar *bar, const char *name)
{
    for (int i = 0; i < bar->tray_cache_n; i++)
        if (strcmp(bar->tray_cache[i].name, name) == 0)
            return bar->tray_cache[i].image;

    int img = 0;
    char *path = dc_icon_resolve(name, 20, 1);
    if (path) {
        img = dc_render_load_icon(bar->render, path, 20);
        free(path);
    }
    if (bar->tray_cache_n < DC_TRAY_MAX) {
        snprintf(bar->tray_cache[bar->tray_cache_n].name,
                sizeof(bar->tray_cache[bar->tray_cache_n].name), "%s", name);
        bar->tray_cache[bar->tray_cache_n].image = img;
        bar->tray_cache_n++;
    }
    return img;
}

/* Per-icon step; the spec's own 21x21-chip-per-item styling is S3 work (this
 * widget opts out of the BasePill background for now — see the widget
 * table's `has_bg`). */
#define DC_BAR_TRAY_STEP 26.0f
#define DC_BAR_TRAY_ICON 19.0f

static float measure_tray(dc_bar *bar)
{
    if (!bar->tray)
        return 0.0f;
    const dc_tray_item *items[DC_TRAY_MAX];
    int n = dc_tray_items(bar->tray, items, DC_TRAY_MAX);
    if (n <= 0)
        return 0.0f;
    return (float)n * DC_BAR_TRAY_STEP;
}

static void draw_tray_pill(dc_bar *bar, const dc_pill *p)
{
    if (!bar->tray)
        return;
    const dc_tray_item *items[DC_TRAY_MAX];
    int n = dc_tray_items(bar->tray, items, DC_TRAY_MAX);
    NVGcontext *vg = bar->render->vg;
    float x = p->content_x0;

    for (int i = 0; i < n; i++) {
        float cx = x + DC_BAR_TRAY_STEP / 2.0f;
        int img = items[i]->icon_name[0] ? get_tray_image(bar, items[i]->icon_name) : 0;
        if (img > 0) {
            NVGpaint pat = nvgImagePattern(vg, cx - DC_BAR_TRAY_ICON / 2.0f,
                                           p->cy - DC_BAR_TRAY_ICON / 2.0f, DC_BAR_TRAY_ICON,
                                           DC_BAR_TRAY_ICON, 0.0f, img, 1.0f);
            nvgBeginPath(vg);
            nvgRect(vg, cx - DC_BAR_TRAY_ICON / 2.0f, p->cy - DC_BAR_TRAY_ICON / 2.0f,
                   DC_BAR_TRAY_ICON, DC_BAR_TRAY_ICON);
            nvgFillPaint(vg, pat);
            nvgFill(vg);
        } else {
            /* Fallback: a small dot for pixmap-only / unresolved items. */
            nvgBeginPath(vg);
            nvgCircle(vg, cx, p->cy, 4.0f);
            nvgFillColor(vg, tc_alpha(dc_theme_current->surface_text, 200));
            nvgFill(vg);
        }
        x += DC_BAR_TRAY_STEP;
    }
}

/* --- controlCenterButton (temporary stand-in) ----------------------------- */

/* DMS's compound pill (network/vpn/bluetooth/audio/screenSharing) is S4 work.
 * For now this slot renders dankc's existing volume/bluetooth/wifi trio as a
 * single pill so the right cluster isn't empty — docs/12-BAR-SPEC.md sec.4
 * says this REPLACES the standalone icons entirely, which is why they no
 * longer have their own top-level draw calls. */
static float measure_cc_stub(dc_bar *bar)
{
    DC_UNUSED(bar);
    float isz = dc_bar_icon_size(dc_config_current, -4);
    return isz * 3.0f + DC_BAR_WIDGET_SPACING * 2.0f;
}

static void draw_cc_stub_pill(dc_bar *bar, const dc_pill *p)
{
    const dc_config *cfg = dc_config_current;
    const dc_theme *t = dc_theme_current;
    const float isz = dc_bar_icon_size(cfg, -4);
    const int align = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE;
    float x = p->content_x0;

    /* Volume — live level + mute state from wpctl. */
    dc_audio_info audio;
    bool have_audio = dc_audio_read(&audio);
    int volume_icon = DC_ICON_VOLUME_UP;
    if (have_audio && audio.muted)
        volume_icon = DC_ICON_VOLUME_OFF;
    else if (have_audio && audio.volume < 34)
        volume_icon = DC_ICON_VOLUME_DOWN;
    dc_color volume_color = (have_audio && audio.muted) ? t->outline : t->surface_text;
    dc_render_icon(bar->render, volume_icon, x, p->cy, isz, volume_color, align);
    x += isz + DC_BAR_WIDGET_SPACING;

    /* Bluetooth — info-blue when a device is connected, mid when powered, dim off. */
    dc_bluez_info bt;
    bool have_bt = dc_bluez_read(&bt);
    dc_color bt_color = t->outline;
    if (have_bt && bt.connected)
        bt_color = t->info;
    else if (have_bt && bt.powered)
        bt_color = t->surface_variant_text;
    dc_render_icon(bar->render, DC_ICON_BLUETOOTH, x, p->cy, isz, bt_color, align);
    x += isz + DC_BAR_WIDGET_SPACING;

    /* Wi-Fi — green when connected (sysfs), dim otherwise. */
    dc_net_info net;
    dc_net_wifi(&net);
    int wifi_icon = net.connected ? DC_ICON_WIFI : DC_ICON_NETWORK_WIFI;
    dc_color wifi_color = net.connected ? t->primary : t->outline;
    dc_render_icon(bar->render, wifi_icon, x, p->cy, isz, wifi_color, align);
}

/* --- widget host: config-driven left/center/right sections --------------- */

typedef struct {
    const char *id;
    float (*measure)(dc_bar *bar);
    void (*draw)(dc_bar *bar, const dc_pill *p);
    bool has_bg;      /* draw the BasePill background? (opt out: e.g. tray) */
    bool custom_hit;  /* widget records its own hit rect(s); host skips the default */
    dc_bar_region region; /* used when !custom_hit */
} dc_bar_widget_def;

/* Table of widget ids this stage implements. Unknown/unimplemented ids
 * (music, weather, cpuUsage, memUsage — arriving in S4) are skipped silently
 * by find_widget() returning NULL; see docs/12-BAR-SPEC.md sec.7 S2/S4. */
static const dc_bar_widget_def *find_widget(const char *id)
{
    static const dc_bar_widget_def table[] = {
        {"launcherButton", measure_launcher, draw_launcher_pill, true, false,
         DC_BAR_REGION_LAUNCHER},
        {"workspaceSwitcher", measure_workspaces, draw_workspaces_pill, true, true,
         DC_BAR_REGION_NONE},
        {"focusedWindow", measure_focused_window, draw_focused_window_pill, true, false,
         DC_BAR_REGION_NONE},
        {"clock", measure_clock, draw_clock_pill, true, false, DC_BAR_REGION_CLOCK},
        {"systemTray", measure_tray, draw_tray_pill, false, false, DC_BAR_REGION_NONE},
        {"clipboard", measure_clipboard, draw_clipboard_pill, true, false,
         DC_BAR_REGION_CLIPBOARD},
        {"notificationButton", measure_notif, draw_notif_pill, true, false,
         DC_BAR_REGION_NOTIFICATIONS},
        /* battery -> control center for now (docs/12-BAR-SPEC.md sec.5: "battery→battery
         * popout (phase 2: CC)"). */
        {"battery", measure_battery, draw_battery_pill, true, false, DC_BAR_REGION_CONTROL_CENTER},
        {"controlCenterButton", measure_cc_stub, draw_cc_stub_pill, true, false,
         DC_BAR_REGION_CONTROL_CENTER},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].id, id) == 0)
            return &table[i];
    return NULL;
}

/* Draw `def`'s pill at `x` for `content_w` px of content, then record its hit
 * rect unless the widget manages its own (workspaceSwitcher). */
static void place_widget(dc_bar *bar, const dc_bar_widget_def *def, float x, float content_w)
{
    dc_pill p = pill_at(bar, x, content_w);
    if (def->has_bg)
        pill_draw_bg(bar, &p);
    def->draw(bar, &p);
    if (!def->custom_hit)
        bar_push_hit(bar, p.x, p.x + p.w, def->region, 0);
}

/* Left section: pinned to the content's left inset, widgets flow rightward. */
static void layout_left(dc_bar *bar, const char ids[][DC_CONFIG_WIDGET_ID_MAX], int n)
{
    float x = bar_content_x0(bar);
    bool first = true;
    for (int i = 0; i < n; i++) {
        const dc_bar_widget_def *def = find_widget(ids[i]);
        if (!def)
            continue;
        float w = def->measure(bar);
        if (w <= 0.0f)
            continue; /* widget has nothing to show this frame */
        if (!first)
            x += DC_BAR_WIDGET_SPACING;
        place_widget(bar, def, x, w);
        x += pill_at(bar, 0.0f, w).w;
        first = false;
    }
}

/* Right section: pinned to the content's right inset, laid out right-to-left
 * (array order left-to-right, so the last id ends up flush against the
 * right edge — e.g. controlCenterButton). */
static void layout_right(dc_bar *bar, const char ids[][DC_CONFIG_WIDGET_ID_MAX], int n)
{
    float x = bar_content_x1(bar);
    bool first = true;
    for (int i = n - 1; i >= 0; i--) {
        const dc_bar_widget_def *def = find_widget(ids[i]);
        if (!def)
            continue;
        float w = def->measure(bar);
        if (w <= 0.0f)
            continue;
        float pill_w = pill_at(bar, 0.0f, w).w;
        if (!first)
            x -= DC_BAR_WIDGET_SPACING;
        x -= pill_w;
        place_widget(bar, def, x, w);
        first = false;
    }
}

/* Center section: "index centering" — the middle widget of the array is
 * centered exactly on the bar rect, and the rest flow outward from it, so
 * that widget's position never shifts as its neighbours resize
 * (docs/12-BAR-SPEC.md sec.2). */
static void layout_center(dc_bar *bar, const char ids[][DC_CONFIG_WIDGET_ID_MAX], int n)
{
    const dc_bar_widget_def *defs[DC_CONFIG_WIDGETS_MAX];
    float widths[DC_CONFIG_WIDGETS_MAX];
    float pill_widths[DC_CONFIG_WIDGETS_MAX];
    int m = 0;

    for (int i = 0; i < n && m < DC_CONFIG_WIDGETS_MAX; i++) {
        const dc_bar_widget_def *def = find_widget(ids[i]);
        if (!def)
            continue;
        float w = def->measure(bar);
        if (w <= 0.0f)
            continue;
        defs[m] = def;
        widths[m] = w;
        pill_widths[m] = pill_at(bar, 0.0f, w).w;
        m++;
    }
    if (m == 0)
        return;

    int mid = m / 2;
    float cx = bar->rect_x + bar->rect_w / 2.0f;
    float mid_x = cx - pill_widths[mid] / 2.0f;
    place_widget(bar, defs[mid], mid_x, widths[mid]);

    float x = mid_x;
    for (int i = mid - 1; i >= 0; i--) {
        x -= DC_BAR_WIDGET_SPACING;
        x -= pill_widths[i];
        place_widget(bar, defs[i], x, widths[i]);
    }

    x = mid_x + pill_widths[mid];
    for (int i = mid + 1; i < m; i++) {
        x += DC_BAR_WIDGET_SPACING;
        place_widget(bar, defs[i], x, widths[i]);
        x += pill_widths[i];
    }
}

/* Elevation level-2 shadow under the floating rect: a tight "key" shadow
 * offset toward the outer screen edge, plus a soft wide "ambient" shadow
 * (docs/12-BAR-SPEC.md sec.1). Drawn as a box-gradient with the rect punched
 * out as a hole so it never paints over the (possibly translucent) bg fill. */
static void draw_bar_shadow(dc_bar *bar)
{
    NVGcontext *vg = bar->render->vg;
    const dc_config *cfg = dc_config_current;
    const float outer_dir = (cfg->bar_position == DC_BAR_POSITION_TOP) ? -1.0f : 1.0f;

    struct {
        float blur, offset, alpha;
    } layers[2] = {
        {14.0f, 0.0f, 0.125f},           /* ambient */
        {8.0f, 4.0f * outer_dir, 0.25f}, /* key */
    };

    for (int i = 0; i < 2; i++) {
        float blur = layers[i].blur;
        float oy = bar->rect_y + layers[i].offset;
        unsigned char a = (unsigned char)(layers[i].alpha * 255.0f);

        NVGpaint paint = nvgBoxGradient(vg, bar->rect_x, oy, bar->rect_w, bar->rect_h,
                                        DC_BAR_CORNER_RADIUS, blur, nvgRGBA(0, 0, 0, a),
                                        nvgRGBA(0, 0, 0, 0));
        nvgBeginPath(vg);
        nvgRect(vg, bar->rect_x - blur, oy - blur, bar->rect_w + 2.0f * blur,
               bar->rect_h + 2.0f * blur);
        nvgRoundedRect(vg, bar->rect_x, bar->rect_y, bar->rect_w, bar->rect_h,
                       DC_BAR_CORNER_RADIUS);
        nvgPathWinding(vg, NVG_HOLE);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }
}

/* The floating rounded rect itself: surfaceContainer x barTransparency. */
static void draw_bar_background(dc_bar *bar)
{
    NVGcontext *vg = bar->render->vg;
    const dc_config *cfg = dc_config_current;
    dc_color c = dc_theme_current->surface_container;
    unsigned char a = (unsigned char)((c.a / 255.0f) * cfg->bar_transparency * 255.0f);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, bar->rect_x, bar->rect_y, bar->rect_w, bar->rect_h, DC_BAR_CORNER_RADIUS);
    nvgFillColor(vg, nvgRGBA(c.r, c.g, c.b, a));
    nvgFill(vg);
}

void dc_bar_render(dc_bar *bar)
{
    if (!bar->configured || bar->phys_width <= 0)
        return;

    if (!bar->egl_ready) {
        if (!dc_egl_window_init(&bar->egl_window, bar->egl, bar->surface, bar->phys_width,
                                bar->phys_height))
            return;
        bar->egl_ready = true;
    } else {
        dc_egl_window_resize(&bar->egl_window, bar->phys_width, bar->phys_height);
    }

    if (!dc_egl_make_current(bar->egl, &bar->egl_window))
        return;
    if (!dc_render_ensure(bar->render))
        return;

    /* Map the large (physical) buffer down to the logical surface size. */
    if (bar->viewport)
        wp_viewport_set_destination(bar->viewport, bar->logical_width, bar->logical_height);

    /* Fully transparent clear: the bar is a floating rect, not an
     * edge-to-edge fill, so most of the buffer must stay see-through. */
    glViewport(0, 0, bar->phys_width, bar->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    float pixel_ratio = (float)bar->scale120 / DC_SCALE_BASE;
    nvgBeginFrame(bar->render->vg, bar->logical_width, bar->logical_height, pixel_ratio);
    draw_bar_shadow(bar);
    draw_bar_background(bar);

    bar->hit_count = 0;
    const dc_config *cfg = dc_config_current;
    layout_left(bar, cfg->bar_left_widgets, cfg->bar_left_widgets_n);
    layout_center(bar, cfg->bar_center_widgets, cfg->bar_center_widgets_n);
    layout_right(bar, cfg->bar_right_widgets, cfg->bar_right_widgets_n);

    nvgEndFrame(bar->render->vg);

    dc_egl_swap(bar->egl, &bar->egl_window);
}

/* --- fractional scale --------------------------------------------------- */

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_bar *bar = data;
    DC_UNUSED(fs);
    if ((int)scale == bar->scale120)
        return;
    bar->scale120 = (int)scale;
    dc_debug("bar scale on %s: %.3gx", bar->output->name ? bar->output->name : "?",
             (double)scale / DC_SCALE_BASE);
    recompute_physical(bar);
    dc_bar_render(bar);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

/* --- layer surface ------------------------------------------------------ */

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_bar *bar = data;

    /* Protocol: ack before attaching a buffer. */
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    bar->logical_width = width > 0 ? (int)width : bar->logical_width;
    bar->logical_height = height > 0 ? (int)height : bar->logical_height;
    bar->configured = true;
    recompute_physical(bar);
    recompute_content_rect(bar);
    dc_debug("bar configured: %dx%d logical (buffer %dx%d), rect %.0fx%.0f+%.0f+%.0f on %s",
             bar->logical_width, bar->logical_height, bar->phys_width, bar->phys_height,
             bar->rect_w, bar->rect_h, bar->rect_x, bar->rect_y,
             bar->output->model ? bar->output->model : "?");
    dc_bar_render(bar);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_bar *bar = data;
    DC_UNUSED(surface);
    dc_debug("bar surface closed by compositor");
    bar->configured = false;
    bar->egl_ready = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_bar *dc_bar_create(dc_wayland *wl, dc_output *output, dc_egl *egl, dc_render *render,
                      dc_niri *niri)
{
    const dc_config *cfg = dc_config_current;
    dc_bar_geometry geo = bar_compute_geometry(cfg);

    dc_bar *bar = calloc(1, sizeof(*bar));
    bar->wl = wl;
    bar->output = output;
    bar->egl = egl;
    bar->render = render;
    bar->niri = niri;
    bar->logical_height = geo.window_height > 0 ? geo.window_height : DC_BAR_HEIGHT_FALLBACK;
    bar->scale120 = (output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;

    bar->surface = wl_compositor_create_surface(wl->compositor);

    if (wl->fractional_scale_mgr) {
        bar->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(wl->fractional_scale_mgr,
                                                                bar->surface);
        wp_fractional_scale_v1_add_listener(bar->fractional_scale, &fractional_scale_listener, bar);
    }
    if (wl->viewporter)
        bar->viewport = wp_viewporter_get_viewport(wl->viewporter, bar->surface);

    bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wl->layer_shell, bar->surface, output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        "dankc:bar");

    uint32_t edge_anchor = (cfg->bar_position == DC_BAR_POSITION_TOP)
                               ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                               : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
                                     edge_anchor | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, (uint32_t)bar->logical_height);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar->logical_height);
    zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_surface_listener, bar);

    /* Commit with no buffer to elicit the first configure. */
    wl_surface_commit(bar->surface);

    dc_info("bar created on output %s", output->model ? output->model : "?");
    return bar;
}

struct wl_surface *dc_bar_surface(dc_bar *bar)
{
    return bar->surface;
}

dc_output *dc_bar_output(dc_bar *bar)
{
    return bar->output;
}

void dc_bar_set_tray(dc_bar *bar, struct dc_tray *tray)
{
    bar->tray = tray;
}

dc_bar_region dc_bar_hittest(dc_bar *bar, double x, double y, int *out_payload)
{
    if (out_payload)
        *out_payload = 0;

    /* Reverse order: workspaceSwitcher's per-capsule hits are pushed after
     * (inside) its section, but any widget's rect could in principle overlap
     * a neighbour by a rounding pixel — last-drawn-wins matches what's on
     * top visually. */
    for (int i = bar->hit_count - 1; i >= 0; i--) {
        const dc_bar_hit *h = &bar->hits[i];
        if (x < (double)h->x0 || x > (double)h->x1 || y < (double)h->y0 || y > (double)h->y1)
            continue;
        if (out_payload)
            *out_payload = h->payload;
        return h->region;
    }
    return DC_BAR_REGION_NONE;
}

void dc_bar_destroy(dc_bar *bar)
{
    if (!bar)
        return;
    if (bar->egl_ready)
        dc_egl_window_finish(&bar->egl_window, bar->egl);
    if (bar->viewport)
        wp_viewport_destroy(bar->viewport);
    if (bar->fractional_scale)
        wp_fractional_scale_v1_destroy(bar->fractional_scale);
    if (bar->layer_surface)
        zwlr_layer_surface_v1_destroy(bar->layer_surface);
    if (bar->surface)
        wl_surface_destroy(bar->surface);
    free(bar);
}
