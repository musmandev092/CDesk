#include "ui/processes.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/sysmon.h"
#include "theme/theme.h"
#include "ui/connected.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ~480x560 logical, matching the user's live DMS reference screenshots
 * (~/Pictures/Screenshots/Screenshot from 2026-07-02 18-1[78]-*.png) and the
 * bar-adjacent popout convention every other panel here follows
 * (controlcenter.c's DC_CC_WIDTH, notifcenter.c's DC_NC_WIDTH, etc). */
#define DC_PS_WIDTH 480
#define DC_PS_HEIGHT 560
#define DC_SCALE_BASE 120
#define DC_PS_SIDE_MARGIN 12

#define DC_PS_PAD 6.0f    /* outer gutter for the drop shadow */
#define DC_PS_RADIUS 14.0f
#define DC_PS_INSET 16.0f /* left/right content inset inside the card */

/* Logical surface width. DC_PS_WIDTH already bakes in the floating chrome's
 * flat 6px pad on every side; connected_frame widens the lateral (side) pad
 * to 12 for the connector fillets (dc_popout_chrome_pads()), so the surface
 * needs 2*(pad_side-6) more logical px to keep the card CONTENT rect --
 * inset by pad_side + DC_PS_INSET, see ps_get_layout() -- exactly where it
 * sits when floating (mirrors controlcenter.c's cc_surface_width()).
 * connected_frame off: pad_side==6, so this is just DC_PS_WIDTH, unchanged. */
static int ps_surface_width(void)
{
    int pad_side = 6;
    dc_popout_chrome_pads(dc_config_current, NULL, &pad_side, NULL);
    return DC_PS_WIDTH + 2 * (pad_side - 6);
}

#define DC_PS_HEADER_TOP 14.0f
#define DC_PS_HEADER_H 32.0f
#define DC_PS_INFO_GAP 14.0f
#define DC_PS_INFO_H 64.0f
#define DC_PS_THEAD_GAP 14.0f
#define DC_PS_THEAD_H 24.0f
#define DC_PS_LIST_GAP 6.0f
#define DC_PS_BOTTOM_PAD 14.0f

#define DC_PS_ROW_H 24.0f
#define DC_PS_ROW_GAP 3.0f

#define DC_PS_QUERY_MAX 128
#define DC_PS_SCROLL_STEP 48.0f

/* All/User/System filter tabs (docs/13-POPOUTS-SPEC.md Processes popout;
 * matches ProcessListPopout.qml's processFilter). */
typedef enum {
    DC_PSF_ALL = 0,
    DC_PSF_USER = 1,
    DC_PSF_SYSTEM = 2,
} dc_ps_filter;

/* Shared layout so ps_render (draw) and the click/scroll handlers agree --
 * same convention as controlcenter.c's cc_layout / clip_picker.c's cp_layout. */
typedef struct {
    float pad_side, ix, iw;
    float header_y, header_h;
    float info_y, info_h;
    float thead_y, thead_h;
    float list_y0, list_y1, list_h;
} ps_layout;

static ps_layout ps_get_layout(float w, float h)
{
    /* Card-fill padding (docs/27-CONNECTED-FRAME-PLAN.md T5): floating
     * chrome reserves a flat 6px of shadow room on all four sides;
     * connected chrome widens the lateral (side) pad to 12 for the
     * connector fillets and drops the bar-facing (near) pad to 0, leaving
     * the far pad at 6 -- see dc_popout_chrome_pads(). Which physical edge
     * is "near" vs "far" swaps with bar_position. */
    int pad_near, pad_side, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, &pad_side, &pad_far);
    const bool bottom_bar = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    const float pad_top = bottom_bar ? (float)pad_far : (float)pad_near;
    const float pad_bottom = bottom_bar ? (float)pad_near : (float)pad_far;

    ps_layout l;
    l.pad_side = (float)pad_side;
    l.ix = l.pad_side + DC_PS_INSET;
    l.iw = w - 2.0f * l.ix;
    l.header_y = pad_top + DC_PS_HEADER_TOP;
    l.header_h = DC_PS_HEADER_H;
    l.info_y = l.header_y + l.header_h + DC_PS_INFO_GAP;
    l.info_h = DC_PS_INFO_H;
    l.thead_y = l.info_y + l.info_h + DC_PS_THEAD_GAP;
    l.thead_h = DC_PS_THEAD_H;
    l.list_y0 = l.thead_y + l.thead_h + DC_PS_LIST_GAP;
    l.list_y1 = h - pad_bottom - DC_PS_BOTTOM_PAD;
    l.list_h = l.list_y1 - l.list_y0;
    return l;
}

/* The table's five columns (Name | CPU | Memory | PID | chevron), shared by
 * the header row and every data row so they always line up. Sized to sum
 * exactly to `iw` (docs/13-POPOUTS-SPEC.md Processes popout reference). */
typedef struct {
    float name_x0, name_x1;
    float cpu_x0, cpu_x1;
    float mem_x0, mem_x1;
    float pid_x0, pid_x1;
    float chevron_cx;
} ps_cols;

static ps_cols ps_get_cols(float ix, float iw)
{
    const float gap = 8.0f;
    const float cpu_w = 64.0f, mem_w = 78.0f, pid_w = 46.0f, chevron_w = 24.0f;

    ps_cols c;
    float x = ix;
    c.name_x0 = x;
    c.name_x1 = ix + iw - (cpu_w + mem_w + pid_w + chevron_w + 4.0f * gap);
    x = c.name_x1 + gap;
    c.cpu_x0 = x;
    c.cpu_x1 = x + cpu_w;
    x = c.cpu_x1 + gap;
    c.mem_x0 = x;
    c.mem_x1 = x + mem_w;
    x = c.mem_x1 + gap;
    c.pid_x0 = x;
    c.pid_x1 = x + pid_w;
    x = c.pid_x1 + gap;
    c.chevron_cx = x + chevron_w / 2.0f;
    return c;
}

