#include "ui/dashboard.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/mpris.h"
#include "services/sysmon.h"
#include "services/wallpaper.h"
#include "services/weather.h"
#include "theme/theme.h"
#include "ui/bar/bar_tokens.h"
#include "ui/material_bg.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Declarations only (no STB_IMAGE_IMPLEMENTATION): nanovg.c already provides
 * the (non-static) stbi_load/stbi_image_free symbols this TU links against,
 * so the Wallpapers tab can decode+downscale its own thumbnails instead of
 * handing nvgCreateImage() a full-size image per grid cell. */
#include "stb_image.h"

/* Popout size: DMS's DankDashPopout.qml popupWidth 700 + the OverviewTab's
 * ~410px content plus the tab bar, matching the user's live reference
 * screenshots (~/Pictures/Screenshots/Screenshot from 2026-07-02 14-18-*.png).
 * Other dankc popouts already use QML popupWidth-ish numbers directly as
 * logical px, so this follows suit. */
#define DC_DASH_WIDTH 700
#define DC_DASH_HEIGHT 512
#define DC_SCALE_BASE 120
#define DC_DASH_PAD 6.0f     /* room for the drop shadow */
#define DC_DASH_MARGIN 14.0f /* content inset from the card edge */
#define DC_DASH_TABBAR_H 78.0f
#define DC_DASH_MAX_HITS 48

/* Click targets recorded by the draw pass, consumed by handle_click. */
typedef enum {
    HIT_NONE = 0,
    HIT_TAB,           /* payload = dc_dash_tab */
    HIT_CAL_PREV,
    HIT_CAL_NEXT,
    HIT_MEDIA_PREV,
    HIT_MEDIA_PLAY,
    HIT_MEDIA_NEXT,
    HIT_WEATHER_DAILY,
    HIT_WEATHER_HOURLY,
    HIT_WALLPAPER_ITEM, /* payload = index into dc_dashboard::walls */
} dash_hit_kind;

typedef struct {
    float x0, y0, x1, y1;
    dash_hit_kind kind;
    int payload;
} dash_hit;

/* --- Wallpapers tab grid (docs/13-POPOUTS-SPEC.md sec.5) ------------------
 * A directory of images scanned once per (re)open, decoded+downscaled into
 * small nvg-image thumbnails a couple at a time (never more than
 * DC_WALL_DECODE_BUDGET per draw call) so opening the tab never stalls a
 * frame. Thumbnails are cached in-memory for the life of the dc_dashboard and
 * only dropped when the source directory changes. */
#define DC_WALL_MAX 256
#define DC_WALL_THUMB_MAX 256 /* thumbnail max width/height, px */
#define DC_WALL_DECODE_BUDGET 2 /* thumbnails decoded per draw call */

typedef struct {
    char path[512];
    int thumb_img; /* nvg image handle, 0 = not yet loaded/failed */
    int thumb_w, thumb_h;
    bool tried; /* a decode was attempted (success or failure) */
} dc_wall_entry;

struct dc_dashboard {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
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

    dc_dash_tab tab;
    int cal_month_offset; /* months forward/back from the current month */
    bool weather_hourly;  /* Weather tab: false = Daily pill, true = Hourly pill */

    /* Album-art cache (Media tab + Overview mini-card). One handle at a time. */
    char art_url[512];
    char art_path[512];
    int art_img;
    bool art_pending;

    /* Wallpapers tab grid (see dc_wall_entry above). */
    char wall_dir[512];
    bool wall_scanned;
    bool wall_pending; /* undecoded thumbnails remain: keep frame-stepping */
    dc_wall_entry walls[DC_WALL_MAX];
    int wall_count;
    float wall_scroll, wall_scroll_max;

    dash_hit hits[DC_DASH_MAX_HITS];
    int hit_count;

    dc_dashboard_action_cb settings_cb;
    void *settings_user;
};

static void dash_render(dc_dashboard *d);
static void dash_teardown(dc_dashboard *d);

static void dash_frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener dash_frame_listener = {.done = dash_frame_done};

static void dash_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_dashboard *d = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    d->frame_cb = NULL;
    if (!d->visible)
        return;
    if (dc_anim_active(&d->anim) || (d->wall_pending && !d->closing))
        dash_render(d);
    else if (d->closing)
        dash_teardown(d);
}

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

static void push_hit(dc_dashboard *d, float x0, float y0, float x1, float y1, dash_hit_kind kind,
                     int payload)
{
    if (d->hit_count >= DC_DASH_MAX_HITS)
        return;
    dash_hit *h = &d->hits[d->hit_count++];
    h->x0 = x0;
    h->y0 = y0;
    h->x1 = x1;
    h->y1 = y1;
    h->kind = kind;
    h->payload = payload;
}

/* --- small shared helpers ------------------------------------------------- */

/* Truncate `buf` on a UTF-8 boundary + ellipsis until it fits `max_w` px at the
 * vg's current font (local twin of controlcenter.c's cc_ellipsize). */
static void dash_ellipsize(NVGcontext *vg, char *buf, size_t bufsize, float max_w)
{
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, buf, NULL, bounds);
    if (bounds[2] - bounds[0] <= max_w)
        return;
    size_t len = strlen(buf);
    if (bufsize < 4)
        return;
    char tmp[256];
    if (bufsize > sizeof(tmp))
        bufsize = sizeof(tmp);
    if (len > bufsize - 4)
        len = bufsize - 4;
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80)
            len--;
        snprintf(tmp, bufsize, "%.*s\xe2\x80\xa6", (int)len, buf);
        nvgTextBounds(vg, 0.0f, 0.0f, tmp, NULL, bounds);
        if (bounds[2] - bounds[0] <= max_w || len == 0) {
            memcpy(buf, tmp, bufsize);
            return;
        }
    }
    snprintf(buf, bufsize, "\xe2\x80\xa6");
}

static void draw_card(NVGcontext *vg, float x, float y, float w, float h, NVGcolor fill)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    nvgFillColor(vg, fill);
    nvgFill(vg);
}

static void get_username(char *user, size_t sz)
{
    struct passwd *pw = getpwuid(getuid());
    const char *name = (pw && pw->pw_name && pw->pw_name[0]) ? pw->pw_name : "";
    snprintf(user, sz, "%s", name[0] ? name : "User");
}

static void get_uptime(char *sub, size_t sz)
{
    double up = -1.0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1)
            up = -1.0;
        fclose(f);
    }
    if (up >= 0.0) {
        int total_min = (int)(up / 60.0);
        int hours = total_min / 60;
        int mins = total_min % 60;
        if (hours > 0)
            snprintf(sub, sz, "up %dh %dm", hours, mins);
        else
            snprintf(sub, sz, "up %dm", mins);
    } else {
        snprintf(sub, sz, "up");
    }
}

/* ~/.face into a nanovg image (PNG/JPEG via dc_render_load_icon), or 0. */
static int load_face_image(dc_render *r, int size)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/.face", home);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return dc_render_load_icon(r, path, size);
}

/* Map dc_weather_icon_name() -> Material Symbols codepoint (same table the bar
 * uses; kept local so the dashboard doesn't depend on bar internals). */
static int weather_codepoint(const char *name)
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
    return DC_ICON_CLOUD;
}

/* --- album art ------------------------------------------------------------ */

/* Minimal percent-decode of a file:// path into `out`. */
static void url_decode_path(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_sz; i++) {
        if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static unsigned long djb2(const char *s)
{
    unsigned long h = 5381;
    for (; *s; s++)
        h = ((h << 5) + h) + (unsigned char)*s;
    return h;
}

/* Fork `curl -o path url` detached; non-blocking (the file appears a frame or
 * two later, like weather.c's fetch). SIGCHLD is SIG_IGN process-wide. */
static void fork_download(const char *url, const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execlp("curl", "curl", "-sfL", "--max-time", "10", "-o", path, url, (char *)NULL);
        _exit(127);
    }
}

