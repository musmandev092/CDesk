#include "ui/bar/bar.h"

#include "core/anim.h"
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
#include "services/notifications.h"
#include "services/sysmon.h"
#include "services/tray.h"
#include "services/weather.h"
#include "theme/theme.h"
#include "ui/bar/bar_tokens.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <ctype.h>
#include <dirent.h>
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

/* Public accessor (bar.h) for popout.c and any panel that needs to sit
 * adjacent to the bar without duplicating bar_compute_geometry()'s formula. */
int dc_bar_window_height(const dc_config *cfg)
{
    return bar_compute_geometry(cfg).window_height;
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

/* Per-channel linear interpolation between two already-converted nanovg
 * colors (docs/12-BAR-SPEC.md sec.4 workspaceSwitcher: the capsule
 * color-crossfade that rides along with the width morph). `t` is clamped to
 * [0,1] so callers can pass raw animation progress unchecked. */
static inline NVGcolor color_lerp(NVGcolor a, NVGcolor b, float t)
{
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    NVGcolor c;
    c.r = a.r + (b.r - a.r) * t;
    c.g = a.g + (b.g - a.g) * t;
    c.b = a.b + (b.b - a.b) * t;
    c.a = a.a + (b.a - a.a) * t;
    return c;
}

/* Theme.outlineButton = withAlpha(outline, 0.5) — clock/focusedWindow bullet
 * separators (docs/12-BAR-SPEC.md sec.4; verified against DMS Theme.qml). */
static inline NVGcolor bar_outline_button(const dc_theme *t)
{
    return tc_alpha(t->outline, 128); /* round(0.5*255) */
}

/* Theme.surfaceTextAlpha = withAlpha(surfaceText, 0.3) — inactive workspace
 * capsules (docs/12-BAR-SPEC.md sec.4; verified against DMS Theme.qml). */
static inline NVGcolor bar_surface_text_alpha(const dc_theme *t)
{
    return tc_alpha(t->surface_text, 77); /* round(0.3*255) */
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

/* Phases of the media-title marquee loop (docs/12-BAR-SPEC.md sec.4 music),
 * matching DMS Media.qml's SequentialAnimation exactly: pause -> scroll out
 * -> pause -> scroll back -> repeat. This is a bounce, not a wrap-around
 * loop, so there is no gap/separator to reproduce. */
typedef enum {
    DC_MARQUEE_PAUSE_START,
    DC_MARQUEE_SCROLL_OUT,
    DC_MARQUEE_PAUSE_END,
    DC_MARQUEE_SCROLL_BACK,
} dc_marquee_phase;

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

    /* System tray (StatusNotifier) + a per-name nanovg image cache. */
    struct dc_tray *tray;
    /* Notification server, for notificationButton's unread dot. */
    struct dc_notifications *notifications;
    struct {
        char name[DC_TRAY_STR];
        int image;
    } tray_cache[DC_TRAY_MAX];
    int tray_cache_n;
    /* IconPixmap fallback cache (docs/POLISH.md P4): one slot per possible
     * tray item, keyed by "service|path" since pixmap data has no stable
     * name to key on like tray_cache above. GC'd every draw_tray_pill() pass
     * against the current dc_tray_items() list so a vanished item's texture
     * doesn't leak. */
    struct {
        bool in_use;
        char key[DC_TRAY_STR * 2 + 2];
        int image; /* 0 = resolved-to-nothing, still cached to skip re-querying every frame */
    } tray_pixmap_cache[DC_TRAY_MAX];

    /* Per-widget click targets from the most recent render (widget host
     * layout pass; docs/12-BAR-SPEC.md sec.5). */
    dc_bar_hit hits[DC_BAR_MAX_HITS];
    int hit_count;

    /* Hover state (docs/12-BAR-SPEC.md sec.3/5): the region+payload under the
     * pointer as of the last hittest, used both to decide whether a motion
     * event is worth a re-render (dc_bar_pointer_motion()) and, during
     * render, to know which hit rect to paint the hover overlay onto. */
    dc_bar_region hover_region;
    int hover_payload;

    /* Workspace capsule morph animation (docs/12-BAR-SPEC.md sec.4/7 S6):
     * tracks this output's active workspace id across renders so a change can
     * kick off a width/color tween, self-driven by frame callbacks (see
     * bar_update_ws_anim(), ws_frame_done()) independent of the 1Hz tick. */
    uint64_t ws_active_id;
    uint64_t ws_prev_active_id;
    bool ws_active_init;
    dc_anim ws_anim;
    struct wl_callback *ws_frame_cb;

    /* Media title/artist marquee (docs/12-BAR-SPEC.md sec.4 music: "2s pause,
     * 60ms/px"), self-driven by frame callbacks exactly like ws_anim above.
     * It isn't a dc_anim because it's an infinite pause/scroll/pause/scroll
     * cycle rather than a finite tween, so it keeps its own phase state
     * machine (bar_update_marquee() in layout_media()). media_marquee_active
     * is recomputed at the top of every dc_bar_render() and only set back to
     * true that same frame if the music widget actually drew an overflowing,
     * playing, non-static label — the instant playback pauses, the text
     * shrinks to fit, or animations are disabled, this drops to false and
     * media_frame_done() stops re-arming, returning the bar to its normal
     * ~1Hz cadence. */
    char media_marquee_label[DC_NIRI_TITLE_MAX]; /* label the phase timer belongs to */
    dc_marquee_phase media_marquee_phase;
    int64_t media_marquee_phase_start_ms;
    bool media_marquee_active;
    struct wl_callback *media_frame_cb;

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

/* Topmost hit rect containing (x,y), or NULL — last-drawn-wins, matching
 * what's visually on top when rects overlap by a rounding pixel
 * (docs/12-BAR-SPEC.md sec.5). Shared by dc_bar_hittest() (click/scroll
 * dispatch) and draw_hover_overlay() (paint the hover highlight at the same
 * rect a click would land on). */
static const dc_bar_hit *bar_find_hit(const dc_bar *bar, double x, double y)
{
    for (int i = bar->hit_count - 1; i >= 0; i--) {
        const dc_bar_hit *h = &bar->hits[i];
        if (x < (double)h->x0 || x > (double)h->x1 || y < (double)h->y0 || y > (double)h->y1)
            continue;
        return h;
    }
    return NULL;
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
    const dc_theme *t = dc_theme_current;
    draw_icon_centered(bar, p, DC_ICON_NOTIFICATIONS, t->surface_text);

    /* Unread dot (docs/12-BAR-SPEC.md sec.4/6): set by the notification
     * server on any new notification, cleared by main.c when the
     * notification center is opened. */
    const bool has_unread = dc_notifications_has_unread(bar->notifications);
    if (has_unread) {
        NVGcontext *vg = bar->render->vg;
        const dc_config *cfg = dc_config_current;
        const float isz = dc_bar_icon_size(cfg, -4);
        const float dot_cx = p->x + p->w / 2.0f + isz / 2.0f - DC_BAR_UNREAD_DOT_RADIUS;
        const float dot_cy = p->cy - isz / 2.0f + DC_BAR_UNREAD_DOT_RADIUS;
        nvgBeginPath(vg);
        nvgCircle(vg, dot_cx, dot_cy, DC_BAR_UNREAD_DOT_RADIUS);
        nvgFillColor(vg, tc(t->error));
        nvgFill(vg);
    }
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
 * past the last capsule.
 *
 * DMS style: every workspace is a capsule filling the pill's full height.
 * The wide/primary capsule tracks this *output's own* active workspace
 * (niri's per-output `is_active`, matching DMS's WorkspaceSwitcher.qml
 * getNiriActiveWorkspace() — NOT the single globally-focused `is_focused`
 * workspace, which may be on a different monitor entirely); everything else
 * (occupied or empty alike, per the user's colorMode config) is narrow +
 * `surfaceTextAlpha` (or `error` if urgent). */
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
    const dc_config *cfg = dc_config_current;
    const float cy = bar_cy(bar);
    /* Capsule height = widgetThickness x 0.5 (docs/12-BAR-SPEC.md sec.7 S4b —
     * WorkspaceSwitcher.qml:1154, showWorkspaceApps=false), vertically
     * centered in the chip; active/inactive *widths* stay thickness-derived
     * (dc_bar_ws_active_width/dc_bar_ws_inactive_width; unchanged). */
    const float h = dc_bar_widget_thickness(cfg) * 0.5f;
    const float y = cy - h / 2.0f;
    const float active_w = dc_bar_ws_active_width(cfg);
    const float inactive_w = dc_bar_ws_inactive_width(cfg);
    /* Morph animation (docs/12-BAR-SPEC.md sec.4/7 S6): progress is shared by
     * whichever capsule is growing (the newly active one) and whichever is
     * shrinking (the previously active one) this frame — see
     * bar_update_ws_anim(). Computed once for both the measure pass
     * (draw=false) and the draw pass so the row's total width never differs
     * between them (it drives place_widget()'s pill width via measure). */
    const bool anim_running = dc_anim_active(&bar->ws_anim);
    const float anim_p = anim_running ? dc_anim_progress(&bar->ws_anim) : 1.0f;
    float x = x0;
    bool any = false;

    for (int i = 0; i < count; i++) {
        const dc_niri_workspace *ws = &workspaces[i];
        if (bar->output->name && ws->output[0] && strcmp(ws->output, bar->output->name) != 0)
            continue;

        float item_w;
        NVGcolor fill;
        if (ws->is_urgent) {
            item_w = ws->is_active ? active_w : inactive_w;
            fill = tc(t->error);
        } else if (anim_running && ws->id == bar->ws_active_id) {
            /* Growing: just became active. */
            item_w = inactive_w + (active_w - inactive_w) * anim_p;
            fill = color_lerp(bar_surface_text_alpha(t), tc(t->primary), anim_p);
        } else if (anim_running && ws->id == bar->ws_prev_active_id) {
            /* Shrinking: was active a moment ago. */
            item_w = active_w + (inactive_w - active_w) * anim_p;
            fill = color_lerp(tc(t->primary), bar_surface_text_alpha(t), anim_p);
        } else if (ws->is_active) {
            item_w = active_w;
            fill = tc(t->primary);
        } else {
            item_w = inactive_w;
            fill = bar_surface_text_alpha(t);
        }

        if (draw) {
            float radius = dc_bar_clamp_radius(DC_BAR_CORNER_RADIUS, item_w, h);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, item_w, h, radius);
            nvgFillColor(vg, fill);
            nvgFill(vg);
            bar_push_hit(bar, x, x + item_w, DC_BAR_REGION_WORKSPACE, ws->idx);
        }
        x += item_w + DC_BAR_WS_SPACING;
        any = true;
    }
    if (any)
        x -= DC_BAR_WS_SPACING; /* no trailing gap past the last capsule */
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

/* Decode one UTF-8 codepoint at `s`, reporting the byte length consumed via
 * `*out_len`. On a malformed sequence (stray continuation byte, truncated
 * multi-byte lead), returns the sentinel BAR_UTF8_INVALID with `*out_len =
 * 1` so the caller resyncs one byte at a time, matching the previous
 * hand-inlined decoder's behavior. Shared by bar_sanitize_utf8() and
 * bar_text_is_meaningless() below. */
#define BAR_UTF8_INVALID 0xFFFFFFFFu

static unsigned bar_next_utf8(const unsigned char *s, int *out_len)
{
    unsigned char c = *s;
    int len;
    unsigned cp;

    if ((c & 0x80) == 0) {
        len = 1;
        cp = c;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        *out_len = 1;
        return BAR_UTF8_INVALID;
    }

    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *out_len = 1;
            return BAR_UTF8_INVALID;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *out_len = len;
    return cp;
}

/* True for codepoints that are *invisible* text machinery — dropped silently
 * (no "…" collapse marker; they never had visible width): C0/C1 control
 * chars, zero-width space/joiners (U+200B-U+200D; emoji ZWJ sequences thus
 * decompose into their component emoji, the best a shaping-less renderer can
 * do), BOM/ZWNBSP (U+FEFF), and variation selectors (U+FE00-U+FE0F,
 * U+E0100-U+E01EF — e.g. the VS16 riding on U+2764 "❤️", whose base heart
 * renders fine on its own from the monochrome emoji fallback). */
static bool bar_cp_invisible(unsigned cp)
{
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) ||
           (cp >= 0x200B && cp <= 0x200D) || cp == 0xFEFF || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

/* Strip codepoints no loaded font can render before building any display
 * string (docs/12-BAR-SPEC.md sec.8: focusedWindow + music titles need "no
 * more tofu"). Coverage (dc_render_font_has()) is the union of the UI font
 * and every fallback nvg.c loaded — CJK, Devanagari, Thai, Cyrillic/Greek,
 * Arabic, and the vendored monochrome NotoEmoji — over the FULL Unicode
 * range, so supplementary-plane emoji now render (monochrome outlines; the
 * old blanket >= U+10000 and private-use bans are gone, replaced by real
 * coverage checks). A run of one or more consecutive dropped codepoints
 * collapses into a single "…" (U+2026) instead of vanishing silently or
 * leaving disconnected fragments; invisible formatting codepoints (see
 * bar_cp_invisible()) are removed without leaving a marker.
 *
 * Builds into a bounded local buffer first, then copies into `out` — an
 * ellipsis can make a dropped run GROW instead of shrink, which would break
 * the byte-for-byte-shrinking in-place aliasing the previous version relied
 * on. `in`/`out` may still alias (all call sites pass the same buffer for
 * both); the copy into `out` only happens after `in` has been fully read
 * into `tmp`, so that's safe regardless. */
static void bar_sanitize_utf8(const char *in, char *out, size_t out_sz)
{
    char tmp[DC_NIRI_TITLE_MAX];
    size_t cap = out_sz < sizeof(tmp) ? out_sz : sizeof(tmp);
    if (cap == 0)
        return;

    const unsigned char *s = (const unsigned char *)in;
    size_t oi = 0;
    bool dropping = false; /* mid a run of collapsed-to-"…" codepoints */

    while (*s) {
        int len;
        unsigned cp = bar_next_utf8(s, &len);
        if (cp == BAR_UTF8_INVALID || bar_cp_invisible(cp)) {
            s += len;
            continue;
        }

        bool banned = !dc_render_font_has(cp);
        if (banned) {
            if (!dropping && oi + 3 < cap) {
                memcpy(tmp + oi, "\xe2\x80\xa6", 3);
                oi += 3;
            }
            dropping = true;
        } else {
            if (oi + (size_t)len < cap) {
                memcpy(tmp + oi, s, (size_t)len);
                oi += (size_t)len;
            }
            dropping = false;
        }
        s += len;
    }
    tmp[oi] = '\0';
    memcpy(out, tmp, oi + 1);
}

/* True if `s` (already run through bar_sanitize_utf8()) has nothing worth
 * showing on its own — empty, or only whitespace/punctuation/the "…"
 * collapse marker. Used to fall back to just the app name (no bullet) when a
 * title sanitizes down to nothing useful, e.g. a title that was entirely
 * uncovered-script text collapses to just "…" (docs/12-BAR-SPEC.md sec.4/8:
 * "no more tofu"). */
static bool bar_text_is_meaningless(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int len;
        unsigned cp = bar_next_utf8(p, &len);
        if (cp == BAR_UTF8_INVALID) {
            p += len;
            continue;
        }
        if (cp == 0x2026 || (cp < 0x80 && (isspace((int)cp) || ispunct((int)cp)))) {
            p += len;
            continue;
        }
        return false;
    }
    return true;
}

/* Truncate a NUL-terminated UTF-8 buffer in place to at most `max_bytes`
 * bytes, backing up to the last full codepoint boundary. Used to bound
 * layout_media()'s title/artist before they're concatenated into a
 * fixed-size label (docs/12-BAR-SPEC.md sec.4/8) — both the truncation
 * itself and the snprintf() precision below are needed: this makes it UTF-8
 * safe, the literal `%.100s` precision is what lets the compiler prove the
 * concatenation can't overflow (declared-array-size analysis alone can't see
 * a runtime truncation), avoiding -Wformat-truncation. */
static void bar_truncate_bytes(char *s, size_t max_bytes)
{
    size_t len = strlen(s);
    if (len <= max_bytes)
        return;
    size_t cut = max_bytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
        cut--;
    s[cut] = '\0';
}

/* Truncate `buf` (a DC_NIRI_TITLE_MAX-sized title buffer) in place on a
 * UTF-8 codepoint boundary and append a real ellipsis ("…", U+2026) until it
 * fits within `max_w` px at the vg context's current font
 * (docs/12-BAR-SPEC.md sec.4 focusedWindow: "true right-ellipsis instead of
 * clipping"). No-op if it already fits. `tmp` is sized identically to `buf`
 * (not just bufsize+headroom) so the compiler can prove the final copy never
 * truncates, rather than tripping -Wformat-truncation. */
static void bar_ellipsize(NVGcontext *vg, char buf[DC_NIRI_TITLE_MAX], float max_w)
{
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, buf, NULL, bounds);
    if (bounds[2] - bounds[0] <= max_w)
        return;

    size_t len = strlen(buf);
    if (len > DC_NIRI_TITLE_MAX - 4)
        len = DC_NIRI_TITLE_MAX - 4; /* leave room for the 3-byte ellipsis + NUL */

    char tmp[DC_NIRI_TITLE_MAX];
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80)
            len--; /* don't split a multi-byte UTF-8 codepoint */
        snprintf(tmp, sizeof(tmp), "%.*s\xe2\x80\xa6", (int)len, buf);
        nvgTextBounds(vg, 0.0f, 0.0f, tmp, NULL, bounds);
        if (bounds[2] - bounds[0] <= max_w || len == 0) {
            memcpy(buf, tmp, sizeof(tmp));
            return;
        }
    }
    snprintf(buf, DC_NIRI_TITLE_MAX, "\xe2\x80\xa6");
}

