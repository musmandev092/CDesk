#include "ui/controlcenter.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/audio.h"
#include "services/bluez.h"
#include "services/net.h"
#include "theme/theme.h"
#include "ui/bar/bar_tokens.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Width/height picked to match the user's live DMS reference screenshot
 * (~/Pictures/Screenshots/Screenshot from 2026-07-02 14-17-26.png, itself
 * ~541x350 for the popout alone) and DMS's ControlCenterPopout.qml
 * `popupWidth: 550` -- dankc's other popouts (launcher 600, clip_picker 640,
 * settings 520) already use QML popupWidth-ish numbers directly as logical
 * px with no extra conversion factor, so this follows the same convention
 * rather than the old 380x420 (sized for the previous 2x2-tiles layout). */
#define DC_CC_WIDTH 480
#define DC_CC_HEIGHT 372
#define DC_SCALE_BASE 120
/* Inset from the screen's right edge when bar-adjacent (docs/13-POPOUTS-SPEC.md
 * sec.0/1: opens near the bar's right cluster, a few px in from the edge). */
#define DC_CC_SIDE_MARGIN 12

struct dc_control_center {
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

    /* Entrance/exit scale-and-fade pivot, bar-position-aware (docs/13-POPOUTS-
     * SPEC.md sec.0): fraction of (w,h) nearest the bar-facing edge, set from
     * dc_popout_bar_adjacent() at show-time. */
    float anim_ox, anim_oy;

    bool visible;
    bool configured;
    bool egl_ready;
};

static void cc_render(dc_control_center *cc);
static void cc_teardown(dc_control_center *cc);

static void cc_frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener cc_frame_listener = {.done = cc_frame_done};

static void cc_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_control_center *cc = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    cc->frame_cb = NULL;
    if (!cc->visible)
        return;
    if (dc_anim_active(&cc->anim))
        cc_render(cc);
    else if (cc->closing)
        cc_teardown(cc);
}

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

/* Shared layout so cc_render (draw) and handle_click (hit-test) agree.
 * Matches docs/13-POPOUTS-SPEC.md sec.1: user header card, two side-by-side
 * sliders, then a 2-column x 3-row tile grid (wifi/bluetooth, audioOutput/
 * audioInput, nightMode/darkMode). */
typedef struct {
    float ix, iw;

    float header_y, header_h;
    float avatar_cx, avatar_cy, avatar_r;
    float btn_cx[4]; /* lock, power, settings, edit */
    float btn_cy, btn_r;

    float sliders_y, slider_h;
    float slot_x[2]; /* volume, brightness */
    float slot_w;

    float tiles_y0, tile_w, tile_h, gap;
} cc_layout;

static cc_layout cc_get_layout(float w)
{
    const float pad = 6.0f;   /* room for the drop shadow */
    const float margin = 16.0f; /* content inset from the card edge (~Theme.spacingL) */
    const float gap = 8.0f;     /* ~Theme.spacingS, used between every stacked row */

    cc_layout l;
    l.ix = pad + margin;
    l.iw = w - 2.0f * l.ix;

    l.header_y = pad + margin;
    l.header_h = 70.0f;
    l.avatar_r = 30.0f;
    l.avatar_cx = l.ix + 16.0f + l.avatar_r;
    l.avatar_cy = l.header_y + l.header_h / 2.0f;
    l.btn_cy = l.avatar_cy;
    l.btn_r = 16.0f;
    /* lock/power/settings/edit, right-aligned, 40px apart center-to-center. */
    l.btn_cx[3] = l.ix + l.iw - 16.0f;
    l.btn_cx[2] = l.btn_cx[3] - 40.0f;
    l.btn_cx[1] = l.btn_cx[2] - 40.0f;
    l.btn_cx[0] = l.btn_cx[1] - 40.0f;

    l.sliders_y = l.header_y + l.header_h + gap;
    l.slider_h = 40.0f;
    l.slot_w = (l.iw - gap) / 2.0f;
    l.slot_x[0] = l.ix;
    l.slot_x[1] = l.ix + l.slot_w + gap;

    l.tiles_y0 = l.sliders_y + l.slider_h + gap;
    l.gap = gap;
    l.tile_w = (l.iw - gap) / 2.0f;
    l.tile_h = 60.0f;
    return l;
}