/* Resolve mpris:artUrl to a loaded nanovg image handle (cached), or 0.
 * file:// loads directly; http(s):// downloads once to /tmp and loads when
 * ready. */
static int ensure_art(dc_dashboard *d, const char *url)
{
    if (!url || !url[0]) {
        if (d->art_img > 0)
            nvgDeleteImage(d->render->vg, d->art_img);
        d->art_img = 0;
        d->art_url[0] = '\0';
        d->art_path[0] = '\0';
        d->art_pending = false;
        return 0;
    }

    if (strcmp(url, d->art_url) != 0) {
        if (d->art_img > 0)
            nvgDeleteImage(d->render->vg, d->art_img);
        d->art_img = 0;
        d->art_pending = false;
        snprintf(d->art_url, sizeof(d->art_url), "%s", url);
        d->art_path[0] = '\0';

        if (strncmp(url, "file://", 7) == 0) {
            url_decode_path(url + 7, d->art_path, sizeof(d->art_path));
        } else if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
            snprintf(d->art_path, sizeof(d->art_path), "/tmp/dankc-art-%lu", djb2(url));
            if (access(d->art_path, F_OK) != 0) {
                fork_download(url, d->art_path);
                d->art_pending = true;
            }
        }
    }

    if (d->art_img <= 0 && d->art_path[0] && access(d->art_path, F_OK) == 0) {
        d->art_img = dc_render_load_icon(d->render, d->art_path, 256);
        if (d->art_img <= 0)
            d->art_path[0] = '\0'; /* undecodable: stop retrying */
        d->art_pending = false;
    }
    return d->art_img;
}

/* Circular album art at (cx,cy,r), or a music-note placeholder circle. */
static void draw_album_circle(dc_dashboard *d, float cx, float cy, float r, int art)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    if (art > 0) {
        NVGpaint pat = nvgImagePattern(vg, cx - r, cy - r, r * 2.0f, r * 2.0f, 0.0f, art, 1.0f);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
    } else {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r);
        nvgFillColor(vg, tc(t->surface_container_highest));
        nvgFill(vg);
        dc_render_icon(d->render, DC_ICON_MUSIC_NOTE, cx, cy, r, t->surface_variant_text,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    }
}

/* --- tab bar -------------------------------------------------------------- */

static const struct {
    dc_dash_tab tab;
    int icon;
    const char *label;
} k_tabs[] = {
    {DC_DASH_OVERVIEW, DC_ICON_DASHBOARD, "Overview"},
    {DC_DASH_MEDIA, DC_ICON_MUSIC_NOTE, "Media"},
    {DC_DASH_WALLPAPERS, DC_ICON_WALLPAPER, "Wallpapers"},
    {DC_DASH_WEATHER, DC_ICON_WB_SUNNY, "Weather"},
    {DC_DASH_SETTINGS, DC_ICON_SETTINGS, "Settings"},
};

static void draw_tabbar(dc_dashboard *d, float w)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    const float x0 = DC_DASH_PAD;
    const float slot_w = (w - 2.0f * DC_DASH_PAD) / 5.0f;
    const float icon_y = DC_DASH_PAD + 26.0f;
    const float label_y = DC_DASH_PAD + 52.0f;

    for (int i = 0; i < 5; i++) {
        const float cx = x0 + slot_w * ((float)i + 0.5f);
        const bool active = k_tabs[i].tab == d->tab;
        dc_color icon_col = active ? t->primary : t->surface_variant_text;
        dc_color label_col = active ? t->primary : t->surface_text;

        dc_render_icon(d->render, k_tabs[i].icon, cx, icon_y, 22.0f, icon_col,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        nvgFontFaceId(vg, d->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(label_col));
        nvgText(vg, cx, label_y, k_tabs[i].label, NULL);

        if (active) {
            float bounds[4];
            nvgTextBounds(vg, 0.0f, 0.0f, k_tabs[i].label, NULL, bounds);
            float lw = bounds[2] - bounds[0];
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - lw / 2.0f, label_y + 12.0f, lw, 3.0f, 1.5f);
            nvgFillColor(vg, tc(t->primary));
            nvgFill(vg);
        }
        push_hit(d, cx - slot_w / 2.0f, DC_DASH_PAD, cx + slot_w / 2.0f, DC_DASH_TABBAR_H,
                 HIT_TAB, (int)k_tabs[i].tab);
    }

    /* Divider under the tab bar. */
    nvgBeginPath(vg);
    nvgRect(vg, x0, DC_DASH_TABBAR_H, w - 2.0f * DC_DASH_PAD, 1.0f);
    nvgFillColor(vg, tc_alpha(t->outline, 40));
    nvgFill(vg);
}

/* --- media transport row (shared by Overview mini-card + Media tab) ------- */

static void draw_transport(dc_dashboard *d, float cx, float cy, bool playing, float play_r,
                           float gap)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    const float side = play_r + gap + 12.0f;
    const float prev_x = cx - side;
    const float next_x = cx + side;

    dc_render_icon(d->render, DC_ICON_SKIP_PREVIOUS, prev_x, cy, 24.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, play_r);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);
    dc_render_icon(d->render, playing ? DC_ICON_PAUSE : DC_ICON_PLAY_ARROW, cx, cy, play_r * 1.05f,
                   t->primary_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    dc_render_icon(d->render, DC_ICON_SKIP_NEXT, next_x, cy, 24.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    push_hit(d, prev_x - 16.0f, cy - 16.0f, prev_x + 16.0f, cy + 16.0f, HIT_MEDIA_PREV, 0);
    push_hit(d, cx - play_r, cy - play_r, cx + play_r, cy + play_r, HIT_MEDIA_PLAY, 0);
    push_hit(d, next_x - 16.0f, cy - 16.0f, next_x + 16.0f, cy + 16.0f, HIT_MEDIA_NEXT, 0);
}

static void fmt_time(int64_t us, char *out, size_t sz)
{
    if (us < 0)
        us = 0;
    long secs = (long)(us / 1000000);
    snprintf(out, sz, "%ld:%02ld", secs / 60, secs % 60);
}

/* --- Overview cards ------------------------------------------------------- */

static void draw_clock_card(dc_dashboard *d, float x, float y, float w, float h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    draw_card(vg, x, y, w, h, tc(t->surface_container_high));

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hh[4], mm[4], date[16];
    int hour = tm.tm_hour;
    if (!cfg->clock_24h) {
        hour = hour % 12;
        if (hour == 0)
            hour = 12;
    }
    snprintf(hh, sizeof(hh), "%02d", hour);
    snprintf(mm, sizeof(mm), "%02d", tm.tm_min);
    strftime(date, sizeof(date), "%b %d", &tm);

    const float tx = x + 18.0f;
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 50.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgText(vg, tx, y + h * 0.32f, hh, NULL);
    nvgText(vg, tx, y + h * 0.60f, mm, NULL);
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, tx, y + h * 0.85f, date, NULL);
}

static void draw_weather_card(dc_dashboard *d, float x, float y, float w, float h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    draw_card(vg, x, y, w, h, tc(t->surface_container_high));

    dc_weather_state ws;
    bool have = dc_weather_get(&ws) && ws.valid;
    const float cy = y + h / 2.0f;
    if (!have) {
        nvgFontFaceId(vg, d->render->font_ui);
        nvgFontSize(vg, 14.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, x + 16.0f, cy, "Weather unavailable", NULL);
        return;
    }
    int icon = weather_codepoint(dc_weather_icon_name(ws.weather_code, ws.is_day));
    dc_render_icon(d->render, icon, x + 32.0f, cy, 40.0f, t->primary,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char temp[16];
    snprintf(temp, sizeof(temp), "%d\xc2\xb0%s", ws.temp_c, cfg->weather_fahrenheit ? "F" : "C");
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 26.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x + 64.0f, cy - 12.0f, temp, NULL);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, x + 64.0f, cy + 12.0f, dc_weather_condition_name(ws.weather_code), NULL);
}

