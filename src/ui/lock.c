#include "ui/lock.h"

#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/auth.h"
#include "theme/theme.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ext-session-lock-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"

#define DC_LOCK_MAX_OUTPUTS 16
#define DC_SCALE_BASE 120
#define DC_LOCK_PW_MAX 256

struct lock_output {
    dc_lock *lock;
    dc_output *output;
    struct wl_surface *surface;
    struct ext_session_lock_surface_v1 *lock_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;
    int logical_w, logical_h, scale120, phys_w, phys_h;
    bool configured, egl_ready;
};

struct dc_lock {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;

    struct ext_session_lock_v1 *lock;
    struct lock_output outputs[DC_LOCK_MAX_OUTPUTS];
    int n_outputs;

    char password[DC_LOCK_PW_MAX];
    int pw_len;
    bool active;
    bool auth_failed;
};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

static void render_output(struct lock_output *o)
{
    dc_lock *l = o->lock;
    if (!o->configured || o->phys_w <= 0)
        return;
    if (!o->egl_ready) {
        if (!dc_egl_window_init(&o->egl_window, l->egl, o->surface, o->phys_w, o->phys_h))
            return;
        o->egl_ready = true;
    } else {
        dc_egl_window_resize(&o->egl_window, o->phys_w, o->phys_h);
    }
    if (!dc_egl_make_current(l->egl, &o->egl_window))
        return;
    if (!dc_render_ensure(l->render))
        return;
    if (o->viewport)
        wp_viewport_set_destination(o->viewport, o->logical_w, o->logical_h);

    NVGcontext *vg = l->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = o->logical_w, h = o->logical_h;

    glViewport(0, 0, o->phys_w, o->phys_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); /* opaque — must fully cover the screen */
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)o->scale120 / DC_SCALE_BASE);

    /* Dim themed backdrop. */
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgFillColor(vg, tc(t->surface));
    nvgFill(vg);

    /* Big clock. */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hhmm[16], date[64];
    strftime(hhmm, sizeof(hhmm), "%H:%M", &tm);
    strftime(date, sizeof(date), "%A, %B %-d", &tm);

    nvgFontFaceId(vg, l->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 96.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, w / 2.0f, h / 2.0f - 90.0f, hhmm, NULL);
    nvgFontSize(vg, 22.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, w / 2.0f, h / 2.0f - 30.0f, date, NULL);

    /* Password field: a rounded pill with dots. */
    const float fw = 320.0f, fh = 46.0f;
    const float fx = w / 2.0f - fw / 2.0f, fy = h / 2.0f + 30.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, fx, fy, fw, fh, fh / 2.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    if (l->auth_failed) {
        nvgStrokeColor(vg, tc(t->error));
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);
    }

    dc_render_icon(l->render, DC_ICON_LOCK, fx + 22.0f, fy + fh / 2.0f, 20.0f, t->surface_variant_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    if (l->pw_len == 0) {
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 110));
        nvgText(vg, fx + 44.0f, fy + fh / 2.0f, l->auth_failed ? "Wrong password" : "Password",
                NULL);
    } else {
        int dots = l->pw_len > 16 ? 16 : l->pw_len;
        for (int i = 0; i < dots; i++) {
            nvgBeginPath(vg);
            nvgCircle(vg, fx + 50.0f + i * 15.0f, fy + fh / 2.0f, 4.5f);
            nvgFillColor(vg, tc(t->surface_text));
            nvgFill(vg);
        }
    }

    nvgEndFrame(vg);
    dc_egl_swap(l->egl, &o->egl_window);
}

static void render_all(dc_lock *l)
{
    for (int i = 0; i < l->n_outputs; i++)
        render_output(&l->outputs[i]);
}