static float cc_tile_x(const cc_layout *l, int col)
{
    return l->ix + (float)col * (l->tile_w + l->gap);
}

static float cc_tile_y(const cc_layout *l, int row)
{
    return l->tiles_y0 + (float)row * (l->tile_h + l->gap);
}

/* Run a shell command detached (children auto-reaped via SIG_IGN on SIGCHLD). */
static void run_detached(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* Invoke `dankc ctl <subcmd>` against the running instance's control socket
 * (the same path `dankc keybinds`' niri snippet and dankctl use) so the
 * header's lock/settings buttons reuse the existing lock-screen/settings-panel
 * mechanisms instead of reimplementing them here (docs/13-POPOUTS-SPEC.md
 * sec.1). Resolved via /proc/self/exe rather than relying on `dankc` being on
 * PATH, since dev.sh/tests often run ./bin/dankc directly. */
static void run_self_ctl(const char *subcmd)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    const char *path = "dankc";
    if (n > 0) {
        exe[n] = '\0';
        path = exe;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(path, path, "ctl", subcmd, (char *)NULL);
        _exit(127);
    }
}

/* Current backlight brightness as 0..1, or -1 if none. */
static float read_brightness(void)
{
    DIR *dir = opendir("/sys/class/backlight");
    if (!dir)
        return -1.0f;
    struct dirent *ent;
    float value = -1.0f;
    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.')
            continue;
        char path[300];
        int cur = -1, max = -1;
        snprintf(path, sizeof(path), "/sys/class/backlight/%.200s/brightness", ent->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d", &cur) != 1)
                cur = -1;
            fclose(f);
        }
        snprintf(path, sizeof(path), "/sys/class/backlight/%.200s/max_brightness", ent->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d", &max) != 1)
                max = -1;
            fclose(f);
        }
        if (cur >= 0 && max > 0) {
            value = (float)cur / (float)max;
            break;
        }
    }
    closedir(dir);
    return value;
}

/* Username + "up XhYm" subtitle (HeaderPane.qml: UserInfoService.username +
 * "up " + DgopService.uptime, falling back to "Unknown" when uptime isn't
 * available). dankc has no dgop dependency, so this reads /proc/uptime
 * directly -- mirrors the QML's *logic* (real uptime when available, else
 * "Unknown") even though the reference screenshot shows "Unknown" (the
 * user's DMS session didn't have the optional `dgop` helper installed). */
static void get_user_info(char *user, size_t user_sz, char *sub, size_t sub_sz)
{
    struct passwd *pw = getpwuid(getuid());
    const char *name = (pw && pw->pw_name && pw->pw_name[0]) ? pw->pw_name : "";
    snprintf(user, user_sz, "%s", name[0] ? name : "User");

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
            snprintf(sub, sub_sz, "up %dh %dm", hours, mins);
        else
            snprintf(sub, sub_sz, "up %dm", mins);
    } else {
        snprintf(sub, sub_sz, "Unknown");
    }
}

/* ~/.face into a nanovg image, if present. dc_render_load_icon() (render/nvg.c)
 * already wraps stb_image (PNG, and JPEG since stb_image decodes both) for
 * launcher.c's app icons, so this reuses it rather than adding a dedicated
 * image-decode dependency -- loaded/freed per-frame like launcher.c does for
 * app icons (cheap relative to the GL work already happening per frame). */
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

/* Truncate `buf` on a UTF-8 codepoint boundary and append an ellipsis until it
 * fits within `max_w` px at the vg context's current font -- a local
 * equivalent of bar.c's bar_ellipsize(), generalized to an arbitrary buffer
 * size since it's used for device names/SSIDs here rather than one fixed
 * title buffer. No-op if `buf` already fits. */
static void cc_ellipsize(NVGcontext *vg, char *buf, size_t bufsize, float max_w)
{
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, buf, NULL, bounds);
    if (bounds[2] - bounds[0] <= max_w)
        return;

    size_t len = strlen(buf);
    if (bufsize < 4)
        return;
    char tmp[96];
    if (bufsize > sizeof(tmp))
        bufsize = sizeof(tmp);
    if (len > bufsize - 4)
        len = bufsize - 4; /* leave room for the 3-byte ellipsis + NUL */

    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80)
            len--; /* don't split a multi-byte UTF-8 codepoint */
        snprintf(tmp, bufsize, "%.*s\xe2\x80\xa6", (int)len, buf);
        nvgTextBounds(vg, 0.0f, 0.0f, tmp, NULL, bounds);
        if (bounds[2] - bounds[0] <= max_w || len == 0) {
            memcpy(buf, tmp, bufsize);
            return;
        }
    }
    snprintf(buf, bufsize, "\xe2\x80\xa6");
}