static void draw_user_card(dc_dashboard *d, float x, float y, float w, float h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    draw_card(vg, x, y, w, h, tc(t->surface_container_high));

    const float r = 34.0f;
    const float acx = x + 20.0f + r;
    const float acy = y + h / 2.0f;
    char user[64];
    get_username(user, sizeof(user));

    int face = load_face_image(d->render, 68);
    if (face > 0) {
        NVGpaint pat = nvgImagePattern(vg, acx - r, acy - r, r * 2.0f, r * 2.0f, 0.0f, face, 1.0f);
        nvgBeginPath(vg);
        nvgCircle(vg, acx, acy, r);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
        nvgDeleteImage(vg, face);
    } else {
        nvgBeginPath(vg);
        nvgCircle(vg, acx, acy, r);
        nvgFillColor(vg, tc(t->surface_container_highest));
        nvgFill(vg);
        char initial[2] = {(char)toupper((unsigned char)user[0]), '\0'};
        nvgFontFaceId(vg, d->render->font_ui);
        nvgFontSize(vg, 26.0f);
        nvgFillColor(vg, tc(t->surface_text));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, acx, acy, initial, NULL);
    }

    const float lx = acx + r + 16.0f;
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 18.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, lx, y + h * 0.30f, user, NULL);

    dc_render_icon(d->render, DC_ICON_PERSON, lx + 7.0f, y + h * 0.56f, 15.0f, t->primary,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    /* dc_render_icon() leaves the icon font + CENTER alignment selected;
     * restore the UI font and LEFT alignment for each label. */
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, lx + 20.0f, y + h * 0.56f, "on Niri", NULL);

    char up[32];
    get_uptime(up, sizeof(up));
    dc_render_icon(d->render, DC_ICON_SCHEDULE, lx + 7.0f, y + h * 0.80f, 15.0f, t->primary,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, lx + 20.0f, y + h * 0.80f, up, NULL);
}

static void draw_vmeter(dc_dashboard *d, float cx, float top, float bottom, float trackw,
                        float frac, int icon)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    const float x = cx - trackw / 2.0f;
    const float trackh = bottom - top;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, top, trackw, trackh, trackw / 2.0f);
    nvgFillColor(vg, tc_alpha(t->outline, 70));
    nvgFill(vg);

    float fh = trackh * frac;
    if (fh < trackw)
        fh = trackw;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, bottom - fh, trackw, fh, trackw / 2.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);

    dc_render_icon(d->render, icon, cx, bottom + 16.0f, 16.0f, t->surface_variant_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

static void draw_sysmon_card(dc_dashboard *d, float x, float y, float w, float h)
{
    const dc_theme *t = dc_theme_current;
    draw_card(d->render->vg, x, y, w, h, tc(t->surface_container_high));

    const float top = y + 20.0f;
    const float bottom = y + h - 34.0f;
    const float trackw = 12.0f;
    /* three evenly spaced columns */
    for (int i = 0; i < 3; i++) {
        float cx = x + w * ((float)i + 0.5f) / 3.0f;
        float frac = 0.0f;
        int icon = DC_ICON_MEMORY;
        if (i == 0) {
            frac = dc_sysmon_cpu_percent() / 100.0f;
            icon = DC_ICON_MEMORY;
        } else if (i == 1) {
            int tc_ = dc_sysmon_temp_c();
            frac = tc_ > 0 ? (float)(tc_ - 20) / 65.0f : 0.0f;
            icon = DC_ICON_DEVICE_THERMOSTAT;
        } else {
            frac = dc_sysmon_mem_percent() / 100.0f;
            icon = DC_ICON_DEVELOPER_BOARD;
        }
        draw_vmeter(d, cx, top, bottom, trackw, frac, icon);
    }
}

static void draw_calendar_card(dc_dashboard *d, float x, float y, float w, float h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    draw_card(vg, x, y, w, h, tc(t->surface_container_high));

    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);

    /* Displayed month = current month + offset. */
    struct tm disp = today;
    disp.tm_mday = 1;
    disp.tm_hour = 12;
    disp.tm_mon += d->cal_month_offset;
    mktime(&disp); /* normalize */

    char title[32];
    strftime(title, sizeof(title), "%B %Y", &disp);

    const float pad = 16.0f;
    const float hdr_y = y + 22.0f;

    /* Header: < Month Year > */
    dc_render_icon(d->render, DC_ICON_CHEVRON_LEFT, x + pad + 6.0f, hdr_y, 20.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    dc_render_icon(d->render, DC_ICON_CHEVRON_RIGHT, x + w - pad - 6.0f, hdr_y, 20.0f,
                   t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    push_hit(d, x + pad - 6.0f, hdr_y - 14.0f, x + pad + 20.0f, hdr_y + 14.0f, HIT_CAL_PREV, 0);
    push_hit(d, x + w - pad - 20.0f, hdr_y - 14.0f, x + w - pad + 6.0f, hdr_y + 14.0f, HIT_CAL_NEXT,
             0);
    nvgFontFaceId(vg, d->render->font_ui);
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x + w / 2.0f, hdr_y, title, NULL);

    /* Weekday header + day grid, 7 columns. Column 0 starts at the config's
     * first_day_of_week (docs/14-COMPLETION-PLAN.md W2 "Locale" tab; 0=Sunday
     * .. 6=Saturday, same tm_wday numbering), defaulting to Sunday so this
     * matches the previous hardcoded layout when unset. */
    static const char *dow[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int fdow = dc_config_current->first_day_of_week;
    if (fdow < 0 || fdow > 6)
        fdow = 0;
    const float grid_x = x + pad;
    const float grid_w = w - 2.0f * pad;
    const float col_w = grid_w / 7.0f;
    const float dow_y = y + 50.0f;
    nvgFontSize(vg, 12.0f);
    for (int c = 0; c < 7; c++) {
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, grid_x + col_w * ((float)c + 0.5f), dow_y, dow[(c + fdow) % 7], NULL);
    }

    /* First weekday + day count of the displayed month. */
    struct tm first = disp;
    first.tm_mday = 1;
    mktime(&first);
    int start_dow = (first.tm_wday - fdow + 7) % 7; /* column offset for day 1 */
    int mon = disp.tm_mon;
    struct tm probe = disp;
    int days_in_month = 0;
    for (int dday = 28; dday <= 31; dday++) {
        probe = disp;
        probe.tm_mday = dday;
        probe.tm_hour = 12;
        time_t tt = mktime(&probe);
        struct tm chk;
        localtime_r(&tt, &chk);
        if (chk.tm_mon == mon)
            days_in_month = dday;
    }

    const float rows_y0 = y + 74.0f;
    const float row_h = (y + h - 12.0f - rows_y0) / 6.0f;
    const bool this_month = d->cal_month_offset == 0;

    nvgFontSize(vg, 13.0f);
    for (int cell = 0; cell < 42; cell++) {
        int col = cell % 7;
        int row = cell / 7;
        int daynum = cell - start_dow + 1;
        float ccx = grid_x + col_w * ((float)col + 0.5f);
        float ccy = rows_y0 + row_h * ((float)row + 0.5f);

        if (daynum < 1 || daynum > days_in_month) {
            /* Leading/trailing filler dimmed (approx: no exact adjacent-month
             * numbers, matching the reference's very faint filler). */
            continue;
        }
        bool is_today = this_month && daynum == today.tm_mday;
        char ds[4];
        snprintf(ds, sizeof(ds), "%d", daynum);
        if (is_today) {
            nvgBeginPath(vg);
            nvgCircle(vg, ccx, ccy, 13.0f);
            nvgFillColor(vg, tc(t->primary));
            nvgFill(vg);
            nvgFillColor(vg, tc(t->primary_text));
        } else {
            nvgFillColor(vg, tc(t->surface_text));
        }
        /* NVG_ALIGN_MIDDLE centers on the font's line-box (ascender+
         * descender)/2, not the digit's actual ink -- same class of bug as
         * dc_render_icon's icon glyphs (see render/nvg.c). Most visible on
         * the "today" pill where it reads ~1px left+high of true center;
         * measure real ink bounds and center that instead, same fix. */
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        float dbounds[4];
        nvgTextInkBounds(vg, 0.0f, 0.0f, ds, NULL, dbounds);
        float dcy = ccy - (dbounds[1] + dbounds[3]) / 2.0f;
        float dcx = ccx - (dbounds[0] + dbounds[2]) / 2.0f;
        nvgText(vg, dcx, dcy, ds, NULL);
    }
}

static void draw_media_card(dc_dashboard *d, float x, float y, float w, float h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    draw_card(vg, x, y, w, h, tc(t->surface_container_high));

    dc_mpris_info m;
    bool have = dc_mpris_read(&m) && m.active;
    const float cx = x + w / 2.0f;
    if (!have) {
        dc_render_icon(d->render, DC_ICON_MUSIC_NOTE, cx, y + h / 2.0f - 20.0f, 40.0f,
                       t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, d->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, cx, y + h / 2.0f + 20.0f, "Nothing playing", NULL);
        return;
    }

    int art = ensure_art(d, m.art_url);
    const float art_r = 42.0f;
    draw_album_circle(d, cx, y + 26.0f + art_r, art_r, art);

    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char title[256];
    snprintf(title, sizeof(title), "%s", m.title[0] ? m.title : "Unknown Track");
    nvgFontSize(vg, 14.0f);
    dash_ellipsize(vg, title, sizeof(title), w - 20.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, cx, y + 128.0f, title, NULL);

    if (m.artist[0]) {
        char artist[128];
        snprintf(artist, sizeof(artist), "%s", m.artist);
        nvgFontSize(vg, 13.0f);
        dash_ellipsize(vg, artist, sizeof(artist), w - 20.0f);
        nvgFillColor(vg, tc(t->primary));
        nvgText(vg, cx, y + 148.0f, artist, NULL);
    }

    /* Progress bar. */
    const float bar_x = x + 16.0f;
    const float bar_w = w - 32.0f;
    const float bar_y = y + 172.0f;
    float frac = m.length_us > 0 ? (float)m.position_us / (float)m.length_us : 0.0f;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bar_x, bar_y, bar_w, 4.0f, 2.0f);
    nvgFillColor(vg, tc_alpha(t->outline, 80));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bar_x, bar_y, bar_w * frac, 4.0f, 2.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);

    draw_transport(d, cx, y + h - 30.0f, m.playing, 18.0f, 6.0f);
}