/* "AppName • Title", text-only (no app icon in the horizontal bar — DMS's
 * FocusedApp.qml only shows one in the vertical orientation), elided to a max
 * content width of 456px (docs/12-BAR-SPEC.md sec.4). Shared measure/draw,
 * like layout_workspaces() above. Returns 0 (the "absent this frame" sentinel
 * — see find_widget()'s callers) when there is no focused window on this
 * output. */
static float layout_focused_window(dc_bar *bar, float x0, bool draw)
{
    const dc_niri_window *win = dc_niri_focused_window(bar->niri);
    if (!win || !focused_window_on_output(bar, win))
        return 0.0f;

    /* Pretty app name from the app_id (last dotted component, capitalised). */
    char app_name[64] = {0};
    if (win->app_id[0]) {
        const char *base = strrchr(win->app_id, '.');
        base = base ? base + 1 : win->app_id;
        snprintf(app_name, sizeof(app_name), "%s", base);
        app_name[0] = (char)toupper((unsigned char)app_name[0]);
    }
    bar_sanitize_utf8(app_name, app_name, sizeof(app_name));

    /* Strip a trailing " - AppName" / " \xe2\x80\x94 AppName" (em dash) window
     * title suffix so the app name isn't shown twice (docs/12-BAR-SPEC.md
     * sec.4, matching DMS FocusedApp.qml's title.endsWith(appName) trim). */
    char title[DC_NIRI_TITLE_MAX];
    snprintf(title, sizeof(title), "%s", win->title);
    bar_sanitize_utf8(title, title, sizeof(title));
    if (app_name[0] && title[0]) {
        char suffix[96];
        size_t tl = strlen(title), sl;

        snprintf(suffix, sizeof(suffix), " - %s", app_name);
        sl = strlen(suffix);
        if (tl > sl && strcmp(title + tl - sl, suffix) == 0) {
            title[tl - sl] = '\0';
        } else {
            snprintf(suffix, sizeof(suffix), " \xe2\x80\x94 %s", app_name);
            sl = strlen(suffix);
            if (tl > sl && strcmp(title + tl - sl, suffix) == 0)
                title[tl - sl] = '\0';
        }
    }

    /* A title that sanitized down to nothing renderable (e.g. entirely
     * uncovered-script text collapsed to just "…", or pure punctuation)
     * falls back to app-name-only, no bullet (docs/12-BAR-SPEC.md sec.4/8:
     * "no more tofu"). */
    if (bar_text_is_meaningless(title))
        title[0] = '\0';

    if (!app_name[0] && !title[0])
        return 0.0f;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const float cy = bar_cy(bar);
    const bool have_both = app_name[0] && title[0];
    const float max_total = 456.0f;

    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);

    float bounds[4];
    float app_w = 0.0f, sep_w = 0.0f;
    if (app_name[0]) {
        nvgTextBounds(vg, 0.0f, 0.0f, app_name, NULL, bounds);
        app_w = bounds[2] - bounds[0];
    }
    if (have_both) {
        nvgTextBounds(vg, 0.0f, 0.0f, "\xe2\x80\xa2", NULL, bounds);
        sep_w = bounds[2] - bounds[0];
    }

    float used = app_w + (have_both ? 2.0f * DC_BAR_ROW_GAP + sep_w : 0.0f);
    float title_avail = max_total - used;
    if (title_avail < 20.0f)
        title_avail = 20.0f;
    if (title[0])
        bar_ellipsize(vg, title, title_avail);

    float title_w = 0.0f;
    if (title[0]) {
        nvgTextBounds(vg, 0.0f, 0.0f, title, NULL, bounds);
        title_w = bounds[2] - bounds[0];
    }

    float total = app_w + (have_both ? 2.0f * DC_BAR_ROW_GAP + sep_w + title_w : title_w);
    if (total > max_total)
        total = max_total;

    if (draw) {
        float x = x0;
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        if (app_name[0]) {
            nvgFillColor(vg, tc(t->surface_text));
            nvgText(vg, x, cy, app_name, NULL);
            x += app_w;
        }
        if (have_both) {
            x += DC_BAR_ROW_GAP;
            nvgFillColor(vg, bar_outline_button(t));
            nvgText(vg, x, cy, "\xe2\x80\xa2", NULL);
            x += sep_w + DC_BAR_ROW_GAP;
        }
        if (title[0]) {
            nvgFillColor(vg, tc(t->surface_text));
            nvgText(vg, x, cy, title, NULL);
        }
    }
    return total;
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