struct dc_processes {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width, logical_height, scale120, phys_width, phys_height;

    dc_ps_filter filter;
    dc_processes_sort sort;
    char query[DC_PS_QUERY_MAX];

    dc_sysmon_proc all[DC_SYSMON_PROC_MAX];  /* raw top-N-by-CPU scan */
    int all_count;
    dc_sysmon_proc view[DC_SYSMON_PROC_MAX]; /* filter+search+sort applied */
    int view_count;

    float scroll, scroll_max;

    /* Header/tab/search/sort-header hit-test rects, recomputed every render. */
    float tab_x0[3], tab_y0[3], tab_x1[3], tab_y1[3];
    float cpu_hdr_x0, cpu_hdr_y0, cpu_hdr_x1, cpu_hdr_y1;
    float mem_hdr_x0, mem_hdr_y0, mem_hdr_x1, mem_hdr_y1;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;
    /* Entrance/exit scale-and-fade pivot, bar-position-aware -- see
     * controlcenter.c's identical field for the full rationale. */
    float anim_ox, anim_oy;

    bool visible, configured, egl_ready;
};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

static void ps_render(dc_processes *ps);
static void ps_teardown(dc_processes *ps);

static void ps_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_processes *ps = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    ps->frame_cb = NULL;
    if (!ps->visible)
        return;
    if (dc_anim_active(&ps->anim))
        ps_render(ps);
    else if (ps->closing)
        ps_teardown(ps);
}
static const struct wl_callback_listener ps_frame_listener = {.done = ps_frame_done};

static void recompute_physical(dc_processes *ps)
{
    ps->phys_width = (ps->logical_width * ps->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    ps->phys_height = (ps->logical_height * ps->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Case-insensitive substring test (same as clip_picker.c's contains_ci). */
static bool contains_ci(const char *hay, const char *needle)
{
    if (!needle[0])
        return true;
    size_t nl = strlen(needle);
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

static int cmp_view_cpu_desc(const void *a, const void *b)
{
    const dc_sysmon_proc *pa = a, *pb = b;
    if (pa->cpu_percent > pb->cpu_percent)
        return -1;
    if (pa->cpu_percent < pb->cpu_percent)
        return 1;
    return pa->pid - pb->pid;
}

static int cmp_view_mem_desc(const void *a, const void *b)
{
    const dc_sysmon_proc *pa = a, *pb = b;
    if (pa->mem_kb > pb->mem_kb)
        return -1;
    if (pa->mem_kb < pb->mem_kb)
        return 1;
    return pa->pid - pb->pid;
}

/* Re-pull the latest scan from sysmon.h and apply the filter/search/sort.
 * Deliberately does NOT touch ps->scroll -- called both from user-initiated
 * changes (tab/sort click, search edit, which reset scroll themselves right
 * after) and from the periodic 2s refresh (dc_processes_refresh()), where
 * resetting scroll would yank the list back to the top out from under a
 * scrolled-down user. */
static void ps_rebuild_view(dc_processes *ps)
{
    ps->all_count = dc_sysmon_processes(ps->all, DC_SYSMON_PROC_MAX);

    char q[DC_PS_QUERY_MAX];
    size_t qi = 0;
    for (; ps->query[qi] && qi + 1 < sizeof(q); qi++)
        q[qi] = (char)tolower((unsigned char)ps->query[qi]);
    q[qi] = '\0';

    uid_t self = getuid();
    ps->view_count = 0;
    for (int i = 0; i < ps->all_count && ps->view_count < DC_SYSMON_PROC_MAX; i++) {
        const dc_sysmon_proc *p = &ps->all[i];
        if (ps->filter == DC_PSF_USER && p->uid != self)
            continue;
        if (ps->filter == DC_PSF_SYSTEM && p->uid == self)
            continue;
        if (q[0] && !contains_ci(p->comm, q))
            continue;
        ps->view[ps->view_count++] = *p;
    }

    qsort(ps->view, (size_t)ps->view_count, sizeof(ps->view[0]),
          ps->sort == DC_PROCESSES_SORT_CPU ? cmp_view_cpu_desc : cmp_view_mem_desc);
}

/* "N.N MB"/"N.N GB", matching ProcessListPopout.qml's statsContainer.compactMem()
 * (used for the memory ring's big label + swap detail line). */
static void compact_mem_kb(unsigned long kb, char *out, size_t outsz)
{
    if (kb < 1024UL * 1024UL) {
        double mb = (double)kb / 1024.0;
        if (mb >= 100.0)
            snprintf(out, outsz, "%.0f MB", mb);
        else
            snprintf(out, outsz, "%.1f MB", mb);
    } else {
        double gb = (double)kb / (1024.0 * 1024.0);
        if (gb >= 10.0)
            snprintf(out, outsz, "%.0f GB", gb);
        else
            snprintf(out, outsz, "%.1f GB", gb);
    }
}

/* Per-process memory chip text, matching ProcessesView.qml's
 * DgopService.formatMemoryUsage() shape ("0 KB" for tiny/zero RSS, otherwise
 * compactMem()'s MB/GB). */
static void format_row_mem(unsigned long kb, char *out, size_t outsz)
{
    if (kb < 1024UL)
        snprintf(out, outsz, "%lu KB", kb);
    else
        compact_mem_kb(kb, out, outsz);
}

/* "up 1d 6h 17m" (docs/13-POPOUTS-SPEC.md reference screenshot). Extends
 * controlcenter.c's get_user_info() subtitle formatting with a days component
 * -- that helper caps at hours since the control center card never needs
 * more, but the Processes popout's reference clearly shows day-scale uptime. */
static void format_uptime(char *out, size_t outsz)
{
    double up = -1.0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1)
            up = -1.0;
        fclose(f);
    }
    if (up < 0.0) {
        snprintf(out, outsz, "up --");
        return;
    }
    long total_min = (long)(up / 60.0);
    long days = total_min / 1440;
    long hours = (total_min % 1440) / 60;
    long mins = total_min % 60;
    if (days > 0)
        snprintf(out, outsz, "up %ldd %ldh %ldm", days, hours, mins);
    else if (hours > 0)
        snprintf(out, outsz, "up %ldh %ldm", hours, mins);
    else
        snprintf(out, outsz, "up %ldm", mins);
}

/* /etc/os-release's NAME= value (unquoted), e.g. "Arch Linux". Falls back to
 * "Linux" if the file or field is missing. */
static void get_distro_name(char *out, size_t outsz)
{
    snprintf(out, outsz, "Linux");
    FILE *f = fopen("/etc/os-release", "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "NAME=", 5) != 0)
            continue;
        char *v = line + 5;
        size_t n = strlen(v);
        while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r'))
            v[--n] = '\0';
        if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
            v[n - 1] = '\0';
            v++;
        }
        /* Copied by hand (not snprintf's "%s") so gcc can see the copy is
         * bounded regardless of NAME='s worst-case length (up to `line`'s
         * 256 bytes) vs. the caller's much smaller `out` buffer --
         * snprintf("%s", v) is equally safe at runtime, but -Wformat-
         * truncation can't prove that statically. */
        size_t vlen = strlen(v);
        if (vlen >= outsz)
            vlen = outsz - 1;
        memcpy(out, v, vlen);
        out[vlen] = '\0';
        break;
    }
    fclose(f);
}

/* Draw a filter tab pill and record its hit rect -- same shape as
 * notifcenter.c's draw_tab(), minus the "(N)" count suffix. */
static void draw_tab(dc_processes *ps, int index, float x, float y, float h, const char *label,
                     bool active, float *out_x1)
{
    NVGcontext *vg = ps->render->vg;
    const dc_theme *t = dc_theme_current;

    nvgFontFaceId(vg, ps->render->font_ui);
    nvgFontSize(vg, 12.0f);
    float b[4];
    nvgTextBounds(vg, 0, 0, label, NULL, b);
    float w = (b[2] - b[0]) + 22.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, h / 2.0f);
    nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_container_high));
    nvgFill(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, active ? tc(t->primary_text) : tc_alpha(t->surface_text, 180));
    nvgText(vg, x + w / 2.0f, y + h / 2.0f, label, NULL);

    ps->tab_x0[index] = x;
    ps->tab_y0[index] = y;
    ps->tab_x1[index] = x + w;
    ps->tab_y1[index] = y + h;
    *out_x1 = x + w;
}