static void draw_overview(dc_dashboard *d, float w)
{
    const float cx0 = DC_DASH_PAD + DC_DASH_MARGIN;
    const float cw = w - 2.0f * (DC_DASH_PAD + DC_DASH_MARGIN);
    const float cy0 = DC_DASH_TABBAR_H + 10.0f;
    const float sm = 8.0f;

    /* Column split matches DMS OverviewTab.qml (0.2/0.3/0.5 top, 0.6/0.2). */
    draw_clock_card(d, cx0, cy0, cw * 0.2f - sm, 176.0f);
    draw_weather_card(d, cx0 + cw * 0.2f, cy0, cw * 0.3f - sm, 100.0f);
    draw_user_card(d, cx0 + cw * 0.5f + sm, cy0, cw * 0.5f - sm, 100.0f);
    draw_sysmon_card(d, cx0, cy0 + 176.0f + sm, cw * 0.2f - sm, 216.0f);
    draw_calendar_card(d, cx0 + cw * 0.2f, cy0 + 100.0f + sm, cw * 0.6f - sm, 292.0f);
    draw_media_card(d, cx0 + cw * 0.8f + sm, cy0 + 100.0f + sm, cw * 0.2f - sm, 292.0f);
}

/* --- Media tab ------------------------------------------------------------ */

static void draw_media_tab(dc_dashboard *d, float w, float h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    const float cx0 = DC_DASH_PAD + DC_DASH_MARGIN;
    const float cw = w - 2.0f * (DC_DASH_PAD + DC_DASH_MARGIN);
    const float cy0 = DC_DASH_TABBAR_H + 12.0f;
    const float ch = h - cy0 - DC_DASH_MARGIN;
    draw_card(vg, cx0, cy0, cw, ch, tc(t->surface_container_high));

    const float cx = cx0 + cw / 2.0f;
    dc_mpris_info m;
    bool have = dc_mpris_read(&m) && m.active;
    if (!have) {
        dc_render_icon(d->render, DC_ICON_MUSIC_NOTE, cx, cy0 + ch / 2.0f - 24.0f, 56.0f,
                       t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, d->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, cx, cy0 + ch / 2.0f + 24.0f, "Nothing playing", NULL);
        return;
    }

    int art = ensure_art(d, m.art_url);
    const float art_r = 88.0f;
    draw_album_circle(d, cx, cy0 + 30.0f + art_r, art_r, art);

    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char title[256];
    snprintf(title, sizeof(title), "%s", m.title[0] ? m.title : "Unknown Track");
    nvgFontSize(vg, 20.0f);
    dash_ellipsize(vg, title, sizeof(title), cw - 60.0f);
    nvgFillColor(vg, tc(t->surface_text));
    float title_y = cy0 + 30.0f + art_r * 2.0f + 30.0f;
    nvgText(vg, cx, title_y, title, NULL);

    if (m.artist[0]) {
        char artist[128];
        snprintf(artist, sizeof(artist), "%s", m.artist);
        nvgFontSize(vg, 15.0f);
        dash_ellipsize(vg, artist, sizeof(artist), cw - 60.0f);
        nvgFillColor(vg, tc(t->primary));
        nvgText(vg, cx, title_y + 26.0f, artist, NULL);
    }

    /* Progress bar + times. */
    const float bar_x = cx0 + 40.0f;
    const float bar_w = cw - 80.0f;
    const float bar_y = title_y + 58.0f;
    float frac = m.length_us > 0 ? (float)m.position_us / (float)m.length_us : 0.0f;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bar_x, bar_y, bar_w, 4.0f, 2.0f);
    nvgFillColor(vg, tc_alpha(t->outline, 80));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bar_x, bar_y, bar_w * frac, 4.0f, 2.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, bar_x + bar_w * frac, bar_y + 2.0f, 5.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);

    char elapsed[16], total[16];
    fmt_time(m.position_us, elapsed, sizeof(elapsed));
    fmt_time(m.length_us, total, sizeof(total));
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, bar_x, bar_y + 18.0f, elapsed, NULL);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgText(vg, bar_x + bar_w, bar_y + 18.0f, total, NULL);

    draw_transport(d, cx, bar_y + 58.0f, m.playing, 24.0f, 14.0f);
}

/* --- Weather tab ---------------------------------------------------------- */

static void draw_weather_stat(dc_dashboard *d, float x, float y, int icon, const char *label,
                              const char *value)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    dc_render_icon(d->render, icon, x + 10.0f, y + 12.0f, 18.0f, t->primary,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, x + 26.0f, y + 5.0f, label, NULL);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x + 26.0f, y + 21.0f, value, NULL);
}

static void weekday_label(int offset, char *out, size_t sz)
{
    if (offset == 0) {
        snprintf(out, sz, "Today");
        return;
    }
    if (offset == 1) {
        snprintf(out, sz, "Tomorrow");
        return;
    }
    time_t now = time(NULL) + (time_t)offset * 86400;
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, sz, "%a", &tm);
}