/* Mirrors services/audio.c's dc_audio_read() for the default *source* (mic):
 * same wpctl tool, same "Volume: %f [MUTED]" parsing, same per-second cache.
 * Kept local (rather than extending audio.h, out of this task's touch-scope)
 * since audio.h's dc_audio_read() only ever targets @DEFAULT_AUDIO_SINK@. */
static bool audio_source_read(dc_audio_info *out)
{
    static dc_audio_info cache;
    static bool cache_ok;
    static time_t cache_time;

    time_t now = time(NULL);
    if (cache_time == now) {
        *out = cache;
        return cache_ok;
    }

    out->available = false;
    out->volume = 0;
    out->muted = false;

    FILE *pipe = popen("wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null", "r");
    if (!pipe)
        return false;

    char line[128];
    bool ok = false;
    if (fgets(line, sizeof(line), pipe)) {
        float volume = 0.0f;
        if (sscanf(line, "Volume: %f", &volume) == 1) {
            out->volume = (int)(volume * 100.0f + 0.5f);
            out->available = true;
            ok = true;
        }
        if (strstr(line, "MUTED"))
            out->muted = true;
    }
    pclose(pipe);

    cache = *out;
    cache_ok = ok;
    cache_time = now;
    return ok;
}

/* The default sink/source's human-readable device name (e.g. "Built-in Audio
 * Analog Stereo"), parsed from `wpctl status`'s starred Sinks:/Sources: line
 * -- audio.h has no device-name field (out of touch-scope to add one), and
 * this reuses the same wpctl binary/popen pattern as dc_audio_read() rather
 * than introducing a new IPC mechanism. Cached per-second like the reads
 * above (this can be called once per render frame during the entrance
 * animation). */
static void read_audio_device_names(char *sink_name, size_t sink_sz, char *source_name,
                                     size_t source_sz)
{
    static char cached_sink[64];
    static char cached_source[64];
    static time_t cache_time;

    time_t now = time(NULL);
    if (cache_time != now) {
        cache_time = now;
        cached_sink[0] = '\0';
        cached_source[0] = '\0';

        FILE *pipe = popen("wpctl status 2>/dev/null", "r");
        if (pipe) {
            char line[256];
            int section = 0; /* 0=other, 1=sinks, 2=sources */
            while (fgets(line, sizeof(line), pipe)) {
                if (strstr(line, "Video")) /* the Audio block always comes first */
                    break;
                if (strstr(line, "Sinks:")) {
                    section = 1;
                    continue;
                }
                if (strstr(line, "Sources:")) {
                    section = 2;
                    continue;
                }
                if (strstr(line, "Filters:") || strstr(line, "Streams:") ||
                    strstr(line, "Devices:")) {
                    section = 0;
                    continue;
                }
                if (section == 0)
                    continue;
                char *star = strchr(line, '*');
                if (!star)
                    continue;
                char *dot = strchr(star, '.');
                if (!dot)
                    continue;
                char *name = dot + 1;
                while (*name == ' ')
                    name++;
                char *bracket = strchr(name, '[');
                size_t len = bracket ? (size_t)(bracket - name) : strlen(name);
                while (len > 0 &&
                       (name[len - 1] == ' ' || name[len - 1] == '\n' || name[len - 1] == '\r'))
                    len--;
                char *dst = section == 1 ? cached_sink : cached_source;
                size_t dst_sz = section == 1 ? sizeof(cached_sink) : sizeof(cached_source);
                if (len >= dst_sz)
                    len = dst_sz - 1;
                memcpy(dst, name, len);
                dst[len] = '\0';
            }
            pclose(pipe);
        }
    }

    snprintf(sink_name, sink_sz, "%s", cached_sink);
    snprintf(source_name, source_sz, "%s", cached_source);
}