/* CPU/Memory circular gauge (docs/13-POPOUTS-SPEC.md Processes popout info
 * row): a dim full-circle track, a solid progress arc from the top going
 * clockwise, and up to three centered text lines (label/sublabel/detail),
 * mirroring ProcessListPopout.qml's inline CircleGauge component. */
static void draw_ring(dc_render *r, float cx, float cy, float radius, float value01,
                      dc_color accent, const char *label, const char *sublabel,
                      const char *detail)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    const float thickness = 5.0f;

    if (value01 < 0.0f)
        value01 = 0.0f;
    if (value01 > 1.0f)
        value01 = 1.0f;

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, radius);
    nvgStrokeColor(vg, tc_alpha(t->outline, 60));
    nvgStrokeWidth(vg, thickness);
    nvgStroke(vg);

    if (value01 > 0.004f) {
        float start = -(float)M_PI / 2.0f;
        float end = start + value01 * 2.0f * (float)M_PI;
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, radius, start, end, NVG_CW);
        nvgLineCap(vg, NVG_ROUND);
        nvgStrokeColor(vg, tc(accent));
        nvgStrokeWidth(vg, thickness);
        nvgStroke(vg);
        nvgLineCap(vg, NVG_BUTT); /* restore the default cap for later strokes */
    }

    bool have_detail = detail && detail[0];
    float ly = have_detail ? cy - 10.0f : cy - 5.0f;

    nvgFontFaceId(vg, r->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    /* Auto-shrink the label to fit inside the ring: at a fixed 13px it's
     * fine for a short "3%" (CPU) but a memory value like "3.6 GB" measures
     * wider than the ring's own diameter and overflows past the track/arc
     * stroke on both sides (reproduced live: the "B" in "3.6 GB" rendered
     * outside the circle). Step the font size down until it fits the
     * available chord width, floor at 9px (still legible; a value that
     * doesn't fit even there is an extreme edge case not worth clipping
     * further for). */
    float max_label_w = 2.0f * radius - thickness * 2.0f - 4.0f;
    float label_size = 13.0f;
    nvgFontSize(vg, label_size);
    float b[4];
    nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, b);
    while (b[2] - b[0] > max_label_w && label_size > 9.0f) {
        label_size -= 1.0f;
        nvgFontSize(vg, label_size);
        nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, b);
    }
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, cx, ly, label, NULL);

    nvgFontSize(vg, 9.0f);
    nvgFillColor(vg, tc_alpha(t->surface_text, 170));
    nvgText(vg, cx, ly + 13.0f, sublabel, NULL);

    if (have_detail) {
        nvgFontSize(vg, 8.0f);
        nvgFillColor(vg, tc_alpha(t->surface_text, 130));
        nvgText(vg, cx, ly + 24.0f, detail, NULL);
    }
}