/* --- fractional scale (per output) --- */
static void fs_preferred(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale)
{
    struct lock_output *o = data;
    DC_UNUSED(fs);
    o->scale120 = (int)scale;
    o->phys_w = (o->logical_w * o->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    o->phys_h = (o->logical_h * o->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    render_output(o);
}
static const struct wp_fractional_scale_v1_listener fs_listener = {.preferred_scale = fs_preferred};

/* --- lock surface configure --- */
static void surface_configure(void *data, struct ext_session_lock_surface_v1 *surface,
                              uint32_t serial, uint32_t width, uint32_t height)
{
    struct lock_output *o = data;
    ext_session_lock_surface_v1_ack_configure(surface, serial);
    o->logical_w = (int)width;
    o->logical_h = (int)height;
    o->phys_w = (o->logical_w * o->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    o->phys_h = (o->logical_h * o->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    o->configured = true;
    render_output(o);
}
static const struct ext_session_lock_surface_v1_listener surface_listener = {
    .configure = surface_configure,
};

/* --- lock object events --- */
static void lock_handle_locked(void *data, struct ext_session_lock_v1 *lock)
{
    DC_UNUSED(lock);
    dc_lock *l = data;
    dc_info("session locked");
    render_all(l);
}

static void teardown(dc_lock *l);

static void lock_handle_finished(void *data, struct ext_session_lock_v1 *lock)
{
    DC_UNUSED(lock);
    dc_lock *l = data;
    /* The compositor refused the lock (e.g. already locked). Do NOT unlock;
     * just tear down our objects. */
    dc_warn("session lock finished/denied — aborting lock");
    teardown(l);
}

static const struct ext_session_lock_v1_listener lock_listener = {
    .locked = lock_handle_locked,
    .finished = lock_handle_finished,
};

dc_lock *dc_lock_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_lock *l = calloc(1, sizeof(*l));
    l->wl = wl;
    l->egl = egl;
    l->render = render;
    return l;
}

void dc_lock_engage(dc_lock *l)
{
    if (l->active)
        return;
    if (!l->wl->session_lock_manager) {
        dc_warn("compositor has no ext-session-lock; cannot lock");
        return;
    }

    l->lock = ext_session_lock_manager_v1_lock(l->wl->session_lock_manager);
    ext_session_lock_v1_add_listener(l->lock, &lock_listener, l);

    l->pw_len = 0;
    l->password[0] = '\0';
    l->auth_failed = false;
    l->n_outputs = 0;

    dc_output *output;
    wl_list_for_each(output, &l->wl->outputs, link) {
        if (l->n_outputs >= DC_LOCK_MAX_OUTPUTS)
            break;
        struct lock_output *o = &l->outputs[l->n_outputs];
        memset(o, 0, sizeof(*o));
        o->lock = l;
        o->output = output;
        o->scale120 = (output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
        o->surface = wl_compositor_create_surface(l->wl->compositor);
        if (l->wl->fractional_scale_mgr) {
            o->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
                l->wl->fractional_scale_mgr, o->surface);
            wp_fractional_scale_v1_add_listener(o->fractional_scale, &fs_listener, o);
        }
        if (l->wl->viewporter)
            o->viewport = wp_viewporter_get_viewport(l->wl->viewporter, o->surface);
        o->lock_surface =
            ext_session_lock_v1_get_lock_surface(l->lock, o->surface, output->wl_output);
        ext_session_lock_surface_v1_add_listener(o->lock_surface, &surface_listener, o);
        l->n_outputs++;
    }

    l->active = true;
    dc_info("engaging session lock (%d output%s)", l->n_outputs, l->n_outputs == 1 ? "" : "s");
}

/* Destroy our surfaces + the lock object WITHOUT unlocking (used on finished /
 * shutdown). The compositor keeps the session locked if we never unlocked. */
static void teardown(dc_lock *l)
{
    for (int i = 0; i < l->n_outputs; i++) {
        struct lock_output *o = &l->outputs[i];
        if (o->egl_ready)
            dc_egl_window_finish(&o->egl_window, l->egl);
        if (o->viewport)
            wp_viewport_destroy(o->viewport);
        if (o->fractional_scale)
            wp_fractional_scale_v1_destroy(o->fractional_scale);
        if (o->lock_surface)
            ext_session_lock_surface_v1_destroy(o->lock_surface);
        if (o->surface)
            wl_surface_destroy(o->surface);
    }
    l->n_outputs = 0;
    l->active = false;
    l->pw_len = 0;
    memset(l->password, 0, sizeof(l->password));
}

static void do_unlock(dc_lock *l)
{
    if (!l->active)
        return;
    /* Unlock first (this is the ONLY way to safely release the session), then
     * destroy our surfaces. */
    ext_session_lock_v1_unlock_and_destroy(l->lock);
    l->lock = NULL;
    for (int i = 0; i < l->n_outputs; i++) {
        struct lock_output *o = &l->outputs[i];
        if (o->egl_ready)
            dc_egl_window_finish(&o->egl_window, l->egl);
        if (o->viewport)
            wp_viewport_destroy(o->viewport);
        if (o->fractional_scale)
            wp_fractional_scale_v1_destroy(o->fractional_scale);
        if (o->lock_surface)
            ext_session_lock_surface_v1_destroy(o->lock_surface);
        if (o->surface)
            wl_surface_destroy(o->surface);
    }
    l->n_outputs = 0;
    l->active = false;
    l->pw_len = 0;
    memset(l->password, 0, sizeof(l->password));
    wl_display_flush(l->wl->display);
    dc_info("session unlocked");
}

bool dc_lock_active(dc_lock *l)
{
    return l && l->active;
}

void dc_lock_force_unlock(dc_lock *l)
{
    if (l && l->active)
        do_unlock(l);
}

void dc_lock_tick(dc_lock *l)
{
    if (l->active)
        render_all(l);
}

void dc_lock_handle_key(dc_lock *l, uint32_t keysym, const char *utf8)
{
    if (!l->active)
        return;

    /* Testing escape hatch: F1 force-unlocks when DANKC_LOCK_ESCAPE is set. */
    if (keysym == XKB_KEY_F1 && getenv("DANKC_LOCK_ESCAPE")) {
        dc_warn("lock: escape hatch (F1) — unlocking without auth");
        do_unlock(l);
        return;
    }

    switch (keysym) {
    case XKB_KEY_Escape:
        l->pw_len = 0;
        l->password[0] = '\0';
        l->auth_failed = false;
        break;
    case XKB_KEY_BackSpace:
        if (l->pw_len > 0)
            l->password[--l->pw_len] = '\0';
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (dc_auth_check(l->password)) {
            do_unlock(l);
            return;
        }
        dc_warn("lock: authentication failed");
        l->auth_failed = true;
        l->pw_len = 0;
        l->password[0] = '\0';
        break;
    default:
        if (utf8 && utf8[0] && !((unsigned char)utf8[0] < 0x20) && (unsigned char)utf8[0] != 0x7f) {
            size_t add = strlen(utf8);
            if ((size_t)l->pw_len + add < sizeof(l->password)) {
                memcpy(l->password + l->pw_len, utf8, add);
                l->pw_len += (int)add;
                l->password[l->pw_len] = '\0';
                l->auth_failed = false;
            }
        }
        break;
    }
    render_all(l);
}

void dc_lock_destroy(dc_lock *l)
{
    if (!l)
        return;
    /* On shutdown while locked, leave the session locked (security): tear down
     * without unlocking. */
    if (l->active)
        teardown(l);
    free(l);
}