static void draw_forecast_card(dc_dashboard *d, float x, float y, float w, float h, int offset,
                               const dc_weather_daily *day, bool today, const char *unit)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 10.0f);
    nvgFillColor(vg, today ? tc_alpha(t->primary, 40) : tc(t->surface_container_highest));
    nvgFill(vg);
    if (today) {
        nvgStrokeColor(vg, tc(t->primary));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }

    char label[16];
    weekday_label(offset, label, sizeof(label));
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, today ? tc(t->primary) : tc(t->surface_text));
    nvgText(vg, x + w / 2.0f, y + 20.0f, label, NULL);

    int icon = weather_codepoint(dc_weather_icon_name(day->weather_code, true));
    dc_render_icon(d->render, icon, x + w / 2.0f, y + h / 2.0f, 30.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char temps[24];
    snprintf(temps, sizeof(temps), "%d\xc2\xb0/%d\xc2\xb0", day->temp_min, day->temp_max);
    DC_UNUSED(unit);
    /* Reset the font face: dc_render_icon() above left the Material Symbols
     * icon font selected, in which "°"/"/" render as tofu. */
    nvgFontFaceId(vg, d->render->font_ui);
    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, x + w / 2.0f, y + h - 18.0f, temps, NULL);
}

/* "2 PM" / "14:00" depending on the clock format, matching draw_clock_card's
 * 12/24h handling. */
static void hour_label(int hour24, bool clock_24h, char *out, size_t sz)
{
    if (clock_24h) {
        snprintf(out, sz, "%02d:00", hour24);
        return;
    }
    int h12 = hour24 % 12;
    if (h12 == 0)
        h12 = 12;
    snprintf(out, sz, "%d %s", h12, hour24 < 12 ? "AM" : "PM");
}

/* One hourly-forecast card: hour label, icon, single temperature. Twin of
 * draw_forecast_card() but for dc_weather_hourly (no min/max, "Now" instead
 * of "Today" for the first entry). */
static void draw_hour_card(dc_dashboard *d, float x, float y, float w, float h, bool now,
                           const dc_weather_hourly *hr, bool clock_24h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 10.0f);
    nvgFillColor(vg, now ? tc_alpha(t->primary, 40) : tc(t->surface_container_highest));
    nvgFill(vg);
    if (now) {
        nvgStrokeColor(vg, tc(t->primary));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }

    char label[16];
    if (now)
        snprintf(label, sizeof(label), "Now");
    else
        hour_label(hr->hour, clock_24h, label, sizeof(label));
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, now ? tc(t->primary) : tc(t->surface_text));
    nvgText(vg, x + w / 2.0f, y + 18.0f, label, NULL);

    int icon = weather_codepoint(dc_weather_icon_name(hr->weather_code, true));
    dc_render_icon(d->render, icon, x + w / 2.0f, y + h / 2.0f + 4.0f, 26.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char temp[16];
    snprintf(temp, sizeof(temp), "%d\xc2\xb0", hr->temp_c);
    /* Reset the font face: dc_render_icon() above left the icon font
     * selected, in which "°" renders as tofu. */
    nvgFontFaceId(vg, d->render->font_ui);
    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, x + w / 2.0f, y + h - 16.0f, temp, NULL);
}

static void draw_weather_tab(dc_dashboard *d, float w, float h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    const char *unit = cfg->weather_fahrenheit ? "F" : "C";
    const float cx0 = DC_DASH_PAD + DC_DASH_MARGIN;
    const float cw = w - 2.0f * (DC_DASH_PAD + DC_DASH_MARGIN);
    const float cy0 = DC_DASH_TABBAR_H + 12.0f;

    dc_weather_state ws;
    bool have = dc_weather_get(&ws) && ws.valid;

    /* Current conditions card. */
    const float cur_h = 120.0f;
    draw_card(vg, cx0, cy0, cw, cur_h, tc(t->surface_container_high));
    if (!have) {
        nvgFontFaceId(vg, d->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, cx0 + 20.0f, cy0 + cur_h / 2.0f, "Weather unavailable", NULL);
        return;
    }

    int icon = weather_codepoint(dc_weather_icon_name(ws.weather_code, ws.is_day));
    dc_render_icon(d->render, icon, cx0 + 44.0f, cy0 + cur_h / 2.0f, 52.0f, t->primary,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char temp[16];
    snprintf(temp, sizeof(temp), "%d\xc2\xb0 %s", ws.temp_c, unit);
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 34.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, cx0 + 82.0f, cy0 + 34.0f, temp, NULL);
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, cx0 + 82.0f, cy0 + 62.0f, dc_weather_condition_name(ws.weather_code), NULL);
    char feels[32];
    snprintf(feels, sizeof(feels), "Feels Like %d\xc2\xb0", ws.feels_like);
    nvgFontSize(vg, 13.0f);
    nvgText(vg, cx0 + 82.0f, cy0 + 84.0f, feels, NULL);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, cx0 + 82.0f, cy0 + 102.0f, cfg->weather_location, NULL);

    /* Stats grid 3 cols x 2 rows on the right half of the current card. */
    const float grid_x = cx0 + cw * 0.44f;
    const float grid_w = cw * 0.56f - 16.0f;
    const float col_w = grid_w / 3.0f;
    const float row0 = cy0 + 26.0f;
    const float row1 = cy0 + 74.0f;
    char hum[16], wind[16], press[16], prec[16];
    snprintf(hum, sizeof(hum), "%d%%", ws.humidity);
    snprintf(wind, sizeof(wind), "%d km/h", ws.wind_kmh);
    snprintf(press, sizeof(press), "%d hPa", ws.pressure_hpa);
    snprintf(prec, sizeof(prec), "%d%%", ws.precip_prob);
    const char *sr = ws.daily_count > 0 ? ws.daily[0].sunrise : "";
    const char *ss = ws.daily_count > 0 ? ws.daily[0].sunset : "";
    draw_weather_stat(d, grid_x, row0, DC_ICON_HUMIDITY_LOW, "Humidity", hum);
    draw_weather_stat(d, grid_x + col_w, row0, DC_ICON_AIR, "Wind", wind);
    draw_weather_stat(d, grid_x + 2.0f * col_w, row0, DC_ICON_SPEED, "Pressure", press);
    draw_weather_stat(d, grid_x, row1, DC_ICON_RAINY, "Precipitation", prec);
    draw_weather_stat(d, grid_x + col_w, row1, DC_ICON_WB_TWILIGHT, "Sunrise", sr[0] ? sr : "--");
    draw_weather_stat(d, grid_x + 2.0f * col_w, row1, DC_ICON_BEDTIME, "Sunset", ss[0] ? ss : "--");

    /* Daily / Hourly pills (docs/13-POPOUTS-SPEC.md sec.5: DankFilterChips —
     * the active pill is filled primary, the inactive one a plain surface
     * chip). */
    const float pill_y = cy0 + cur_h + 16.0f;
    const float pill_h = 30.0f;
    const bool hourly = d->weather_hourly;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, cx0, pill_y, 72.0f, pill_h, pill_h / 2.0f);
    nvgFillColor(vg, hourly ? tc(t->surface_container_highest) : tc(t->primary));
    nvgFill(vg);
    nvgFontFaceId(vg, d->render->font_ui);
    nvgFontSize(vg, 13.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, hourly ? tc(t->surface_variant_text) : tc(t->primary_text));
    nvgText(vg, cx0 + 36.0f, pill_y + pill_h / 2.0f, "Daily", NULL);
    push_hit(d, cx0, pill_y, cx0 + 72.0f, pill_y + pill_h, HIT_WEATHER_DAILY, 0);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, cx0 + 80.0f, pill_y, 72.0f, pill_h, pill_h / 2.0f);
    nvgFillColor(vg, hourly ? tc(t->primary) : tc(t->surface_container_highest));
    nvgFill(vg);
    nvgFillColor(vg, hourly ? tc(t->primary_text) : tc(t->surface_variant_text));
    nvgText(vg, cx0 + 116.0f, pill_y + pill_h / 2.0f, "Hourly", NULL);
    push_hit(d, cx0 + 80.0f, pill_y, cx0 + 152.0f, pill_y + pill_h, HIT_WEATHER_HOURLY, 0);

    /* Forecast row: 7-day cards, or the next-24h hourly strip. */
    const float fc_y = pill_y + pill_h + 16.0f;
    const float fc_gap = 8.0f;
    const float fc_h = h - fc_y - DC_DASH_MARGIN;

    if (!hourly) {
        const float fc_w = (cw - 6.0f * fc_gap) / 7.0f;
        int n = ws.daily_count;
        for (int i = 0; i < 7 && i < n; i++) {
            float fx = cx0 + (fc_w + fc_gap) * (float)i;
            draw_forecast_card(d, fx, fc_y, fc_w, fc_h, i, &ws.daily[i], i == 0, unit);
        }
    } else if (ws.hourly_count == 0) {
        nvgFontFaceId(vg, d->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, cx0 + cw / 2.0f, fc_y + fc_h / 2.0f, "Hourly forecast unavailable", NULL);
    } else {
        /* Fit as many hourly cards as the row width comfortably allows
         * (DMS's "dense" hourly view shows ~10 on a same-width popout). */
        const int show_max = 8;
        int show = ws.hourly_count < show_max ? ws.hourly_count : show_max;
        const float hc_w = (cw - (float)(show - 1) * fc_gap) / (float)show;
        for (int i = 0; i < show; i++) {
            float fx = cx0 + (hc_w + fc_gap) * (float)i;
            draw_hour_card(d, fx, fc_y, hc_w, fc_h, i == 0, &ws.hourly[i], cfg->clock_24h);
        }
    }
}