/* Truncate on a UTF-8 boundary + ellipsis, matching controlcenter.c's
 * cc_ellipsize()/notifcenter.c's nc_ellipsize() (duplicated locally; no
 * shared string-util module yet). */
static void ps_ellipsize(NVGcontext *vg, char *buf, size_t bufsize, float max_w)
{
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, buf, NULL, bounds);
    if (bounds[2] - bounds[0] <= max_w)
        return;
    size_t len = strlen(buf);
    if (bufsize < 4)
        return;
    char tmp[64];
    size_t cap = bufsize > sizeof(tmp) ? sizeof(tmp) : bufsize;
    if (len > cap - 4)
        len = cap - 4;
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80)
            len--;
        snprintf(tmp, cap, "%.*s\xe2\x80\xa6", (int)len, buf);
        nvgTextBounds(vg, 0.0f, 0.0f, tmp, NULL, bounds);
        if (bounds[2] - bounds[0] <= max_w || len == 0) {
            memcpy(buf, tmp, cap);
            return;
        }
    }
}

/* A CPU%/memory value chip: rounded pill, neutral/warning/error tint by
 * threshold, matching ProcessesView.qml's ProcessItem exactly (cpu >80%
 * error / >50% warning; mem >2GB error / >1GB warning). */
static void draw_value_chip(dc_render *r, float cx, float cy, float w, float h, const char *text,
                            bool warn, bool danger)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    dc_color bg = danger ? t->error : warn ? t->warning : t->surface_container_high;
    dc_color fg = danger ? t->error : warn ? t->warning : t->surface_text;
    int bg_alpha = (danger || warn) ? 45 : 255;

    float x0 = cx - w / 2.0f, y0 = cy - h / 2.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x0, y0, w, h, h / 2.0f);
    nvgFillColor(vg, tc_alpha(bg, bg_alpha));
    nvgFill(vg);

    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 11.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(fg));
    nvgText(vg, cx, cy + 0.5f, text, NULL);
}