/* Draw (or just measure, if `vg` is NULL) one HH/MM digit in its own
 * fixed-width cell (docs/12-BAR-SPEC.md sec.4: "fixed-width digit cells...
 * so width doesn't jitter"). Returns the cell width. */
static float clock_digit_cell(NVGcontext *vg, float x, float cy, char digit, NVGcolor color)
{
    float cell = roundf(DC_BAR_TEXT_SIZE * DC_BAR_CLOCK_DIGIT_FACTOR);
    if (vg) {
        char s[2] = {digit, '\0'};
        nvgFillColor(vg, color);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, x + cell / 2.0f, cy, s, NULL);
    }
    return cell;
}

/* "HH:MM • Www D" (honours use24HourClock/showDate), each time digit in a
 * fixed-width cell, "•" in outlineButton, DC_BAR_ROW_GAP between the time
 * block and the date (docs/12-BAR-SPEC.md sec.4). Shared measure/draw. */
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
    const char *time_fmt = cfg->clock_24h ? (cfg->show_seconds ? "%H:%M:%S" : "%H:%M")
                                           : (cfg->show_seconds ? "%-I:%M:%S %p" : "%-I:%M %p");
    strftime(time_str, sizeof(time_str), time_fmt, &tm);
    strftime(date_str, sizeof(date_str), "%a %-d", &tm); /* e.g. "Wed 1" */

    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    /* Time block: digits in fixed-width cells, ':'/' '/"AM"/"PM" at their
     * natural width (only the digits jitter frame to frame). */
    float x = x0;
    float bounds[4];
    for (const char *c = time_str; *c; c++) {
        if (*c >= '0' && *c <= '9') {
            x += clock_digit_cell(draw ? vg : NULL, x, cy, *c, tc(t->surface_text));
        } else {
            char s[2] = {*c, '\0'};
            nvgTextBounds(vg, 0.0f, 0.0f, s, NULL, bounds);
            if (draw) {
                nvgFillColor(vg, tc(t->surface_text));
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                nvgText(vg, x, cy, s, NULL);
            }
            x += bounds[2] - bounds[0];
        }
    }
    const float time_w = x - x0;

    float date_w = 0.0f;
    if (cfg->show_date) {
        nvgTextBounds(vg, 0.0f, 0.0f, "\xe2\x80\xa2", NULL, bounds);
        const float sep_w = bounds[2] - bounds[0];
        nvgTextBounds(vg, 0.0f, 0.0f, date_str, NULL, bounds);
        date_w = bounds[2] - bounds[0];

        if (draw) {
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, bar_outline_button(t));
            nvgText(vg, x + DC_BAR_ROW_GAP, cy, "\xe2\x80\xa2", NULL);
            nvgFillColor(vg, tc(t->surface_text));
            nvgText(vg, x + DC_BAR_ROW_GAP + sep_w + DC_BAR_ROW_GAP, cy, date_str, NULL);
        }
        date_w += 2.0f * DC_BAR_ROW_GAP + sep_w;
    }

    return time_w + date_w;
}