/* --- Wallpapers tab (grid browser, docs/13-POPOUTS-SPEC.md sec.5) --------- */

/* stb_image (nanovg's vendored copy) has no webp decoder, so .webp is
 * deliberately excluded from the scan filter. */
static bool wall_ext_ok(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".bmp") == 0 ||
           strcasecmp(dot, ".gif") == 0;
}

/* Resolve the wallpaper directory: dirname of the configured wallpaper (like
 * DMS's WallpaperTab.loadWallpaperDirectory()), else ~/Pictures/wallpapers if
 * it exists, else ~/Pictures. */
static void wall_pick_dir(char *out, size_t sz)
{
    const dc_config *cfg = dc_config_current;
    if (cfg->wallpaper[0] && cfg->wallpaper[0] != '#') {
        snprintf(out, sz, "%s", cfg->wallpaper);
        char *slash = strrchr(out, '/');
        if (slash && slash != out) {
            *slash = '\0';
            return;
        }
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        snprintf(out, sz, "/usr/share/backgrounds");
        return;
    }
    snprintf(out, sz, "%s/Pictures/wallpapers", home);
    struct stat st;
    if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
        return;
    snprintf(out, sz, "%s/Pictures", home);
}

static int wall_cmp(const void *a, const void *b)
{
    return strcmp(((const dc_wall_entry *)a)->path, ((const dc_wall_entry *)b)->path);
}

/* Re-list d->wall_dir into d->walls (name-sorted, like DMS's default sort).
 * Called from the draw pass, so the GL context is current and stale thumbnail
 * images can be deleted safely. */
static void wall_rescan(dc_dashboard *d)
{
    for (int i = 0; i < d->wall_count; i++)
        if (d->walls[i].thumb_img > 0)
            nvgDeleteImage(d->render->vg, d->walls[i].thumb_img);
    d->wall_count = 0;
    d->wall_scroll = 0.0f;
    d->wall_scanned = true;

    DIR *dir = opendir(d->wall_dir);
    if (!dir)
        return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && d->wall_count < DC_WALL_MAX) {
        if (ent->d_name[0] == '.' || !wall_ext_ok(ent->d_name))
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", d->wall_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        dc_wall_entry *e = &d->walls[d->wall_count];
        memset(e, 0, sizeof(*e));
        if (snprintf(e->path, sizeof(e->path), "%.511s", full) >= (int)sizeof(e->path))
            continue; /* path too long to store faithfully: skip */
        d->wall_count++;
    }
    closedir(dir);
    qsort(d->walls, (size_t)d->wall_count, sizeof(d->walls[0]), wall_cmp);
    dc_debug("wallpapers: %d image(s) in %s", d->wall_count, d->wall_dir);
}

/* Decode e->path and box-sample it down to a <=DC_WALL_THUMB_MAX thumbnail
 * nvg image. One full-size decode per call — the caller budgets these. */
static void wall_decode(dc_dashboard *d, dc_wall_entry *e)
{
    e->tried = true;
    int sw = 0, sh = 0, n = 0;
    unsigned char *src = stbi_load(e->path, &sw, &sh, &n, 4);
    if (!src || sw <= 0 || sh <= 0) {
        if (src)
            stbi_image_free(src);
        return;
    }

    int tw = sw, th = sh;
    if (tw > DC_WALL_THUMB_MAX || th > DC_WALL_THUMB_MAX) {
        float s = (float)DC_WALL_THUMB_MAX / (float)(sw > sh ? sw : sh);
        tw = (int)((float)sw * s);
        th = (int)((float)sh * s);
        if (tw < 1)
            tw = 1;
        if (th < 1)
            th = 1;
    }

    unsigned char *dst = malloc((size_t)tw * (size_t)th * 4);
    if (dst) {
        for (int y = 0; y < th; y++) {
            int sy0 = (int)((long)y * sh / th);
            int sy1 = (int)((long)(y + 1) * sh / th);
            if (sy1 <= sy0)
                sy1 = sy0 + 1;
            for (int x = 0; x < tw; x++) {
                int sx0 = (int)((long)x * sw / tw);
                int sx1 = (int)((long)(x + 1) * sw / tw);
                if (sx1 <= sx0)
                    sx1 = sx0 + 1;
                unsigned int acc[4] = {0, 0, 0, 0};
                int cnt = 0;
                for (int yy = sy0; yy < sy1; yy++) {
                    const unsigned char *row = src + ((size_t)yy * (size_t)sw + (size_t)sx0) * 4;
                    for (int xx = sx0; xx < sx1; xx++, row += 4) {
                        acc[0] += row[0];
                        acc[1] += row[1];
                        acc[2] += row[2];
                        acc[3] += row[3];
                        cnt++;
                    }
                }
                unsigned char *out = dst + ((size_t)y * (size_t)tw + (size_t)x) * 4;
                out[0] = (unsigned char)(acc[0] / (unsigned int)cnt);
                out[1] = (unsigned char)(acc[1] / (unsigned int)cnt);
                out[2] = (unsigned char)(acc[2] / (unsigned int)cnt);
                out[3] = (unsigned char)(acc[3] / (unsigned int)cnt);
            }
        }
        e->thumb_img = nvgCreateImageRGBA(d->render->vg, tw, th, 0, dst);
        e->thumb_w = tw;
        e->thumb_h = th;
        free(dst);
    }
    stbi_image_free(src);
}

/* Click on a thumbnail: persist config `wallpaper`, re-derive the palette when
 * dynamicColor is on (dc_config_reapply -> apply_theme -> dc_dynamic_from_
 * image), repaint live surfaces, and set the compositor wallpaper.
 * DANKC_WALL_DRY=1 keeps it in-memory only (no config.json write, no swaybg)
 * for UI verification against a real config. */
static void wall_set_active(dc_dashboard *d, const dc_wall_entry *e)
{
    DC_UNUSED(d);
    dc_config *cfg = dc_config_mut();
    if (strcmp(cfg->wallpaper, e->path) == 0)
        return;
    snprintf(cfg->wallpaper, sizeof(cfg->wallpaper), "%s", e->path);
    dc_config_reapply(); /* stock theme + dynamic-color overlay when enabled */
    dc_material_bg_invalidate(); /* new wallpaper -> panels' blurred bg regenerates lazily */
    if (!getenv("DANKC_WALL_DRY")) {
        dc_config_save();
        dc_wallpaper_apply(e->path);
    }
    dc_config_notify_changed(); /* bars pick up the (possibly) new palette */
    dc_info("wallpaper set: %s%s", e->path, getenv("DANKC_WALL_DRY") ? " (dry)" : "");
}