static void draw_row(dc_processes *ps, const dc_sysmon_proc *p, const ps_cols *cols, float row_cy)
{
    NVGcontext *vg = ps->render->vg;
    const dc_theme *t = dc_theme_current;

    /* Generic process glyph (docs/13-POPOUTS-SPEC.md: dankc has no per-app
     * icon lookup for arbitrary /proc comm names -- every row in the
     * reference screenshot already shows the same gear glyph, since the
     * user's DMS session likewise falls back to it for unrecognized
     * binaries). */
    bool cpu_warn = p->cpu_percent > 50.0f, cpu_danger = p->cpu_percent > 80.0f;
    dc_color icon_color = cpu_danger ? t->error : cpu_warn ? t->warning : t->surface_variant_text;
    dc_render_icon(ps->render, DC_ICON_SETTINGS, cols->name_x0 + 8.0f, row_cy, 13.0f, icon_color,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char name_buf[DC_SYSMON_COMM_MAX];
    snprintf(name_buf, sizeof(name_buf), "%s", p->comm);
    float name_x = cols->name_x0 + 20.0f;
    float name_w = cols->name_x1 - name_x - 4.0f;
    nvgFontFaceId(vg, ps->render->font_ui);
    nvgFontSize(vg, 12.0f);
    if (name_w > 8.0f)
        ps_ellipsize(vg, name_buf, sizeof(name_buf), name_w);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, name_x, row_cy, name_buf, NULL);

    char cpu_buf[16];
    snprintf(cpu_buf, sizeof(cpu_buf), "%.1f%%", p->cpu_percent);
    draw_value_chip(ps->render, (cols->cpu_x0 + cols->cpu_x1) / 2.0f, row_cy,
                    cols->cpu_x1 - cols->cpu_x0, 20.0f, cpu_buf, cpu_warn, cpu_danger);

    char mem_buf[24];
    format_row_mem(p->mem_kb, mem_buf, sizeof(mem_buf));
    bool mem_warn = p->mem_kb > 1024UL * 1024UL, mem_danger = p->mem_kb > 2UL * 1024UL * 1024UL;
    draw_value_chip(ps->render, (cols->mem_x0 + cols->mem_x1) / 2.0f, row_cy,
                    cols->mem_x1 - cols->mem_x0, 20.0f, mem_buf, mem_warn, mem_danger);

    char pid_buf[16];
    snprintf(pid_buf, sizeof(pid_buf), "%d", p->pid);
    nvgFontFaceId(vg, ps->render->font_ui);
    nvgFontSize(vg, 11.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 140));
    nvgText(vg, (cols->pid_x0 + cols->pid_x1) / 2.0f, row_cy, pid_buf, NULL);

    /* Disclosure chevron -- drawn only; expanding a row for the full command
     * line (ProcessContextMenu.qml) isn't wired up yet (out of this task's
     * scope), so it's a non-functional visual placeholder. */
    dc_render_icon(ps->render, DC_ICON_EXPAND_MORE, cols->chevron_cx, row_cy, 14.0f,
                  t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

static void ps_render(dc_processes *ps)
{
    if (!ps->configured || ps->phys_width <= 0)
        return;
    if (!ps->egl_ready) {
        if (!dc_egl_window_init(&ps->egl_window, ps->egl, ps->surface, ps->phys_width,
                                ps->phys_height))
            return;
        ps->egl_ready = true;
    } else {
        dc_egl_window_resize(&ps->egl_window, ps->phys_width, ps->phys_height);
    }
    if (!dc_egl_make_current(ps->egl, &ps->egl_window))
        return;
    if (!dc_render_ensure(ps->render))
        return;
    if (ps->viewport)
        wp_viewport_set_destination(ps->viewport, ps->logical_width, ps->logical_height);

    NVGcontext *vg = ps->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = ps->logical_width, h = ps->logical_height;
    const bool bottom_bar = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    int pad_near, pad_side, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, &pad_side, &pad_far);
    const float pad_top = bottom_bar ? (float)pad_far : (float)pad_near;
    const float pad_bottom = bottom_bar ? (float)pad_near : (float)pad_far;
    const float pad_side_f = (float)pad_side;

    glViewport(0, 0, ps->phys_width, ps->phys_height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)ps->scale120 / DC_SCALE_BASE);

    float pr = dc_anim_progress(&ps->anim);
    if (ps->closing)
        pr = 1.0f - (pr > 1.0f ? 1.0f : pr);
    float alpha = pr > 1.0f ? 1.0f : pr;
    float scale = 0.94f + 0.06f * pr;
    float ox = pad_side_f + (w - 2.0f * pad_side_f) * ps->anim_ox;
    float oy = pad_top + (h - pad_top - pad_bottom) * ps->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Card chrome: shadow + fill + outline, floating or stitched into the
     * bar depending on connected_frame -- see ui/connected.h. Byte-identical
     * to the old inline floating-chrome block when the toggle is off. */
    dc_connected_card_chrome(vg, ps->render, w, h, bottom_bar);

    ps_layout l = ps_get_layout(w, h);

    /* --- Header row: icon + "Processes", All/User/System tabs, search --- */
    const float header_cy = l.header_y + l.header_h / 2.0f;
    dc_render_icon(ps->render, DC_ICON_ANALYTICS, l.ix, header_cy, 18.0f, t->primary,
                  NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, ps->render->font_ui);
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    const float title_x = l.ix + 18.0f + 8.0f;
    nvgText(vg, title_x, header_cy, "Processes", NULL);
    float title_b[4];
    nvgTextBounds(vg, title_x, header_cy, "Processes", NULL, title_b);

    const float tabs_h = 24.0f;
    const float tabs_y = header_cy - tabs_h / 2.0f;
    float next_x = title_b[2] + 16.0f;
    float tab_x1;
    draw_tab(ps, 0, next_x, tabs_y, tabs_h, "All", ps->filter == DC_PSF_ALL, &tab_x1);
    next_x = tab_x1 + 6.0f;
    draw_tab(ps, 1, next_x, tabs_y, tabs_h, "User", ps->filter == DC_PSF_USER, &tab_x1);
    next_x = tab_x1 + 6.0f;
    draw_tab(ps, 2, next_x, tabs_y, tabs_h, "System", ps->filter == DC_PSF_SYSTEM, &tab_x1);

    /* Search field fills the rest of the header row (docs/13-POPOUTS-SPEC.md;
     * keyboard-exclusive like clip_picker.c's, so it's always "focused" --
     * no separate blurred visual state). */
    const float search_x0 = tab_x1 + 12.0f;
    const float search_x1 = l.ix + l.iw;
    const float search_h = 26.0f;
    const float search_y = header_cy - search_h / 2.0f;
    if (search_x1 - search_x0 > 40.0f) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, search_x0, search_y, search_x1 - search_x0, search_h, search_h / 2.0f);
        nvgFillColor(vg, tc(t->surface_container_high));
        nvgFill(vg);
        nvgStrokeColor(vg, tc(t->primary));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
        const float scy = search_y + search_h / 2.0f;
        dc_render_icon(ps->render, DC_ICON_SEARCH, search_x0 + 12.0f, scy, 14.0f,
                      t->surface_variant_text, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, ps->render->font_ui);
        nvgFontSize(vg, 12.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        char search_buf[DC_PS_QUERY_MAX + 8];
        float text_w = search_x1 - (search_x0 + 30.0f) - 8.0f;
        if (ps->query[0]) {
            snprintf(search_buf, sizeof(search_buf), "%s", ps->query);
            nvgFillColor(vg, tc(t->surface_text));
        } else {
            snprintf(search_buf, sizeof(search_buf), "Search\xe2\x80\xa6");
            nvgFillColor(vg, tc_alpha(t->surface_text, 110));
        }
        if (text_w > 8.0f)
            ps_ellipsize(vg, search_buf, sizeof(search_buf), text_w);
        nvgText(vg, search_x0 + 30.0f, scy, search_buf, NULL);
    }

    /* --- Info row: distro/host card (left) + CPU/Memory rings (right) --- */
    const float logo_r = 20.0f;
    const float logo_cx = l.ix + logo_r, logo_cy = l.info_y + l.info_h / 2.0f;
    char distro[64];
    get_distro_name(distro, sizeof(distro));
    nvgBeginPath(vg);
    nvgRoundedRect(vg, logo_cx - logo_r, logo_cy - logo_r, logo_r * 2.0f, logo_r * 2.0f, 10.0f);
    nvgFillColor(vg, tc_alpha(t->primary, 40));
    nvgFill(vg);
    char logo_letter[2] = {distro[0] ? (char)toupper((unsigned char)distro[0]) : '?', 0};
    nvgFontFaceId(vg, ps->render->font_ui);
    nvgFontSize(vg, 17.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->primary));
    nvgText(vg, logo_cx, logo_cy + 1.0f, logo_letter, NULL);

    char hostname[64] = {0};
    if (gethostname(hostname, sizeof(hostname)) != 0 || !hostname[0])
        snprintf(hostname, sizeof(hostname), "localhost");
    hostname[sizeof(hostname) - 1] = '\0';

    char uptime_buf[48];
    format_uptime(uptime_buf, sizeof(uptime_buf));
    char meta_buf[96];
    snprintf(meta_buf, sizeof(meta_buf), "%s \xe2\x80\xa2 %d procs", uptime_buf,
             dc_sysmon_process_total());

    const float text_x = logo_cx + logo_r + 12.0f;
    nvgFontFaceId(vg, ps->render->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, text_x, l.info_y + 6.0f, hostname, NULL);
    nvgFontSize(vg, 11.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, text_x, l.info_y + 24.0f, distro, NULL);
    dc_render_icon(ps->render, DC_ICON_SCHEDULE, text_x + 6.0f, l.info_y + 44.0f, 11.0f,
                  t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, ps->render->font_ui); /* icon draw above left the icon face active */
    nvgFontSize(vg, 10.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, text_x + 14.0f, l.info_y + 39.0f, meta_buf, NULL);

    const float ring_r = 26.0f;
    const float mem_ring_cx = l.ix + l.iw - ring_r, ring_cy = l.info_y + l.info_h / 2.0f;
    const float cpu_ring_cx = mem_ring_cx - ring_r * 2.0f - 14.0f;

    int cpu_pct = dc_sysmon_cpu_percent();
    char cpu_label[16];
    snprintf(cpu_label, sizeof(cpu_label), "%d%%", cpu_pct);
    dc_color cpu_accent = cpu_pct > 80 ? t->error : cpu_pct > 50 ? t->warning : t->primary;
    draw_ring(ps->render, cpu_ring_cx, ring_cy, ring_r, cpu_pct / 100.0f, cpu_accent, cpu_label,
             "CPU", NULL);

    int mem_pct = dc_sysmon_mem_percent();
    char mem_label[24];
    compact_mem_kb(dc_sysmon_mem_used_kb(), mem_label, sizeof(mem_label));
    char mem_detail[40] = {0};
    unsigned long swap_total = dc_sysmon_swap_total_kb();
    if (swap_total > 0) {
        char swap_used_buf[32];
        compact_mem_kb(dc_sysmon_swap_used_kb(), swap_used_buf, sizeof(swap_used_buf));
        snprintf(mem_detail, sizeof(mem_detail), "+%s", swap_used_buf);
    }
    dc_color mem_accent = mem_pct > 90 ? t->error : mem_pct > 70 ? t->warning : t->info;
    draw_ring(ps->render, mem_ring_cx, ring_cy, ring_r, mem_pct / 100.0f, mem_accent, mem_label,
             "Memory", swap_total > 0 ? mem_detail : NULL);

    /* --- Table header: Name | CPU | Memory | PID, CPU/Memory clickable --- */
    ps_cols cols = ps_get_cols(l.ix, l.iw);
    const float thead_cy = l.thead_y + l.thead_h / 2.0f;

    nvgFontFaceId(vg, ps->render->font_ui);
    nvgFontSize(vg, 11.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 160));
    nvgText(vg, cols.name_x0 + 20.0f, thead_cy, "Name", NULL);

    /* CPU/Memory headers double as sort-key buttons: the active one gets a
     * primary pill (matching the tabs' active styling) with a down-arrow
     * suffix; the inactive one is plain text (docs/13-POPOUTS-SPEC.md
     * reference: the currently-sorted column's header is highlighted). */
    bool cpu_active = ps->sort == DC_PROCESSES_SORT_CPU;
    char cpu_hdr[16];
    snprintf(cpu_hdr, sizeof(cpu_hdr), cpu_active ? "CPU \xe2\x86\x93" : "CPU");
    if (cpu_active) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cols.cpu_x0, l.thead_y + 2.0f, cols.cpu_x1 - cols.cpu_x0,
                       l.thead_h - 4.0f, (l.thead_h - 4.0f) / 2.0f);
        nvgFillColor(vg, tc_alpha(t->primary, 40));
        nvgFill(vg);
    }
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, cpu_active ? tc(t->primary) : tc_alpha(t->surface_text, 160));
    nvgText(vg, (cols.cpu_x0 + cols.cpu_x1) / 2.0f, thead_cy, cpu_hdr, NULL);
    ps->cpu_hdr_x0 = cols.cpu_x0;
    ps->cpu_hdr_x1 = cols.cpu_x1;
    ps->cpu_hdr_y0 = l.thead_y;
    ps->cpu_hdr_y1 = l.thead_y + l.thead_h;

    bool mem_active = ps->sort == DC_PROCESSES_SORT_MEM;
    char mem_hdr[16];
    snprintf(mem_hdr, sizeof(mem_hdr), mem_active ? "Memory \xe2\x86\x93" : "Memory");
    if (mem_active) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cols.mem_x0, l.thead_y + 2.0f, cols.mem_x1 - cols.mem_x0,
                       l.thead_h - 4.0f, (l.thead_h - 4.0f) / 2.0f);
        nvgFillColor(vg, tc_alpha(t->primary, 40));
        nvgFill(vg);
    }
    nvgFillColor(vg, mem_active ? tc(t->primary) : tc_alpha(t->surface_text, 160));
    nvgText(vg, (cols.mem_x0 + cols.mem_x1) / 2.0f, thead_cy, mem_hdr, NULL);
    ps->mem_hdr_x0 = cols.mem_x0;
    ps->mem_hdr_x1 = cols.mem_x1;
    ps->mem_hdr_y0 = l.thead_y;
    ps->mem_hdr_y1 = l.thead_y + l.thead_h;

    nvgFillColor(vg, tc_alpha(t->surface_text, 160));
    nvgText(vg, (cols.pid_x0 + cols.pid_x1) / 2.0f, thead_cy, "PID", NULL);

    nvgBeginPath(vg);
    nvgMoveTo(vg, l.ix, l.thead_y + l.thead_h + 1.0f);
    nvgLineTo(vg, l.ix + l.iw, l.thead_y + l.thead_h + 1.0f);
    nvgStrokeColor(vg, tc_alpha(t->outline, 50));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* --- Row list, scrollable ------------------------------------------- */
    float content_h = ps->view_count > 0
                          ? (float)ps->view_count * (DC_PS_ROW_H + DC_PS_ROW_GAP) - DC_PS_ROW_GAP
                          : 0.0f;
    ps->scroll_max = content_h > l.list_h ? content_h - l.list_h : 0.0f;
    if (ps->scroll < 0.0f)
        ps->scroll = 0.0f;
    if (ps->scroll > ps->scroll_max)
        ps->scroll = ps->scroll_max;

    if (ps->view_count == 0) {
        dc_color dim = t->surface_text;
        dim.a = 90;
        dc_render_icon(ps->render, DC_ICON_ANALYTICS, w / 2.0f, l.list_y0 + l.list_h / 2.0f - 14.0f,
                      30.0f, dim, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, ps->render->font_ui);
        nvgFontSize(vg, 12.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 120));
        nvgText(vg, w / 2.0f, l.list_y0 + l.list_h / 2.0f + 14.0f,
               ps->query[0] ? "No matching processes" : "No processes", NULL);
    } else {
        nvgSave(vg);
        nvgScissor(vg, l.ix, l.list_y0, l.iw, l.list_h);
        for (int i = 0; i < ps->view_count; i++) {
            float y = l.list_y0 + (float)i * (DC_PS_ROW_H + DC_PS_ROW_GAP) - ps->scroll;
            if (y + DC_PS_ROW_H < l.list_y0 || y > l.list_y1)
                continue; /* fully outside the viewport -- skip drawing */
            draw_row(ps, &ps->view[i], &cols, y + DC_PS_ROW_H / 2.0f);
        }
        nvgRestore(vg);

        if (ps->scroll_max > 0.0f) {
            float track_x = l.ix + l.iw - 3.0f;
            float thumb_h = l.list_h * (l.list_h / content_h);
            if (thumb_h < 24.0f)
                thumb_h = 24.0f;
            float thumb_y = l.list_y0 + (l.list_h - thumb_h) * (ps->scroll / ps->scroll_max);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, track_x, thumb_y, 3.0f, thumb_h, 1.5f);
            nvgFillColor(vg, tc_alpha(t->outline, 140));
            nvgFill(vg);
        }
    }

    nvgEndFrame(vg);
    if ((dc_anim_active(&ps->anim) || ps->closing) && !ps->frame_cb) {
        ps->frame_cb = wl_surface_frame(ps->surface);
        wl_callback_add_listener(ps->frame_cb, &ps_frame_listener, ps);
    }
    dc_egl_swap(ps->egl, &ps->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_processes *ps = data;
    DC_UNUSED(fs);
    ps->scale120 = (int)scale;
    recompute_physical(ps);
    ps_render(ps);
}
static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_processes *ps = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    ps->logical_width = width > 0 ? (int)width : ps_surface_width();
    ps->logical_height = height > 0 ? (int)height : DC_PS_HEIGHT;
    ps->configured = true;
    recompute_physical(ps);
    ps_render(ps);
}
static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_processes *ps = data;
    DC_UNUSED(surface);
    ps->configured = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_processes *dc_processes_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_processes *ps = calloc(1, sizeof(*ps));
    ps->wl = wl;
    ps->egl = egl;
    ps->render = render;
    ps->logical_width = ps_surface_width();
    ps->logical_height = DC_PS_HEIGHT;
    ps->scale120 = DC_SCALE_BASE;
    ps->sort = DC_PROCESSES_SORT_CPU;
    return ps;
}