static float measure_clock(dc_bar *bar)
{
    return layout_clock(bar, 0.0f, false);
}

static void draw_clock_pill(dc_bar *bar, const dc_pill *p)
{
    layout_clock(bar, p->content_x0, true);
}

/* --- music (media) --------------------------------------------------------- */

/* DMS Media.qml's SequentialAnimation constants (verified against
 * quickshell/Modules/DankBar/Widgets/Media.qml): 2s pause at each end, then a
 * linear scroll at 60ms per overflow pixel (minimum 1s so short overflows
 * don't whip past). */
#define DC_MARQUEE_PAUSE_MS 2000
#define DC_MARQUEE_MS_PER_PX 60.0f
#define DC_MARQUEE_MIN_SCROLL_MS 1000

/* Reset the marquee phase machine to a clean "not scrolling" state — called
 * whenever the music widget draws a static (non-animating) label this frame,
 * so playback resuming (or the title changing back to something that
 * overflows) always restarts from a fresh pause instead of resuming mid-cycle
 * against a stale wall-clock timestamp. */
static void bar_reset_marquee(dc_bar *bar)
{
    bar->media_marquee_phase = DC_MARQUEE_PAUSE_START;
    bar->media_marquee_phase_start_ms = 0;
    bar->media_marquee_label[0] = '\0';
}

/* Advance (and return the current px offset of) the marquee's infinite
 * pause/scroll-out/pause/scroll-back cycle. Only called from the draw pass
 * when overflow+playing+animations-enabled all hold this frame — the caller
 * is responsible for setting bar->media_marquee_active so dc_bar_render()
 * knows to keep re-arming the frame callback (docs/12-BAR-SPEC.md sec.4
 * music / sec.7 S6). */
static float bar_update_marquee(dc_bar *bar, const char *label, float overflow_px)
{
    if (overflow_px < 0.0f)
        overflow_px = 0.0f;

    int64_t now = dc_anim_now_ms();
    if (strcmp(bar->media_marquee_label, label) != 0) {
        /* New/changed track: DMS's mediaText.onTextChanged restarts the whole
         * SequentialAnimation from the top. */
        snprintf(bar->media_marquee_label, sizeof(bar->media_marquee_label), "%s", label);
        bar->media_marquee_phase = DC_MARQUEE_PAUSE_START;
        bar->media_marquee_phase_start_ms = now;
    }

    int scroll_ms = (int)fmaxf(DC_MARQUEE_MIN_SCROLL_MS, overflow_px * DC_MARQUEE_MS_PER_PX);
    int64_t elapsed = now - bar->media_marquee_phase_start_ms;
    float offset = 0.0f;

    switch (bar->media_marquee_phase) {
    case DC_MARQUEE_PAUSE_START:
        offset = 0.0f;
        if (elapsed >= DC_MARQUEE_PAUSE_MS) {
            bar->media_marquee_phase = DC_MARQUEE_SCROLL_OUT;
            bar->media_marquee_phase_start_ms = now;
        }
        break;
    case DC_MARQUEE_SCROLL_OUT:
        if (elapsed >= scroll_ms) {
            bar->media_marquee_phase = DC_MARQUEE_PAUSE_END;
            bar->media_marquee_phase_start_ms = now;
            offset = overflow_px;
        } else {
            offset = ((float)elapsed / (float)scroll_ms) * overflow_px;
        }
        break;
    case DC_MARQUEE_PAUSE_END:
        offset = overflow_px;
        if (elapsed >= DC_MARQUEE_PAUSE_MS) {
            bar->media_marquee_phase = DC_MARQUEE_SCROLL_BACK;
            bar->media_marquee_phase_start_ms = now;
        }
        break;
    case DC_MARQUEE_SCROLL_BACK:
        if (elapsed >= scroll_ms) {
            bar->media_marquee_phase = DC_MARQUEE_PAUSE_START;
            bar->media_marquee_phase_start_ms = now;
            offset = 0.0f;
        } else {
            offset = overflow_px * (1.0f - (float)elapsed / (float)scroll_ms);
        }
        break;
    }

    return offset;
}

/* 20x20 music_note (primary) + "Title • Artist" (elided to ~200px, or
 * marquee-scrolled while playing and overflowing — docs/12-BAR-SPEC.md
 * sec.4 music) + a prev/play-pause/next transport, matching DMS's Media.qml
 * horizontal layout. Hidden entirely (returns 0) when no MPRIS player is
 * present. Shared measure/draw, like the widgets above; pushes the three
 * transport hit rects itself when drawing (custom_hit — the whole pill is
 * not one click target here, docs/12-BAR-SPEC.md sec.5). */