static void draw_wallpapers_tab(dc_dashboard *d, float w, float h)
{
    NVGcontext *vg = d->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    const float cx0 = DC_DASH_PAD + DC_DASH_MARGIN;
    const float cw = w - 2.0f * (DC_DASH_PAD + DC_DASH_MARGIN);
    const float list_y0 = DC_DASH_TABBAR_H + 12.0f;
    const float footer_h = 26.0f;
    const float list_h = h - list_y0 - DC_DASH_MARGIN - footer_h;
    const float list_y1 = list_y0 + list_h;

    /* (Re)scan when the resolved directory changed (incl. the first draw). */
    char dir[512];
    wall_pick_dir(dir, sizeof(dir));
    if (!d->wall_scanned || strcmp(dir, d->wall_dir) != 0) {
        snprintf(d->wall_dir, sizeof(d->wall_dir), "%s", dir);
        wall_rescan(d);
    }

    d->wall_pending = false;

    if (d->wall_count == 0) {
        const float ecx = w / 2.0f;
        const float ecy = list_y0 + list_h * 0.42f;
        dc_render_icon(d->render, DC_ICON_WALLPAPER, ecx, ecy - 34.0f, 40.0f,
                       t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, d->render->font_ui);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, 16.0f);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, ecx, ecy + 8.0f, "No wallpapers found", NULL);
        nvgFontSize(vg, 13.0f);
        char sub[560];
        snprintf(sub, sizeof(sub), "No images in %s", d->wall_dir);
        dash_ellipsize(vg, sub, sizeof(sub), cw - 40.0f);
        nvgText(vg, ecx, ecy + 34.0f, sub, NULL);
        return;
    }

    /* 4-column grid, DMS WallpaperTab.qml's cell shape (wider than tall). */
    const float gap = 8.0f;
    const int cols = 4;
    const float cell_w = (cw - (float)(cols - 1) * gap) / (float)cols;
    const float cell_h = cell_w * 0.60f;
    const int rows = (d->wall_count + cols - 1) / cols;
    const float content_h = (float)rows * (cell_h + gap) - gap;
    d->wall_scroll_max = content_h > list_h ? content_h - list_h : 0.0f;
    if (d->wall_scroll < 0.0f)
        d->wall_scroll = 0.0f;
    if (d->wall_scroll > d->wall_scroll_max)
        d->wall_scroll = d->wall_scroll_max;

    int budget = DC_WALL_DECODE_BUDGET;

    nvgSave(vg);
    nvgScissor(vg, cx0, list_y0, cw, list_h);
    for (int i = 0; i < d->wall_count; i++) {
        const float x = cx0 + (float)(i % cols) * (cell_w + gap);
        const float y = list_y0 + (float)(i / cols) * (cell_h + gap) - d->wall_scroll;
        if (y + cell_h < list_y0 || y > list_y1)
            continue; /* off-screen: don't draw, don't decode */

        dc_wall_entry *e = &d->walls[i];
        if (!e->tried) {
            if (budget > 0) {
                wall_decode(d, e);
                budget--;
            } else {
                d->wall_pending = true; /* keep frame-stepping until decoded */
            }
        }

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, cell_w, cell_h, 10.0f);
        nvgFillColor(vg, tc(t->surface_container_highest));
        nvgFill(vg);

        if (e->thumb_img > 0) {
            /* Aspect-cover the cell with the thumbnail. */
            float sx = cell_w / (float)e->thumb_w;
            float sy = cell_h / (float)e->thumb_h;
            float s = sx > sy ? sx : sy;
            float iw = (float)e->thumb_w * s;
            float ih = (float)e->thumb_h * s;
            NVGpaint pat = nvgImagePattern(vg, x + (cell_w - iw) / 2.0f, y + (cell_h - ih) / 2.0f,
                                           iw, ih, 0.0f, e->thumb_img, 1.0f);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, cell_w, cell_h, 10.0f);
            nvgFillPaint(vg, pat);
            nvgFill(vg);
        } else {
            /* Placeholder while pending (or after a failed decode). */
            dc_color dim = t->surface_variant_text;
            dim.a = e->tried ? 70 : 140;
            dc_render_icon(d->render, DC_ICON_WALLPAPER, x + cell_w / 2.0f, y + cell_h / 2.0f,
                           24.0f, dim, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }

        /* Highlight the currently active wallpaper (DMS: 3px primary ring). */
        if (cfg->wallpaper[0] && strcmp(cfg->wallpaper, e->path) == 0) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + 1.5f, y + 1.5f, cell_w - 3.0f, cell_h - 3.0f, 9.0f);
            nvgStrokeColor(vg, tc(t->primary));
            nvgStrokeWidth(vg, 3.0f);
            nvgStroke(vg);
        }

        /* Hit target clipped to the visible list area. */
        float hy0 = y < list_y0 ? list_y0 : y;
        float hy1 = y + cell_h > list_y1 ? list_y1 : y + cell_h;
        if (hy1 > hy0)
            push_hit(d, x, hy0, x + cell_w, hy1, HIT_WALLPAPER_ITEM, i);
    }
    nvgRestore(vg);

    /* Scrollbar thumb (processes-popout pattern). */
    if (d->wall_scroll_max > 0.0f) {
        float track_x = cx0 + cw - 3.0f;
        float thumb_h = list_h * (list_h / content_h);
        if (thumb_h < 24.0f)
            thumb_h = 24.0f;
        float thumb_y = list_y0 + (list_h - thumb_h) * (d->wall_scroll / d->wall_scroll_max);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, track_x, thumb_y, 3.0f, thumb_h, 1.5f);
        nvgFillColor(vg, tc_alpha(t->outline, 140));
        nvgFill(vg);
    }

    /* Footer: count + directory, centered (DMS footer's page indicator slot). */
    char foot[600];
    snprintf(foot, sizeof(foot), "%d wallpaper%s  \xc2\xb7  %s", d->wall_count,
             d->wall_count == 1 ? "" : "s", d->wall_dir);
    nvgFontFaceId(vg, d->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 12.0f);
    dash_ellipsize(vg, foot, sizeof(foot), cw - 20.0f);
    nvgFillColor(vg, tc_alpha(t->surface_variant_text, 180));
    nvgText(vg, w / 2.0f, list_y1 + footer_h / 2.0f + 2.0f, foot, NULL);
}

/* --- frame ---------------------------------------------------------------- */