static void ps_show(dc_processes *ps, dc_output *output, dc_processes_sort sort)
{
    ps->output = output;
    ps->configured = false;
    ps->egl_ready = false;
    ps->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    ps->sort = sort;
    ps->filter = DC_PSF_ALL;
    ps->query[0] = '\0';
    ps->scroll = 0.0f;

    /* Scan only while the popout is actually open (docs/13-POPOUTS-SPEC.md;
     * this task's explicit "no background cost" requirement). Enabling
     * resets the internal rate-limit gate, so this poll runs immediately
     * rather than waiting up to 2s for the first row to appear. */
    dc_sysmon_set_process_scan_enabled(true);
    dc_sysmon_poll_processes();
    ps_rebuild_view(ps);

    dc_anim_start(&ps->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    ps->surface = wl_compositor_create_surface(ps->wl->compositor);
    if (ps->wl->fractional_scale_mgr) {
        ps->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            ps->wl->fractional_scale_mgr, ps->surface);
        wp_fractional_scale_v1_add_listener(ps->fractional_scale, &fractional_scale_listener, ps);
    }
    if (ps->wl->viewporter)
        ps->viewport = wp_viewporter_get_viewport(ps->wl->viewporter, ps->surface);

    ps->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        ps->wl->layer_shell, ps->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:processes");

    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_END, DC_PS_SIDE_MARGIN);
    ps->anim_ox = pa.origin_x;
    ps->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(ps->layer_surface, pa.anchor);
    ps->logical_width = ps_surface_width();
    zwlr_layer_surface_v1_set_size(ps->layer_surface, (uint32_t)ps->logical_width, DC_PS_HEIGHT);
    zwlr_layer_surface_v1_set_margin(ps->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        ps->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(ps->layer_surface, &layer_surface_listener, ps);
    wl_surface_commit(ps->surface);
    ps->visible = true;
    ps->closing = false;
    dc_debug("processes popout shown");
}