/* CompoundPill-style tile (Widgets/CompoundPill.qml): pill background stays
 * constant, only the icon chip fills solid-primary when active; two stacked
 * text lines (title/subtitle) to the right. Used for wifi/bluetooth/
 * audioOutput/audioInput. */
static void draw_pill_tile(dc_render *r, float x, float y, float w, float h, int icon,
                           const char *title, const char *subtitle, bool active)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    const float chip_pad = 8.0f;
    const float chip = h - 2.0f * chip_pad;
    const float chip_x = x + chip_pad;
    const float chip_y = y + chip_pad;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, chip_x, chip_y, chip, chip, 10.0f);
    nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_container_highest));
    nvgFill(vg);
    dc_render_icon(r, icon, chip_x + chip / 2.0f, chip_y + chip / 2.0f, 20.0f,
                   active ? t->primary_text : t->primary, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    const float text_x = chip_x + chip + 12.0f;
    const float text_w = (x + w - 12.0f) - text_x;

    char title_buf[64];
    snprintf(title_buf, sizeof(title_buf), "%s", title);
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 14.0f);
    cc_ellipsize(vg, title_buf, sizeof(title_buf), text_w);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, text_x, y + h / 2.0f - 9.0f, title_buf, NULL);

    if (subtitle && subtitle[0]) {
        char sub_buf[64];
        snprintf(sub_buf, sizeof(sub_buf), "%s", subtitle);
        nvgFontSize(vg, DC_BAR_TEXT_SIZE);
        cc_ellipsize(vg, sub_buf, sizeof(sub_buf), text_w);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, text_x, y + h / 2.0f + 9.0f, sub_buf, NULL);
    }
}

/* ToggleButton-style tile (Widgets/ToggleButton.qml): the whole tile fills
 * solid-primary when active (dark text/icon on the light-green fill), a
 * single icon+label line when inactive. NOTE the inactive icon and label use
 * *different* colors (Theme.ccTileInactiveIcon=primary vs
 * Theme.surfaceText) -- matches the QML exactly, not a typo. Used for
 * nightMode/darkMode. */
static void draw_toggle_tile(dc_render *r, float x, float y, float w, float h, int icon,
                             const char *label, bool active)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_container_high));
    nvgFill(vg);

    dc_color icon_fg = active ? t->primary_text : t->primary;
    dc_color text_fg = active ? t->primary_text : t->surface_text;

    dc_render_icon(r, icon, x + 18.0f, y + h / 2.0f, 20.0f, icon_fg,
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, tc(text_fg));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + 48.0f, y + h / 2.0f, label, NULL);
}

/* A horizontal slider: icon + rounded track + primary fill (docs/13-POPOUTS-
 * SPEC.md sec.1: "green fill, rounded, ~12px tall track"). */
static void draw_slider(dc_render *r, float x, float cy, float w, int icon, float value)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;

    dc_render_icon(r, icon, x, cy, 20.0f, t->surface_text, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    const float tx = x + 32.0f;
    const float tw = w - 32.0f;
    const float th = 12.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, tx, cy - th / 2.0f, tw, th, th / 2.0f);
    nvgFillColor(vg, tc_alpha(t->outline, 90));
    nvgFill(vg);

    float fw = tw * value;
    if (fw < th)
        fw = th;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, tx, cy - th / 2.0f, fw, th, th / 2.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);
}