static void recompute_physical(dc_dashboard *d)
{
    d->phys_width = (d->logical_width * d->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    d->phys_height = (d->logical_height * d->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void dash_render(dc_dashboard *d)
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
    const float w = d->logical_width;
    const float h = d->logical_height;
    const float pad = DC_DASH_PAD;

    d->hit_count = 0;

    glViewport(0, 0, d->phys_width, d->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)d->scale120 / DC_SCALE_BASE);

    float p = dc_anim_progress(&d->anim);
    if (d->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.96f + 0.04f * p;
    float ox = pad + (w - 2.0f * pad) * d->anim_ox;
    float oy = pad + (h - 2.0f * pad) * d->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Drop shadow. */
    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 12.0f, 22.0f,
                                     nvgRGBA(0, 0, 0, 100), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    /* Card: blurred+dimmed wallpaper ("material" bg) when enabled, else the
     * flat surfaceContainer fill (docs/POLISH.md P2, ui/material_bg.c). */
    dc_material_bg_fill_card(vg, d->render, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    draw_tabbar(d, w);

    switch (d->tab) {
    case DC_DASH_OVERVIEW:
        draw_overview(d, w);
        break;
    case DC_DASH_MEDIA:
        draw_media_tab(d, w, h);
        break;
    case DC_DASH_WEATHER:
        draw_weather_tab(d, w, h);
        break;
    case DC_DASH_WALLPAPERS:
        draw_wallpapers_tab(d, w, h);
        break;
    case DC_DASH_SETTINGS:
        break; /* action tab: never a rendered page */
    }

    nvgEndFrame(vg);

    if ((dc_anim_active(&d->anim) || d->closing || d->wall_pending) && !d->frame_cb) {
        d->frame_cb = wl_surface_frame(d->surface);
        wl_callback_add_listener(d->frame_cb, &dash_frame_listener, d);
    }
    dc_egl_swap(d->egl, &d->egl_window);
}

/* --- surface lifecycle ---------------------------------------------------- */

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_dashboard *d = data;
    DC_UNUSED(fs);
    d->scale120 = (int)scale;
    recompute_physical(d);
    dash_render(d);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_dashboard *d = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    d->logical_width = width > 0 ? (int)width : DC_DASH_WIDTH;
    d->logical_height = height > 0 ? (int)height : DC_DASH_HEIGHT;
    d->configured = true;
    recompute_physical(d);
    dash_render(d);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_dashboard *d = data;
    DC_UNUSED(surface);
    d->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_dashboard *dc_dashboard_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_dashboard *d = calloc(1, sizeof(*d));
    d->wl = wl;
    d->egl = egl;
    d->render = render;
    d->logical_width = DC_DASH_WIDTH;
    d->logical_height = DC_DASH_HEIGHT;
    d->scale120 = DC_SCALE_BASE;
    return d;
}

static void dash_show(dc_dashboard *d, dc_output *output, dc_dash_tab tab)
{
    d->output = output;
    d->configured = false;
    d->egl_ready = false;
    d->tab = tab;
    d->cal_month_offset = 0;
    d->weather_hourly = false; /* DMS WeatherTab defaults to Daily per open */
    d->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    dc_anim_start(&d->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    d->surface = wl_compositor_create_surface(d->wl->compositor);
    if (d->wl->fractional_scale_mgr) {
        d->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            d->wl->fractional_scale_mgr, d->surface);
        wp_fractional_scale_v1_add_listener(d->fractional_scale, &fractional_scale_listener, d);
    }
    if (d->wl->viewporter)
        d->viewport = wp_viewporter_get_viewport(d->wl->viewporter, d->surface);

    d->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        d->wl->layer_shell, d->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:dashboard");

    /* Bar-adjacent, center-aligned (docs/13-POPOUTS-SPEC.md sec.0/5: the
     * dashboard opens centered above the bar). */
    dc_popout_anchor pa = dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_CENTER, 0);
    d->anim_ox = pa.origin_x;
    d->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(d->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(d->layer_surface, DC_DASH_WIDTH, DC_DASH_HEIGHT);
    zwlr_layer_surface_v1_set_margin(d->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_exclusive_zone(d->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(d->layer_surface, &layer_surface_listener, d);

    wl_surface_commit(d->surface);
    d->visible = true;
    d->closing = false;
    dc_debug("dashboard shown (tab %d)", (int)tab);
}

static void dash_teardown(dc_dashboard *d)
{
    if (d->frame_cb) {
        wl_callback_destroy(d->frame_cb);
        d->frame_cb = NULL;
    }
    if (d->art_img > 0 && d->render && d->render->vg) {
        /* Needs a current GL context; make one if possible. */
        if (d->egl_ready && dc_egl_make_current(d->egl, &d->egl_window))
            nvgDeleteImage(d->render->vg, d->art_img);
        d->art_img = 0;
    }
    d->art_url[0] = '\0';
    d->art_path[0] = '\0';
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
    d->visible = false;
    d->closing = false;
    dc_debug("dashboard hidden");
}

static void dash_begin_close(dc_dashboard *d)
{
    if (!d->visible || d->closing)
        return;
    dc_anim_start(&d->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    d->closing = true;
    if (!dc_anim_active(&d->anim)) {
        dash_teardown(d);
        return;
    }
    dash_render(d);
}

void dc_dashboard_toggle(dc_dashboard *d, dc_output *output, dc_dash_tab tab)
{
    if (d->visible && !d->closing) {
        if (d->tab == tab) {
            dash_begin_close(d);
        } else {
            d->tab = tab;
            d->cal_month_offset = 0;
            dash_render(d);
        }
    } else {
        dash_show(d, output, tab);
    }
}

void dc_dashboard_hide(dc_dashboard *d)
{
    dash_begin_close(d);
}

bool dc_dashboard_visible(dc_dashboard *d)
{
    return d->visible;
}

struct wl_surface *dc_dashboard_surface(dc_dashboard *d)
{
    return d->surface;
}

void dc_dashboard_refresh(dc_dashboard *d)
{
    if (d->visible && !d->closing && !dc_anim_active(&d->anim))
        dash_render(d);
}

void dc_dashboard_set_settings_cb(dc_dashboard *d, dc_dashboard_action_cb cb, void *user)
{
    d->settings_cb = cb;
    d->settings_user = user;
}

void dc_dashboard_handle_click(dc_dashboard *d, double x, double y)
{
    if (!d->visible || d->closing)
        return;

    for (int i = d->hit_count - 1; i >= 0; i--) {
        const dash_hit *hit = &d->hits[i];
        if (x < hit->x0 || x > hit->x1 || y < hit->y0 || y > hit->y1)
            continue;
        switch (hit->kind) {
        case HIT_TAB:
            if ((dc_dash_tab)hit->payload == DC_DASH_SETTINGS) {
                dc_dashboard_action_cb cb = d->settings_cb;
                void *user = d->settings_user;
                dash_begin_close(d);
                if (cb)
                    cb(user);
            } else if ((dc_dash_tab)hit->payload != d->tab) {
                d->tab = (dc_dash_tab)hit->payload;
                d->cal_month_offset = 0;
                dash_render(d);
            }
            return;
        case HIT_CAL_PREV:
            d->cal_month_offset--;
            dash_render(d);
            return;
        case HIT_CAL_NEXT:
            d->cal_month_offset++;
            dash_render(d);
            return;
        case HIT_MEDIA_PREV:
            dc_mpris_previous();
            dash_render(d);
            return;
        case HIT_MEDIA_PLAY:
            dc_mpris_play_pause();
            dash_render(d);
            return;
        case HIT_MEDIA_NEXT:
            dc_mpris_next();
            dash_render(d);
            return;
        case HIT_WEATHER_DAILY:
            if (d->weather_hourly) {
                d->weather_hourly = false;
                dash_render(d);
            }
            return;
        case HIT_WEATHER_HOURLY:
            if (!d->weather_hourly) {
                d->weather_hourly = true;
                dash_render(d);
            }
            return;
        case HIT_WALLPAPER_ITEM:
            if (hit->payload >= 0 && hit->payload < d->wall_count) {
                wall_set_active(d, &d->walls[hit->payload]);
                dash_render(d);
            }
            return;
        case HIT_NONE:
            return;
        }
    }
}

void dc_dashboard_handle_scroll(dc_dashboard *d, int steps_v)
{
    if (!d->visible || d->closing || d->tab != DC_DASH_WALLPAPERS)
        return;
    float s = d->wall_scroll + (float)steps_v * 48.0f;
    if (s < 0.0f)
        s = 0.0f;
    if (s > d->wall_scroll_max)
        s = d->wall_scroll_max;
    if (s == d->wall_scroll)
        return;
    d->wall_scroll = s;
    dash_render(d);
}

void dc_dashboard_destroy(dc_dashboard *d)
{
    if (!d)
        return;
    if (d->visible)
        dash_teardown(d);
    /* Thumbnail nvg images are intentionally not deleted here: destroy runs
     * at process exit only (see main.c), where GL teardown is skipped. */
    free(d);
}