static float layout_media(dc_bar *bar, float x0, bool draw)
{
    dc_mpris_info info;
    if (!dc_mpris_read(&info) || !info.active)
        return 0.0f;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    const float cy = bar_cy(bar);
    const float icon_sz = 20.0f;
    const float btn_sm = 20.0f;
    const float btn_play = 24.0f;
    const float icon_sm = 12.0f;
    const float icon_play = 14.0f;
    const float gap = DC_BAR_WIDGET_SPACING;
    const float text_max = 200.0f;
    const bool playing = info.playing;

    char title[DC_NIRI_TITLE_MAX];
    char artist[DC_NIRI_TITLE_MAX];
    bar_sanitize_utf8(info.title, title, sizeof(title));
    bar_sanitize_utf8(info.artist, artist, sizeof(artist));
    /* Same "sanitized down to nothing" fallback as focusedWindow (see
     * bar_text_is_meaningless()): a title that collapsed to just "…" reads
     * as "Unknown Track" instead, and a meaningless artist is dropped so the
     * label doesn't end in a dangling "• …". */
    if (bar_text_is_meaningless(title))
        title[0] = '\0';
    if (bar_text_is_meaningless(artist))
        artist[0] = '\0';
    bar_truncate_bytes(title, 100);
    bar_truncate_bytes(artist, 100);
    if (!title[0])
        snprintf(title, sizeof(title), "Unknown Track");

    char label[DC_NIRI_TITLE_MAX];
    if (artist[0])
        snprintf(label, sizeof(label), "%.100s \xe2\x80\xa2 %.100s", title, artist);
    else
        snprintf(label, sizeof(label), "%.100s", title);

    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
    float raw_w = bounds[2] - bounds[0];

    /* DMS: needsScrolling = implicitWidth > textContainer.width — the
     * marquee only ever engages on genuine overflow, never on text that
     * already fits inside the box. Scrolling is further gated on `playing`
     * (DMS itself scrolls even while paused, as long as a track is loaded;
     * this C port intentionally narrows that to "playing" so a paused/idle
     * overflowing title can't hold the bar off its ~1Hz idle cadence
     * forever) and on animationsEnabled, matching every other frame-callback
     * animation in this file. */
    bool overflow = raw_w > text_max;
    bool marquee_wanted = overflow && playing && cfg->animations_enabled;

    float label_w;
    float marquee_offset = 0.0f;
    if (marquee_wanted) {
        label_w = text_max;
        if (draw) {
            marquee_offset = bar_update_marquee(bar, label, raw_w - text_max + 5.0f);
            bar->media_marquee_active = true;
        }
    } else {
        if (draw)
            bar_reset_marquee(bar);
        if (overflow)
            bar_ellipsize(vg, label, text_max);
        nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
        label_w = bounds[2] - bounds[0];
    }

    float x = x0;

    if (draw)
        dc_render_icon(bar->render, DC_ICON_MUSIC_NOTE, x + icon_sz / 2.0f, cy, icon_sz, t->primary,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    x += icon_sz + gap;

    if (draw) {
        /* dc_render_icon() just switched the shared vg context to the icons
         * font/size — restore the UI font before drawing the label text. */
        nvgFontFaceId(vg, bar->render->font_ui);
        nvgFontSize(vg, DC_BAR_TEXT_SIZE);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_text));
        if (marquee_wanted) {
            /* Full (non-ellipsized) label shifted left by the current
             * scroll offset, clipped to the reserved text_max box — same
             * nvgSave/nvgScissor/nvgRestore idiom as toasts.c's summary
             * line. */
            nvgSave(vg);
            nvgScissor(vg, x, cy - DC_BAR_TEXT_SIZE, label_w, DC_BAR_TEXT_SIZE * 2.0f);
            nvgText(vg, x - marquee_offset, cy, label, NULL);
            nvgRestore(vg);
        } else {
            nvgText(vg, x, cy, label, NULL);
        }
    }
    x += label_w + gap;

    /* prev/next: transparent circles for now — hover fill lands with the S6
     * hover pass (docs/12-BAR-SPEC.md sec.4/5). */
    float prev_x0 = x;
    if (draw)
        dc_render_icon(bar->render, DC_ICON_SKIP_PREVIOUS, x + btn_sm / 2.0f, cy, icon_sm,
                       t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    x += btn_sm + gap;

    /* play/pause: filled circle — bg=primary/icon=background while playing,
     * bg=primaryHover(primary @ 12%)/icon=primary while paused. */
    float play_x0 = x;
    if (draw) {
        nvgBeginPath(vg);
        nvgCircle(vg, x + btn_play / 2.0f, cy, btn_play / 2.0f);
        nvgFillColor(vg, playing ? tc(t->primary) : tc_alpha(t->primary, 31));
        nvgFill(vg);
        dc_render_icon(bar->render, playing ? DC_ICON_PAUSE : DC_ICON_PLAY_ARROW,
                       x + btn_play / 2.0f, cy, icon_play, playing ? t->background : t->primary,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    }
    x += btn_play + gap;

    float next_x0 = x;
    if (draw)
        dc_render_icon(bar->render, DC_ICON_SKIP_NEXT, x + btn_sm / 2.0f, cy, icon_sm,
                       t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    x += btn_sm;

    if (draw) {
        /* Body (icon + title/artist, left of the transport) opens the Media tab
         * of the dashboard (docs/13-POPOUTS-SPEC.md sec.5); pushed first so the
         * three transport rects below win where they overlap (last-drawn-wins,
         * see bar_find_hit()). */
        bar_push_hit(bar, x0, prev_x0, DC_BAR_REGION_MEDIA_BODY, 0);
        bar_push_hit(bar, prev_x0, prev_x0 + btn_sm, DC_BAR_REGION_MEDIA_PREV, 0);
        bar_push_hit(bar, play_x0, play_x0 + btn_play, DC_BAR_REGION_MEDIA_PLAY, 0);
        bar_push_hit(bar, next_x0, next_x0 + btn_sm, DC_BAR_REGION_MEDIA_NEXT, 0);
    }

    return x - x0;
}

static float measure_media(dc_bar *bar)
{
    return layout_media(bar, 0.0f, false);
}

static void draw_media_pill(dc_bar *bar, const dc_pill *p)
{
    layout_media(bar, p->content_x0, true);
}

/* --- weather --------------------------------------------------------------- */

/* Map dc_weather_icon_name()'s returned name to a Material Symbols codepoint
 * (docs/12-BAR-SPEC.md sec.4/6 weather). */
static int weather_icon_codepoint(const char *name)
{
    if (strcmp(name, "clear_day") == 0)
        return DC_ICON_CLEAR_DAY;
    if (strcmp(name, "clear_night") == 0)
        return DC_ICON_CLEAR_NIGHT;
    if (strcmp(name, "partly_cloudy_day") == 0)
        return DC_ICON_PARTLY_CLOUDY_DAY;
    if (strcmp(name, "partly_cloudy_night") == 0)
        return DC_ICON_PARTLY_CLOUDY_NIGHT;
    if (strcmp(name, "foggy") == 0)
        return DC_ICON_FOGGY;
    if (strcmp(name, "rainy") == 0)
        return DC_ICON_RAINY;
    if (strcmp(name, "weather_snowy") == 0)
        return DC_ICON_WEATHER_SNOWY;
    if (strcmp(name, "thunderstorm") == 0)
        return DC_ICON_THUNDERSTORM;
    return DC_ICON_CLOUD; /* "cloud" and any unmapped code */
}

/* Weather glyph (barIconSize(-6), 15px) + "NN°C"/"NN°F" (docs/12-BAR-SPEC.md
 * sec.4 weather). Hidden (returns 0) until dc_weather_init() has a
 * last-known-good reading — including while disabled (weatherEnabled=false
 * never calls dc_weather_init(), so dc_weather_get() just returns false). */
static float layout_weather(dc_bar *bar, float x0, bool draw)
{
    dc_weather_state w;
    if (!dc_weather_get(&w) || !w.valid)
        return 0.0f;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    const float cy = bar_cy(bar);
    const float isz = dc_bar_icon_size(cfg, -6);
    const float gap = DC_BAR_WIDGET_SPACING;
    const int codepoint = weather_icon_codepoint(dc_weather_icon_name(w.weather_code, w.is_day));

    float x = x0;
    if (draw)
        dc_render_icon(bar->render, codepoint, x + isz / 2.0f, cy, isz, t->surface_text,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    x += isz + gap;

    char label[16];
    snprintf(label, sizeof(label), "%d\xc2\xb0%s", w.temp_c, cfg->weather_fahrenheit ? "F" : "C");
    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
    float label_w = bounds[2] - bounds[0];

    if (draw) {
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, x, cy, label, NULL);
    }
    x += label_w;
    return x - x0;
}

static float measure_weather(dc_bar *bar)
{
    return layout_weather(bar, 0.0f, false);
}

static void draw_weather_pill(dc_bar *bar, const dc_pill *p)
{
    layout_weather(bar, p->content_x0, true);
}

/* --- cpuUsage / memUsage ---------------------------------------------------- */

/* Icon (barIconSize(-6), 15px) + "NN%" (docs/12-BAR-SPEC.md sec.4), matching
 * DMS's CpuMonitor.qml / RamMonitor.qml: icon glyph + color thresholds, then
 * the percentage text. Always visible (no MPRIS/weather-style hide
 * condition) — dc_sysmon_poll() self-limits to a 3s cadence, driven from
 * main.c's 1 Hz tick. Shared by both widgets via `icon`/`percent`/
 * `warn_at`/`danger_at`, mirroring layout_battery()'s icon+label shape. */
static float layout_sysmon(dc_bar *bar, float x0, bool draw, int icon, int percent, int warn_at,
                           int danger_at)
{
    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    const float cy = bar_cy(bar);
    const float isz = dc_bar_icon_size(cfg, -6);
    const float icon_text_gap = 2.0f;

    dc_color icon_color =
        percent > danger_at ? t->error : (percent > warn_at ? t->warning : t->surface_text);

    float x = x0;
    if (draw)
        dc_render_icon(bar->render, icon, x, cy, isz, icon_color, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    x += isz + icon_text_gap;

    char label[8];
    snprintf(label, sizeof(label), "%d%%", percent);
    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
    float text_w = bounds[2] - bounds[0];

    if (draw) {
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, x, cy, label, NULL);
    }
    x += text_w;
    return x - x0;
}

static float measure_cpu(dc_bar *bar)
{
    return layout_sysmon(bar, 0.0f, false, DC_ICON_MEMORY, dc_sysmon_cpu_percent(), 60, 80);
}

static void draw_cpu_pill(dc_bar *bar, const dc_pill *p)
{
    layout_sysmon(bar, p->content_x0, true, DC_ICON_MEMORY, dc_sysmon_cpu_percent(), 60, 80);
}

static float measure_mem(dc_bar *bar)
{
    return layout_sysmon(bar, 0.0f, false, DC_ICON_DEVELOPER_BOARD, dc_sysmon_mem_percent(), 75, 90);
}

static void draw_mem_pill(dc_bar *bar, const dc_pill *p)
{
    layout_sysmon(bar, p->content_x0, true, DC_ICON_DEVELOPER_BOARD, dc_sysmon_mem_percent(), 75, 90);
}

/* --- battery ------------------------------------------------------------ */

/* Any Mains/USB power-supply reporting online=1 (docs/12-BAR-SPEC.md sec.4/6
 * battery item 5). battery.h's `charging` is a strict sysfs status=="Charging"
 * check, but plenty of laptops report "Not charging" once a charge threshold
 * is hit while AC stays connected — DMS's BatteryService shows the green
 * charging glyph for as long as AC is plugged in, not just mid-charge.
 * battery.{c,h} is out of scope for this stage (see AGENTS.md constraints),
 * so this reads the sibling "online" sysfs attribute directly, mirroring
 * battery.c's own read_line() pattern. Best-effort: false if no supply is
 * found (never treated as fatal — the bar just falls back to bat.charging). */
static bool bar_ac_online(void)
{
    DIR *dir = opendir("/sys/class/power_supply");
    if (!dir)
        return false;

    bool online = false;
    struct dirent *entry;
    char path[512], value[32];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        bool got_type = fgets(value, sizeof(value), f) != NULL;
        fclose(f);
        if (!got_type)
            continue;
        value[strcspn(value, "\n")] = '\0';
        if (strcmp(value, "Mains") != 0 && strcmp(value, "USB") != 0)
            continue;

        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", entry->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        bool got_online = fgets(value, sizeof(value), f) != NULL;
        fclose(f);
        if (got_online && value[0] == '1')
            online = true;
    }
    closedir(dir);
    return online;
}

/* Pick the Material Symbols battery glyph for this level/charging state
 * (docs/12-BAR-SPEC.md sec.4 + sec.6). DMS's BatteryService.getBatteryIcon()
 * has a numbered battery_1_bar..battery_6_bar glyph per ~15% band, but the
 * bundled font (assets/fonts/MaterialSymbolsRounded.ttf, a build-time
 * subset) doesn't include those codepoints — verified by parsing its cmap,
 * they render as blank space, not tofu. Collapsed to the three tiers that
 * are actually present in the font: full, low (0_bar), and alert. */
static int battery_icon_codepoint(bool charging, int percent)
{
    if (charging)
        return DC_ICON_BATTERY_CHARGING_FULL;
    if (percent <= 10)
        return DC_ICON_BATTERY_ALERT;
    if (percent <= 25)
        return DC_ICON_BATTERY_0_BAR;
    return DC_ICON_BATTERY_FULL;
}

/* Material Symbols glyph + "NN%", left-to-right (docs/12-BAR-SPEC.md sec.4 —
 * replaces the hand-drawn pictograph). Shared measure/draw; returns 0
 * (absent) when there is no battery. */
static float layout_battery(dc_bar *bar, float x0, bool draw)
{
    dc_battery_info bat;
    if (!dc_battery_read(&bat) || !bat.present)
        return 0.0f;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    const float cy = bar_cy(bar);
    const bool charging = bat.charging || bar_ac_online();
    const bool low = bat.percent < 20 && !charging;

    dc_color icon_color = charging ? t->primary : (low ? t->error : t->surface_text);
    const int codepoint = battery_icon_codepoint(charging, bat.percent);
    const float isz = dc_bar_icon_size(cfg, -4);
    const float icon_text_gap = 2.0f;
    float x = x0;

    if (draw)
        dc_render_icon(bar->render, codepoint, x, cy, isz, icon_color,
                       NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    x += isz + icon_text_gap;

    char label[8];
    snprintf(label, sizeof(label), "%d%%", bat.percent);
    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
    float text_w = bounds[2] - bounds[0];

    if (draw) {
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_text));
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

/* Resolve+load item `index`'s IconPixmap property (docs/POLISH.md P4),
 * cached per "service|path" since pixmap-only items have no name to key on.
 * Called only when the named-icon lookup above found nothing. */
static int get_tray_pixmap_image(dc_bar *bar, int index, const char *service, const char *path)
{
    char key[DC_TRAY_STR * 2 + 2];
    snprintf(key, sizeof(key), "%s|%s", service, path);

    for (int i = 0; i < DC_TRAY_MAX; i++)
        if (bar->tray_pixmap_cache[i].in_use && strcmp(bar->tray_pixmap_cache[i].key, key) == 0)
            return bar->tray_pixmap_cache[i].image;

    int img = 0;
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (dc_tray_icon_pixmap(bar->tray, index, 48, &rgba, &w, &h) && rgba) {
        img = nvgCreateImageRGBA(bar->render->vg, w, h, 0, rgba);
        free(rgba);
    }

    int slot = -1;
    for (int i = 0; i < DC_TRAY_MAX; i++)
        if (!bar->tray_pixmap_cache[i].in_use) {
            slot = i;
            break;
        }
    if (slot < 0)
        slot = 0; /* shouldn't happen: at most DC_TRAY_MAX items exist */
    bar->tray_pixmap_cache[slot].in_use = true;
    snprintf(bar->tray_pixmap_cache[slot].key, sizeof(bar->tray_pixmap_cache[slot].key), "%s", key);
    bar->tray_pixmap_cache[slot].image = img;
    return img;
}

/* Evict + nvgDeleteImage() every IconPixmap cache slot whose item is no
 * longer in the tray (docs/POLISH.md P4: "free nvg images ... when items
 * vanish"). Must run with the bar's own EGL context current -- called from
 * inside draw_tray_pill()'s render pass, same as every other nvg* call
 * here. */
static void gc_tray_pixmap_cache(dc_bar *bar)
{
    const dc_tray_item *items[DC_TRAY_MAX];
    int n = bar->tray ? dc_tray_items(bar->tray, items, DC_TRAY_MAX) : 0;

    for (int i = 0; i < DC_TRAY_MAX; i++) {
        if (!bar->tray_pixmap_cache[i].in_use)
            continue;
        bool found = false;
        for (int j = 0; j < n && !found; j++) {
            char key[DC_TRAY_STR * 2 + 2];
            snprintf(key, sizeof(key), "%s|%s", items[j]->service, items[j]->path);
            if (strcmp(key, bar->tray_pixmap_cache[i].key) == 0)
                found = true;
        }
        if (!found) {
            if (bar->tray_pixmap_cache[i].image > 0)
                nvgDeleteImage(bar->render->vg, bar->tray_pixmap_cache[i].image);
            memset(&bar->tray_pixmap_cache[i], 0, sizeof(bar->tray_pixmap_cache[i]));
        }
    }
}

/* Per-item chip: 21x21 (DC_BAR_TRAY_CHIP), transparent idle bg (hover fill —
 * radius-clamped to a circle via dc_bar_clamp_radius() — lands with the S6
 * hover pass), icon at barIconSize(-6) (named icon, else IconPixmap, else a
 * letter fallback -- docs/12-BAR-SPEC.md sec.4 systemTray). Pushes each
 * item's own hit rect (DC_BAR_REGION_TRAY, payload = item index); main.c
 * routes left/middle/right clicks there to Activate/SecondaryActivate/the
 * dbusmenu popup (docs/POLISH.md P4). */
static float measure_tray(dc_bar *bar)
{
    if (!bar->tray)
        return 0.0f;
    const dc_tray_item *items[DC_TRAY_MAX];
    int n = dc_tray_items(bar->tray, items, DC_TRAY_MAX);
    if (n <= 0)
        return 0.0f;
    return (float)n * DC_BAR_TRAY_CHIP + (float)(n - 1) * DC_BAR_TRAY_GAP;
}

static void draw_tray_pill(dc_bar *bar, const dc_pill *p)
{
    if (!bar->tray)
        return;
    const dc_tray_item *items[DC_TRAY_MAX];
    int n = dc_tray_items(bar->tray, items, DC_TRAY_MAX);
    NVGcontext *vg = bar->render->vg;
    const dc_config *cfg = dc_config_current;
    const dc_theme *t = dc_theme_current;
    const float icon_sz = dc_bar_icon_size(cfg, -6);
    float x = p->content_x0;

    for (int i = 0; i < n; i++) {
        float cx = x + DC_BAR_TRAY_CHIP / 2.0f;
        int img = items[i]->icon_name[0] ? get_tray_image(bar, items[i]->icon_name) : 0;
        if (img <= 0)
            img = get_tray_pixmap_image(bar, i, items[i]->service, items[i]->path);
        if (img > 0) {
            NVGpaint pat = nvgImagePattern(vg, cx - icon_sz / 2.0f, p->cy - icon_sz / 2.0f,
                                           icon_sz, icon_sz, 0.0f, img, 1.0f);
            nvgBeginPath(vg);
            nvgRect(vg, cx - icon_sz / 2.0f, p->cy - icon_sz / 2.0f, icon_sz, icon_sz);
            nvgFillPaint(vg, pat);
            nvgFill(vg);
        } else {
            /* Letter fallback for pixmap-only / unresolved items. */
            const char *name = items[i]->title[0] ? items[i]->title : items[i]->icon_name;
            char letter[2] = {name[0] ? (char)toupper((unsigned char)name[0]) : '?', '\0'};
            nvgFontFaceId(vg, bar->render->font_ui);
            nvgFontSize(vg, DC_BAR_TRAY_LETTER_SIZE);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, tc(t->surface_text));
            nvgText(vg, cx, p->cy, letter, NULL);
        }
        bar_push_hit(bar, x, x + DC_BAR_TRAY_CHIP, DC_BAR_REGION_TRAY, i);
        x += DC_BAR_TRAY_CHIP + DC_BAR_TRAY_GAP;
    }

    gc_tray_pixmap_cache(bar);
}

/* --- controlCenterButton --------------------------------------------------- */

/* DMS's compound pill: network/bluetooth/audio sub-icons (the user's config
 * doesn't enable vpn/screenSharing/etc — docs/12-BAR-SPEC.md sec.0/4), each
 * `primary` when active/connected else `surfaceText`, 17px, DC_BAR_WIDGET_SPACING
 * (4px) apart. REPLACES dankc's old standalone wifi/bluetooth/volume icons and
 * the dead cellular icon — this is the only place those services are read for
 * bar display now. One hit region for the whole pill (already routed to
 * DC_BAR_REGION_CONTROL_CENTER by the table below). */
static float measure_cc(dc_bar *bar)
{
    DC_UNUSED(bar);
    float isz = dc_bar_icon_size(dc_config_current, -4);
    return isz * 3.0f + DC_BAR_WIDGET_SPACING * 2.0f;
}

static void draw_cc_pill(dc_bar *bar, const dc_pill *p)
{
    const dc_config *cfg = dc_config_current;
    const dc_theme *t = dc_theme_current;
    const float isz = dc_bar_icon_size(cfg, -4);
    const int align = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE;
    float x = p->content_x0;

    /* network — wifi glyph, primary when connected (dc_net_wifi). */
    dc_net_info net;
    dc_net_wifi(&net);
    dc_render_icon(bar->render, DC_ICON_WIFI, x, p->cy, isz,
                  net.connected ? t->primary : t->surface_text, align);
    x += isz + DC_BAR_WIDGET_SPACING;

    /* bluetooth — bluetooth_connected + primary when a device is connected,
     * plain bluetooth + surfaceText otherwise (dc_bluez_read). */
    dc_bluez_info bt;
    bool have_bt = dc_bluez_read(&bt);
    bool bt_connected = have_bt && bt.connected;
    dc_render_icon(bar->render, bt_connected ? DC_ICON_BLUETOOTH_CONNECTED : DC_ICON_BLUETOOTH, x,
                  p->cy, isz, bt_connected ? t->primary : t->surface_text, align);
    x += isz + DC_BAR_WIDGET_SPACING;

    /* audio — volume_up/down/off by level/mute (dc_audio_read), surfaceText. */
    dc_audio_info audio;
    bool have_audio = dc_audio_read(&audio);
    int volume_icon = DC_ICON_VOLUME_UP;
    if (have_audio && audio.muted)
        volume_icon = DC_ICON_VOLUME_OFF;
    else if (have_audio && audio.volume < 34)
        volume_icon = DC_ICON_VOLUME_DOWN;
    dc_render_icon(bar->render, volume_icon, x, p->cy, isz, t->surface_text, align);
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

/* Table of widget ids this stage implements (docs/12-BAR-SPEC.md sec.7
 * S4b adds music/weather/cpuUsage/memUsage + the real controlCenterButton;
 * any remaining unknown id is skipped silently by find_widget() returning
 * NULL). */
static const dc_bar_widget_def *find_widget(const char *id)
{
    static const dc_bar_widget_def table[] = {
        {"launcherButton", measure_launcher, draw_launcher_pill, true, false,
         DC_BAR_REGION_LAUNCHER},
        {"workspaceSwitcher", measure_workspaces, draw_workspaces_pill, true, true,
         DC_BAR_REGION_NONE},
        {"focusedWindow", measure_focused_window, draw_focused_window_pill, true, false,
         DC_BAR_REGION_NONE},
        {"music", measure_media, draw_media_pill, true, true, DC_BAR_REGION_NONE},
        {"clock", measure_clock, draw_clock_pill, true, false, DC_BAR_REGION_DASHBOARD},
        {"weather", measure_weather, draw_weather_pill, true, false, DC_BAR_REGION_WEATHER},
        {"systemTray", measure_tray, draw_tray_pill, false, true, DC_BAR_REGION_NONE},
        {"clipboard", measure_clipboard, draw_clipboard_pill, true, false,
         DC_BAR_REGION_CLIPBOARD},
        {"cpuUsage", measure_cpu, draw_cpu_pill, true, false, DC_BAR_REGION_CPU},
        {"memUsage", measure_mem, draw_mem_pill, true, false, DC_BAR_REGION_MEM},
        {"notificationButton", measure_notif, draw_notif_pill, true, false,
         DC_BAR_REGION_NOTIFICATIONS},
        /* battery -> its own popout (docs/13-POPOUTS-SPEC.md sec.2). */
        {"battery", measure_battery, draw_battery_pill, true, false, DC_BAR_REGION_BATTERY},
        {"controlCenterButton", measure_cc, draw_cc_pill, true, false, DC_BAR_REGION_CONTROL_CENTER},
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

/* --- workspace capsule morph animation ------------------------------------ */

/* Detect an active-workspace change on this bar's output and, if one just
 * happened, kick off the width/color morph (docs/12-BAR-SPEC.md sec.4/7 S6).
 * Called once per render, before layout_workspaces() reads bar->ws_anim /
 * ws_active_id / ws_prev_active_id. The first observation after bar creation
 * just primes ws_active_id — no morph plays for the initial state. */
static void bar_update_ws_anim(dc_bar *bar)
{
    if (!bar->niri)
        return;

    int count = 0;
    const dc_niri_workspace *workspaces = dc_niri_workspaces(bar->niri, &count);
    uint64_t active_id = 0;
    for (int i = 0; i < count; i++) {
        const dc_niri_workspace *ws = &workspaces[i];
        if (bar->output->name && ws->output[0] && strcmp(ws->output, bar->output->name) != 0)
            continue;
        if (ws->is_active) {
            active_id = ws->id;
            break;
        }
    }
    if (active_id == 0)
        return; /* no active workspace on this output this frame */

    if (!bar->ws_active_init) {
        bar->ws_active_id = active_id;
        bar->ws_active_init = true;
        return;
    }
    if (active_id != bar->ws_active_id) {
        bar->ws_prev_active_id = bar->ws_active_id;
        bar->ws_active_id = active_id;
        dc_anim_start(&bar->ws_anim, DC_DUR_MEDIUM, DC_EASE_EMPHASIZED);
    }
}

/* Frame callback that keeps the workspace morph animating independently of
 * the bar's normal 1Hz clock-driven redraw (docs/12-BAR-SPEC.md sec.7 S6) —
 * same self-terminating pattern as launcher.c/controlcenter.c's entrance
 * animations: re-arm only while dc_anim_active(), so an idle bar costs 0
 * extra frames. */
static void ws_frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener ws_frame_listener = {.done = ws_frame_done};

static void ws_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_bar *bar = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    bar->ws_frame_cb = NULL;
    if (dc_anim_active(&bar->ws_anim))
        dc_bar_render(bar);
}

/* Same self-terminating pattern as ws_frame_done() above, driving the media
 * marquee instead of the workspace morph: re-fires only while the most
 * recent render actually animated the marquee (bar->media_marquee_active,
 * recomputed fresh every dc_bar_render() call), so it stops re-arming the
 * instant playback pauses, the label stops overflowing, or animations get
 * disabled — never a free-running timer. */
static void media_frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener media_frame_listener = {.done = media_frame_done};

static void media_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_bar *bar = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    bar->media_frame_cb = NULL;
    if (bar->media_marquee_active)
        dc_bar_render(bar);
}

/* --- hover overlay --------------------------------------------------------- */

/* Visual height (px) of the shape at a given hit region — hit rects
 * themselves are padded to the full bar height for a forgiving click target
 * (bar_push_hit()), so this is the only place that needs each region's
 * actual on-screen size (docs/12-BAR-SPEC.md sec.3/4). Anything not listed
 * (including DC_BAR_REGION_NONE) falls back to the standard BasePill height —
 * harmless, since draw_hover_overlay() skips NONE before this is used. */
static float bar_hover_height(const dc_config *cfg, dc_bar_region region)
{
    switch (region) {
    case DC_BAR_REGION_WORKSPACE:
        return dc_bar_widget_thickness(cfg) * 0.5f;
    case DC_BAR_REGION_MEDIA_PREV:
    case DC_BAR_REGION_MEDIA_NEXT:
        return 20.0f;
    case DC_BAR_REGION_MEDIA_PLAY:
        return 24.0f;
    case DC_BAR_REGION_TRAY:
        return DC_BAR_TRAY_CHIP;
    default:
        return dc_bar_widget_thickness(cfg);
    }
}

/* Hover bg (docs/12-BAR-SPEC.md sec.3): withAlpha(blend(surfaceContainerHigh,
 * primary, 0.10), max(0.30, widgetTransparency)), painted on top of whichever
 * hit rect the pointer is currently over — sized to that region's own visual
 * shape (bar_hover_height()), not the taller click target, so full pills get
 * a stadium and circular sub-regions (media transport, tray chips) get a
 * circle via the same stadium/circle radius clamp normal drawing uses.
 * Non-interactive widgets (focusedWindow, weather) push hits with region
 * DC_BAR_REGION_NONE, so they never reach here. cpuUsage/memUsage became
 * clickable (-> the Processes popout) when that popout was added, so they no
 * longer fall in that bucket. */
static void draw_hover_overlay(dc_bar *bar)
{
    if (!bar->wl->pointer || bar->wl->pointer_surface != bar->surface)
        return;

    const dc_bar_hit *hit = bar_find_hit(bar, bar->wl->pointer_x, bar->wl->pointer_y);
    if (!hit || hit->region == DC_BAR_REGION_NONE)
        return;

    const dc_config *cfg = dc_config_current;
    const dc_theme *t = dc_theme_current;
    float h = bar_hover_height(cfg, hit->region);
    float w = hit->x1 - hit->x0;
    float y = bar_cy(bar) - h / 2.0f;
    float radius = dc_bar_clamp_radius(h / 2.0f, w, h);

    NVGcolor blended = color_lerp(tc(t->surface_container_high), tc(t->primary), 0.10f);
    float alpha = fmaxf(0.30f, cfg->bar_widget_transparency);

    NVGcontext *vg = bar->render->vg;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, hit->x0, y, w, h, radius);
    nvgFillColor(vg, nvgRGBA((unsigned char)(blended.r * 255.0f), (unsigned char)(blended.g * 255.0f),
                            (unsigned char)(blended.b * 255.0f), (unsigned char)(alpha * 255.0f)));
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
    /* dc_render_ensure() guarantees a live vg on success, but assert it at the
     * call site too: dc_bar_render() is reached from Wayland listener
     * callbacks (layer-surface configure, fractional-scale, and the S6
     * workspace-anim frame callback ws_frame_done, all tail-calling here), any
     * of which can fire during startup before the shared render context is
     * up. Entering nanovg with a NULL context is the crash we are guarding
     * against (nvgBeginFrame -> nvgSave, NULL NVGcontext). */
    if (!dc_render_ensure(bar->render) || !bar->render->vg)
        return;

    /* Map the large (physical) buffer down to the logical surface size. */
    if (bar->viewport)
        wp_viewport_set_destination(bar->viewport, bar->logical_width, bar->logical_height);

    /* Fully transparent clear: the bar is a floating rect, not an
     * edge-to-edge fill, so most of the buffer must stay see-through. */
    glViewport(0, 0, bar->phys_width, bar->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    bar_update_ws_anim(bar);

    float pixel_ratio = (float)bar->scale120 / DC_SCALE_BASE;
    nvgBeginFrame(bar->render->vg, bar->logical_width, bar->logical_height, pixel_ratio);
    draw_bar_shadow(bar);
    draw_bar_background(bar);

    bar->hit_count = 0;
    /* Recomputed by layout_media() below (draw pass only) if the music
     * widget is present, playing, overflowing, and animating this frame —
     * defaulting to false here means a widget that's absent this frame (or
     * that just went static) can never leave a stale frame-callback loop
     * running (docs/12-BAR-SPEC.md sec.4 music / sec.7 S6). */
    bar->media_marquee_active = false;
    const dc_config *cfg = dc_config_current;
    layout_left(bar, cfg->bar_left_widgets, cfg->bar_left_widgets_n);
    layout_center(bar, cfg->bar_center_widgets, cfg->bar_center_widgets_n);
    layout_right(bar, cfg->bar_right_widgets, cfg->bar_right_widgets_n);

    draw_hover_overlay(bar);

    nvgEndFrame(bar->render->vg);

    /* Re-arm only while the morph animation is still running (idle = 0 extra
     * frames), matching launcher.c/controlcenter.c's entrance-animation
     * pattern (docs/12-BAR-SPEC.md sec.7 S6). */
    if (dc_anim_active(&bar->ws_anim) && !bar->ws_frame_cb) {
        bar->ws_frame_cb = wl_surface_frame(bar->surface);
        wl_callback_add_listener(bar->ws_frame_cb, &ws_frame_listener, bar);
    }
    /* Same idea for the media marquee (docs/12-BAR-SPEC.md sec.4 music):
     * re-arm only while this frame actually animated it. */
    if (bar->media_marquee_active && !bar->media_frame_cb) {
        bar->media_frame_cb = wl_surface_frame(bar->surface);
        wl_callback_add_listener(bar->media_frame_cb, &media_frame_listener, bar);
    }

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
    /* Cancel the self-arming S6 workspace-anim frame callback: it must not
     * outlive the surface it was requested on. Without this, a pending
     * ws_frame_done would fire against a closed surface (and dc_bar_render
     * would re-arm a new frame callback on the next configure while the stale
     * one is still in flight). dc_anim's own state is left as-is — the next
     * configure's render re-arms cleanly if the morph is still running. */
    if (bar->ws_frame_cb) {
        wl_callback_destroy(bar->ws_frame_cb);
        bar->ws_frame_cb = NULL;
    }
    /* Same reasoning for the media-marquee frame callback. */
    if (bar->media_frame_cb) {
        wl_callback_destroy(bar->media_frame_cb);
        bar->media_frame_cb = NULL;
    }
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

void dc_bar_reconfigure(dc_bar *bar)
{
    if (!bar->layer_surface)
        return;
    const dc_config *cfg = dc_config_current;
    dc_bar_geometry geo = bar_compute_geometry(cfg);
    bar->logical_height = geo.window_height > 0 ? geo.window_height : DC_BAR_HEIGHT_FALLBACK;

    uint32_t edge_anchor = (cfg->bar_position == DC_BAR_POSITION_TOP)
                               ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                               : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
                                     edge_anchor | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, (uint32_t)bar->logical_height);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar->logical_height);
    wl_surface_commit(bar->surface);

    /* If the height is unchanged the compositor won't send a new configure, so
     * refresh the content rect + repaint directly; if it did change, the
     * configure ack will do it again harmlessly. */
    recompute_physical(bar);
    recompute_content_rect(bar);
    dc_bar_render(bar);
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

void dc_bar_set_notifications(dc_bar *bar, struct dc_notifications *notifications)
{
    bar->notifications = notifications;
}

dc_bar_region dc_bar_hittest(dc_bar *bar, double x, double y, int *out_payload)
{
    if (out_payload)
        *out_payload = 0;
    const dc_bar_hit *hit = bar_find_hit(bar, x, y);
    if (!hit)
        return DC_BAR_REGION_NONE;
    if (out_payload)
        *out_payload = hit->payload;
    return hit->region;
}

void dc_bar_pointer_motion(dc_bar *bar, double x, double y)
{
    int payload = 0;
    dc_bar_region region = dc_bar_hittest(bar, x, y, &payload);
    if (region == bar->hover_region && payload == bar->hover_payload)
        return; /* still the same region — nothing to repaint (docs/12-BAR-SPEC.md sec.5) */

    bar->hover_region = region;
    bar->hover_payload = payload;
    dc_wayland_set_cursor(bar->wl,
                          region != DC_BAR_REGION_NONE ? DC_CURSOR_POINTER : DC_CURSOR_DEFAULT);
    dc_bar_render(bar);
}

void dc_bar_pointer_leave(dc_bar *bar)
{
    if (bar->hover_region == DC_BAR_REGION_NONE)
        return; /* nothing was hovered — no repaint needed */

    bar->hover_region = DC_BAR_REGION_NONE;
    bar->hover_payload = 0;
    dc_wayland_set_cursor(bar->wl, DC_CURSOR_DEFAULT);
    dc_bar_render(bar);
}

void dc_bar_destroy(dc_bar *bar)
{
    if (!bar)
        return;
    if (bar->ws_frame_cb)
        wl_callback_destroy(bar->ws_frame_cb);
    if (bar->media_frame_cb)
        wl_callback_destroy(bar->media_frame_cb);
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