static void recompute_physical(dc_control_center *cc)
{
    cc->phys_width = (cc->logical_width * cc->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    cc->phys_height = (cc->logical_height * cc->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void cc_render(dc_control_center *cc)
{
    if (!cc->configured || cc->phys_width <= 0)
        return;

    if (!cc->egl_ready) {
        if (!dc_egl_window_init(&cc->egl_window, cc->egl, cc->surface, cc->phys_width,
                                cc->phys_height))
            return;
        cc->egl_ready = true;
    } else {
        dc_egl_window_resize(&cc->egl_window, cc->phys_width, cc->phys_height);
    }

    if (!dc_egl_make_current(cc->egl, &cc->egl_window))
        return;
    if (!dc_render_ensure(cc->render))
        return;

    if (cc->viewport)
        wp_viewport_set_destination(cc->viewport, cc->logical_width, cc->logical_height);

    NVGcontext *vg = cc->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = cc->logical_width;
    const float h = cc->logical_height;
    const float pad = 6.0f; /* room for the drop shadow */

    glViewport(0, 0, cc->phys_width, cc->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f); /* transparent -> rounded card over wallpaper */
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)cc->scale120 / DC_SCALE_BASE);

    /* Entrance/exit: fade + scale from the bar-facing edge (docs/13-POPOUTS-
     * SPEC.md sec.0) — pivot set by dc_popout_bar_adjacent() in cc_show().
     * Closing runs the progress in reverse. */
    float p = dc_anim_progress(&cc->anim);
    if (cc->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.94f + 0.06f * p;
    float ox = pad + (w - 2.0f * pad) * cc->anim_ox;
    float oy = pad + (h - 2.0f * pad) * cc->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Soft drop shadow. */
    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 12.0f, 18.0f,
                                     nvgRGBA(0, 0, 0, 90), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    /* Card. No separate "Control Center" title -- the reference screenshot
     * goes straight from the card edge into the user header card
     * (docs/13-POPOUTS-SPEC.md sec.1). */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgFillColor(vg, nvgRGBA(t->surface_container.r, t->surface_container.g, t->surface_container.b,
                             255));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(t->outline.r, t->outline.g, t->outline.b, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    cc_layout l = cc_get_layout(w);

    /* --- User header card (HeaderPane.qml) --------------------------- */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l.ix, l.header_y, l.iw, l.header_h, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    char username[64], subtitle[64];
    get_user_info(username, sizeof(username), subtitle, sizeof(subtitle));

    /* Avatar: ~/.face if present (loaded via the same PNG/JPEG decoder
     * launcher.c already uses for app icons); otherwise a letter-avatar
     * circle (first letter of the username), per this task's explicit
     * fallback spec rather than DMS's generic "person" glyph fallback. */
    int face_img = load_face_image(cc->render, 60);
    if (face_img > 0) {
        NVGpaint pat = nvgImagePattern(vg, l.avatar_cx - l.avatar_r, l.avatar_cy - l.avatar_r,
                                       l.avatar_r * 2.0f, l.avatar_r * 2.0f, 0.0f, face_img, 1.0f);
        nvgBeginPath(vg);
        nvgCircle(vg, l.avatar_cx, l.avatar_cy, l.avatar_r);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
        nvgDeleteImage(vg, face_img);
    } else {
        nvgBeginPath(vg);
        nvgCircle(vg, l.avatar_cx, l.avatar_cy, l.avatar_r);
        nvgFillColor(vg, tc(t->surface_container_highest));
        nvgFill(vg);
        if (username[0]) {
            char initial[2] = {(char)toupper((unsigned char)username[0]), '\0'};
            nvgFontFaceId(vg, cc->render->font_ui);
            nvgFontSize(vg, 22.0f);
            nvgFillColor(vg, tc(t->surface_text));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, l.avatar_cx, l.avatar_cy, initial, NULL);
        } else {
            dc_render_icon(cc->render, DC_ICON_PERSON, l.avatar_cx, l.avatar_cy, 26.0f,
                           t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }
    }

    const float text_x = l.avatar_cx + l.avatar_r + 12.0f;
    nvgFontFaceId(vg, cc->render->font_ui);
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, text_x, l.avatar_cy - 9.0f, username, NULL);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, text_x, l.avatar_cy + 9.0f, subtitle, NULL);

    /* lock / power / settings / edit (docs/13-POPOUTS-SPEC.md sec.1). No
     * hover-tint here: dankc's popout surfaces don't have per-pixel pointer-
     * motion tracking wired up yet (see main.c's handle_bar_motion comment),
     * and wiring that is out of this task's touch-scope (controlcenter.c
     * only) -- left as a deviation rather than forcing a main.c change. */
    dc_render_icon(cc->render, DC_ICON_LOCK, l.btn_cx[0], l.btn_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    dc_render_icon(cc->render, DC_ICON_POWER, l.btn_cx[1], l.btn_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    dc_render_icon(cc->render, DC_ICON_SETTINGS, l.btn_cx[2], l.btn_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    dc_render_icon(cc->render, DC_ICON_EDIT, l.btn_cx[3], l.btn_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    /* --- Live state for the sliders + tile grid ----------------------- */
    dc_audio_info audio_out;
    bool have_out = dc_audio_read(&audio_out);
    dc_audio_info audio_in;
    bool have_in = audio_source_read(&audio_in);
    dc_net_info net;
    dc_net_wifi(&net);
    dc_bluez_info bt;
    bool have_bt = dc_bluez_read(&bt);
    char sink_name[64], source_name[64];
    read_audio_device_names(sink_name, sizeof(sink_name), source_name, sizeof(source_name));
    float brightness = read_brightness();

    /* --- Sliders: volume + brightness, side by side ------------------- */
    draw_slider(cc->render, l.slot_x[0], l.sliders_y + l.slider_h / 2.0f, l.slot_w,
               DC_ICON_VOLUME_UP, have_out ? audio_out.volume / 100.0f : 0.5f);
    draw_slider(cc->render, l.slot_x[1], l.sliders_y + l.slider_h / 2.0f, l.slot_w,
               DC_ICON_BRIGHTNESS_MEDIUM, brightness >= 0.0f ? brightness : 0.7f);

    /* --- Tile grid: wifi/bluetooth, audioOutput/audioInput, nightMode/
     * darkMode (order per the user's controlCenterWidgets config) -------- */
    char wifi_title[64], wifi_sub[32];
    if (net.connected) {
        snprintf(wifi_title, sizeof(wifi_title), "%s", net.ssid[0] ? net.ssid : "Connected");
        if (net.signal_percent >= 0)
            snprintf(wifi_sub, sizeof(wifi_sub), "%d%%", net.signal_percent);
        else
            snprintf(wifi_sub, sizeof(wifi_sub), "Connected");
    } else {
        snprintf(wifi_title, sizeof(wifi_title), "Wi-Fi");
        snprintf(wifi_sub, sizeof(wifi_sub), "%s", net.has_wifi ? "Disconnected" : "Unavailable");
    }
    draw_pill_tile(cc->render, cc_tile_x(&l, 0), cc_tile_y(&l, 0), l.tile_w, l.tile_h, DC_ICON_WIFI,
                  wifi_title, wifi_sub, net.connected);

    /* NOTE: "active" (icon chip fill) tracks adapter *powered* state, not
     * device-connected -- matches the reference screenshot (chip is solid
     * green with 0 connected devices). bluez.h has no device-count field
     * (out of touch-scope to add one), so the subtitle is "Connected"/"No
     * devices" rather than the reference's exact "N connected". */
    const char *bt_title = (have_bt && bt.powered) ? "Enabled" : "Disabled";
    const char *bt_sub = (have_bt && bt.connected) ? "Connected" : "No devices";
    draw_pill_tile(cc->render, cc_tile_x(&l, 1), cc_tile_y(&l, 0), l.tile_w, l.tile_h,
                  DC_ICON_BLUETOOTH, bt_title, bt_sub, have_bt && bt.powered);

    char out_title[64], out_sub[16];
    snprintf(out_title, sizeof(out_title), "%s", sink_name[0] ? sink_name : "Speakers");
    bool out_muted = have_out && audio_out.muted;
    snprintf(out_sub, sizeof(out_sub), "%s", out_muted ? "Muted" : "");
    if (!out_muted)
        snprintf(out_sub, sizeof(out_sub), "%d%%", have_out ? audio_out.volume : 0);
    draw_pill_tile(cc->render, cc_tile_x(&l, 0), cc_tile_y(&l, 1), l.tile_w, l.tile_h,
                  DC_ICON_VOLUME_UP, out_title, out_sub, !out_muted);

    char in_title[64], in_sub[16];
    snprintf(in_title, sizeof(in_title), "%s", source_name[0] ? source_name : "Microphone");
    bool in_muted = have_in && audio_in.muted;
    if (in_muted)
        snprintf(in_sub, sizeof(in_sub), "Muted");
    else
        snprintf(in_sub, sizeof(in_sub), "%d%%", have_in ? audio_in.volume : 0);
    draw_pill_tile(cc->render, cc_tile_x(&l, 1), cc_tile_y(&l, 1), l.tile_w, l.tile_h,
                  in_muted ? DC_ICON_MIC_OFF : DC_ICON_MIC, in_title, in_sub, !in_muted);

    /* nightMode/darkMode: dankc has no real dark/night-mode service state
     * yet, so these stay the same hardcoded placeholders the previous
     * implementation used (one active, one not) -- restyled only, per this
     * task's "logic should NOT change" instruction. This means it won't
     * match the reference screenshot's both-active look. */
    draw_toggle_tile(cc->render, cc_tile_x(&l, 0), cc_tile_y(&l, 2), l.tile_w, l.tile_h,
                     DC_ICON_NIGHTLIGHT, "Night Mode", false);
    draw_toggle_tile(cc->render, cc_tile_x(&l, 1), cc_tile_y(&l, 2), l.tile_w, l.tile_h,
                     DC_ICON_CONTRAST, "Dark Mode", true);

    nvgEndFrame(vg);

    if ((dc_anim_active(&cc->anim) || cc->closing) && !cc->frame_cb) {
        cc->frame_cb = wl_surface_frame(cc->surface);
        wl_callback_add_listener(cc->frame_cb, &cc_frame_listener, cc);
    }
    dc_egl_swap(cc->egl, &cc->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_control_center *cc = data;
    DC_UNUSED(fs);
    cc->scale120 = (int)scale;
    recompute_physical(cc);
    cc_render(cc);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_control_center *cc = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    cc->logical_width = width > 0 ? (int)width : DC_CC_WIDTH;
    cc->logical_height = height > 0 ? (int)height : DC_CC_HEIGHT;
    cc->configured = true;
    recompute_physical(cc);
    cc_render(cc);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_control_center *cc = data;
    DC_UNUSED(surface);
    cc->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_control_center *dc_control_center_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_control_center *cc = calloc(1, sizeof(*cc));
    cc->wl = wl;
    cc->egl = egl;
    cc->render = render;
    cc->logical_width = DC_CC_WIDTH;
    cc->logical_height = DC_CC_HEIGHT;
    cc->scale120 = DC_SCALE_BASE;
    return cc;
}

static void cc_show(dc_control_center *cc, dc_output *output)
{
    cc->output = output;
    cc->configured = false;
    cc->egl_ready = false;
    cc->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    dc_anim_start(&cc->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    cc->surface = wl_compositor_create_surface(cc->wl->compositor);
    if (cc->wl->fractional_scale_mgr) {
        cc->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            cc->wl->fractional_scale_mgr, cc->surface);
        wp_fractional_scale_v1_add_listener(cc->fractional_scale, &fractional_scale_listener, cc);
    }
    if (cc->wl->viewporter)
        cc->viewport = wp_viewporter_get_viewport(cc->wl->viewporter, cc->surface);

    cc->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        cc->wl->layer_shell, cc->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:control-center");

    /* Bar-adjacent, right-aligned (docs/13-POPOUTS-SPEC.md sec.0/1: CC opens
     * near the bar's right cluster, on whichever screen edge the bar is on). */
    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_END, DC_CC_SIDE_MARGIN);
    cc->anim_ox = pa.origin_x;
    cc->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(cc->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(cc->layer_surface, DC_CC_WIDTH, DC_CC_HEIGHT);
    zwlr_layer_surface_v1_set_margin(cc->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_exclusive_zone(cc->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(cc->layer_surface, &layer_surface_listener, cc);

    wl_surface_commit(cc->surface);
    cc->visible = true;
    cc->closing = false;
    dc_debug("control center shown");
}

static void cc_teardown(dc_control_center *cc)
{
    if (cc->frame_cb) {
        wl_callback_destroy(cc->frame_cb);
        cc->frame_cb = NULL;
    }
    if (cc->egl_ready)
        dc_egl_window_finish(&cc->egl_window, cc->egl);
    if (cc->viewport)
        wp_viewport_destroy(cc->viewport);
    if (cc->fractional_scale)
        wp_fractional_scale_v1_destroy(cc->fractional_scale);
    if (cc->layer_surface)
        zwlr_layer_surface_v1_destroy(cc->layer_surface);
    if (cc->surface)
        wl_surface_destroy(cc->surface);
    cc->egl_ready = false;
    cc->configured = false;
    cc->viewport = NULL;
    cc->fractional_scale = NULL;
    cc->layer_surface = NULL;
    cc->surface = NULL;
    cc->visible = false;
    cc->closing = false;
    dc_debug("control center hidden");
}

static void cc_begin_close(dc_control_center *cc)
{
    if (!cc->visible || cc->closing)
        return;
    dc_anim_start(&cc->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    cc->closing = true;
    if (!dc_anim_active(&cc->anim)) {
        cc_teardown(cc);
        return;
    }
    cc_render(cc);
}

void dc_control_center_toggle(dc_control_center *cc, dc_output *output)
{
    if (cc->visible)
        cc_begin_close(cc);
    else
        cc_show(cc, output);
}

bool dc_control_center_visible(dc_control_center *cc)
{
    return cc->visible;
}

void dc_control_center_hide(dc_control_center *cc)
{
    cc_begin_close(cc);
}

struct wl_surface *dc_control_center_surface(dc_control_center *cc)
{
    return cc->surface;
}

void dc_control_center_handle_click(dc_control_center *cc, double x, double y)
{
    if (!cc->visible || cc->closing)
        return;

    cc_layout l = cc_get_layout((float)cc->logical_width);

    /* Header action buttons: lock, power, settings, edit. */
    for (int i = 0; i < 4; i++) {
        double dx = x - (double)l.btn_cx[i];
        double dy = y - (double)l.btn_cy;
        if (dx * dx + dy * dy <= (double)(l.btn_r * l.btn_r)) {
            switch (i) {
            case 0: /* lock */
                run_self_ctl("lock");
                break;
            case 1: /* power */
                /* TODO(P4-power): power menu modal not yet implemented
                 * (QML: ControlCenterPopout.qml's powerMenuModalLoader /
                 * Components/PowerButton.qml). */
                dc_debug("control center: power button (power menu TODO)");
                break;
            case 2: /* settings */
                run_self_ctl("settings");
                break;
            case 3: /* edit */
                /* TODO: tile edit/reorder mode (EditControls.qml) not
                 * implemented -- dankc's tile grid/order is config-driven,
                 * not user-editable at runtime yet. */
                dc_debug("control center: edit button (edit mode TODO)");
                break;
            }
            return;
        }
    }

    /* Sliders: volume (slot 0) / brightness (slot 1), click-to-set. */
    for (int i = 0; i < 2; i++) {
        if (x < (double)l.slot_x[i] || x > (double)(l.slot_x[i] + l.slot_w))
            continue;
        if (y < (double)l.sliders_y || y > (double)(l.sliders_y + l.slider_h))
            continue;

        const float track_x = l.slot_x[i] + 32.0f;
        const float track_w = l.slot_w - 32.0f;
        float frac = (float)(x - track_x) / track_w;
        if (frac < 0.0f)
            frac = 0.0f;
        if (frac > 1.0f)
            frac = 1.0f;

        if (i == 0) {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f", frac);
            run_detached(cmd);
        } else {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "brightnessctl set %d%% 2>/dev/null",
                     (int)(frac * 100.0f + 0.5f));
            run_detached(cmd);
        }
        cc_render(cc);
        return;
    }

    /* Tile grid: wifi/bluetooth toggle rfkill (unchanged from before);
     * audioOutput/audioInput toggle mute via the same wpctl already used by
     * the sliders above (new, but reuses the exact tool/pattern -- these
     * tiles didn't exist as clickable elements before this task);
     * nightMode/darkMode stay no-ops (no backing service yet). */
    for (int row = 0; row < 3; row++) {
        float ry = cc_tile_y(&l, row);
        if (y < (double)ry || y > (double)(ry + l.tile_h))
            continue;
        for (int col = 0; col < 2; col++) {
            float rx = cc_tile_x(&l, col);
            if (x < (double)rx || x > (double)(rx + l.tile_w))
                continue;
            if (row == 0 && col == 0)
                run_detached("rfkill toggle wifi");
            else if (row == 0 && col == 1)
                run_detached("rfkill toggle bluetooth");
            else if (row == 1 && col == 0)
                run_detached("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
            else if (row == 1 && col == 1)
                run_detached("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle");
            else
                dc_debug("control center: night/dark toggle (no-op)");
            cc_render(cc);
            return;
        }
    }
}

void dc_control_center_destroy(dc_control_center *cc)
{
    if (!cc)
        return;
    if (cc->visible)
        cc_teardown(cc);
    free(cc);
}