static void ps_teardown(dc_processes *ps)
{
    if (ps->frame_cb) {
        wl_callback_destroy(ps->frame_cb);
        ps->frame_cb = NULL;
    }
    if (ps->egl_ready)
        dc_egl_window_finish(&ps->egl_window, ps->egl);
    if (ps->viewport)
        wp_viewport_destroy(ps->viewport);
    if (ps->fractional_scale)
        wp_fractional_scale_v1_destroy(ps->fractional_scale);
    if (ps->layer_surface)
        zwlr_layer_surface_v1_destroy(ps->layer_surface);
    if (ps->surface)
        wl_surface_destroy(ps->surface);
    ps->egl_ready = false;
    ps->configured = false;
    ps->viewport = NULL;
    ps->fractional_scale = NULL;
    ps->layer_surface = NULL;
    ps->surface = NULL;
    ps->visible = false;
    ps->closing = false;
    dc_sysmon_set_process_scan_enabled(false); /* no background scan cost once closed */
    dc_debug("processes popout hidden");
}

static void ps_begin_close(dc_processes *ps)
{
    if (!ps->visible || ps->closing)
        return;
    dc_anim_start(&ps->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    ps->closing = true;
    if (!dc_anim_active(&ps->anim)) {
        ps_teardown(ps);
        return;
    }
    ps_render(ps);
}

void dc_processes_toggle(dc_processes *ps, dc_output *output, dc_processes_sort sort)
{
    if (ps->visible) {
        if (!ps->closing && ps->sort != sort) {
            /* Same popout, different trigger chip (cpuUsage vs memUsage):
             * re-sort in place rather than closing + reopening. */
            ps->sort = sort;
            ps_rebuild_view(ps);
            ps->scroll = 0.0f;
            ps_render(ps);
            return;
        }
        ps_begin_close(ps);
        return;
    }
    ps_show(ps, output, sort);
}

void dc_processes_hide(dc_processes *ps)
{
    ps_begin_close(ps);
}

bool dc_processes_visible(dc_processes *ps)
{
    return ps->visible;
}

struct wl_surface *dc_processes_surface(dc_processes *ps)
{
    return ps->surface;
}

void dc_processes_refresh(dc_processes *ps)
{
    if (!ps || !ps->visible || ps->closing)
        return;
    ps_rebuild_view(ps);
    ps_render(ps);
}

void dc_processes_handle_key(dc_processes *ps, uint32_t keysym, const char *utf8)
{
    if (!ps->visible || ps->closing)
        return;
    switch (keysym) {
    case XKB_KEY_Escape:
        ps_begin_close(ps);
        return;
    case XKB_KEY_BackSpace: {
        size_t n = strlen(ps->query);
        if (n > 0) {
            ps->query[n - 1] = '\0';
            ps_rebuild_view(ps);
            ps->scroll = 0.0f;
        }
        break;
    }
    default:
        if (utf8 && utf8[0] && !((unsigned char)utf8[0] < 0x20) && (unsigned char)utf8[0] != 0x7f) {
            size_t n = strlen(ps->query), add = strlen(utf8);
            if (n + add < sizeof(ps->query)) {
                memcpy(ps->query + n, utf8, add + 1);
                ps_rebuild_view(ps);
                ps->scroll = 0.0f;
            }
        }
        break;
    }
    ps_render(ps);
}

static inline bool in_rect(double x, double y, float x0, float y0, float x1, float y1)
{
    return x1 > x0 && x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

void dc_processes_handle_click(dc_processes *ps, double x, double y)
{
    if (!ps->visible || ps->closing)
        return;

    for (int i = 0; i < 3; i++) {
        if (in_rect(x, y, ps->tab_x0[i], ps->tab_y0[i], ps->tab_x1[i], ps->tab_y1[i])) {
            if (ps->filter != (dc_ps_filter)i) {
                ps->filter = (dc_ps_filter)i;
                ps_rebuild_view(ps);
                ps->scroll = 0.0f;
                ps_render(ps);
            }
            return;
        }
    }

    if (in_rect(x, y, ps->cpu_hdr_x0, ps->cpu_hdr_y0, ps->cpu_hdr_x1, ps->cpu_hdr_y1)) {
        if (ps->sort != DC_PROCESSES_SORT_CPU) {
            ps->sort = DC_PROCESSES_SORT_CPU;
            ps_rebuild_view(ps);
            ps->scroll = 0.0f;
            ps_render(ps);
        }
        return;
    }
    if (in_rect(x, y, ps->mem_hdr_x0, ps->mem_hdr_y0, ps->mem_hdr_x1, ps->mem_hdr_y1)) {
        if (ps->sort != DC_PROCESSES_SORT_MEM) {
            ps->sort = DC_PROCESSES_SORT_MEM;
            ps_rebuild_view(ps);
            ps->scroll = 0.0f;
            ps_render(ps);
        }
        return;
    }
}

void dc_processes_handle_scroll(dc_processes *ps, int steps_v)
{
    if (!ps->visible || ps->closing || steps_v == 0)
        return;
    float s = ps->scroll + (float)steps_v * DC_PS_SCROLL_STEP;
    if (s < 0.0f)
        s = 0.0f;
    if (s > ps->scroll_max)
        s = ps->scroll_max;
    if (s == ps->scroll)
        return;
    ps->scroll = s;
    ps_render(ps);
}

void dc_processes_destroy(dc_processes *ps)
{
    if (!ps)
        return;
    if (ps->visible)
        ps_teardown(ps);
    free(ps);
}
