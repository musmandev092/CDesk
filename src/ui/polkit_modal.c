/* polkit_modal.c — polkit authentication prompt (password dialog).
 *
 * See polkit_modal.h. Surface lifecycle (layer-shell overlay, fractional
 * scale, fade+scale entrance/exit) copied from ui/powermenu.c; the password
 * field (rounded pill + dot-per-character mask, placeholder text when empty)
 * copied from ui/lock.c. DMS reference: Modals/PolkitAuthModal.qml +
 * PolkitAuthContent.qml (title "Authentication Required", message,
 * identity, password field, Cancel/Authenticate buttons, inline error).
 */
#include "ui/polkit_modal.h"

#include "core/anim.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "render/shape.h"
#include "theme/theme.h"
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

#define DC_SCALE_BASE 120
#define DC_PKM_PW_MAX 256
#define DC_PKM_MSG_MAX 512
#define DC_PKM_IDENTITY_MAX 128
#define DC_PKM_ERROR_MAX 160

/* Card geometry (logical px). Message area is a fixed-height 3-line clamp
 * (dc_shape_draw_textbox truncates visually rather than growing the card --
 * matches PolkitAuthContent.qml's own `maximumLineCount: 2` clamp on the
 * message text). */
#define DC_PKM_WIDTH 420.0f
#define DC_PKM_PAD 6.0f
#define DC_PKM_INSET 22.0f
#define DC_PKM_TOP 22.0f
#define DC_PKM_TITLE_H 24.0f
#define DC_PKM_GAP 12.0f
#define DC_PKM_MSG_H 54.0f
#define DC_PKM_IDENTITY_H 18.0f
#define DC_PKM_FIELD_H 46.0f
#define DC_PKM_ERROR_H 18.0f
#define DC_PKM_BTN_H 44.0f
#define DC_PKM_BOTTOM 22.0f

#define DC_PKM_HEIGHT                                                                            \
    (2.0f * DC_PKM_PAD + DC_PKM_TOP + DC_PKM_TITLE_H + DC_PKM_GAP + DC_PKM_MSG_H + DC_PKM_GAP +   \
     DC_PKM_IDENTITY_H + DC_PKM_GAP + DC_PKM_FIELD_H + DC_PKM_GAP + DC_PKM_ERROR_H + DC_PKM_GAP + \
     DC_PKM_BTN_H + DC_PKM_BOTTOM)

struct dc_polkit_modal {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width, logical_height;
    int scale120;
    int phys_width, phys_height;

    char message[DC_PKM_MSG_MAX];
    char identity[DC_PKM_IDENTITY_MAX];
    char password[DC_PKM_PW_MAX];
    int pw_len;
    char error[DC_PKM_ERROR_MAX];
    bool busy;

    dc_polkit_submit_cb on_submit;
    dc_polkit_cancel_cb on_cancel;
    void *user_data;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;

    bool visible;
    bool configured;
    bool egl_ready;
};

typedef struct {
    float card_x, card_y, card_w, card_h;
    float content_x, content_w;
    float title_y, msg_y, identity_y, field_y, error_y, btn_y;
    float cancel_x, cancel_w, auth_x, auth_w;
} pkm_layout;

static pkm_layout pkm_get_layout(float screen_w, float screen_h)
{
    pkm_layout lay;
    lay.card_w = DC_PKM_WIDTH;
    lay.card_h = DC_PKM_HEIGHT;
    lay.card_x = (screen_w - lay.card_w) / 2.0f;
    lay.card_y = (screen_h - lay.card_h) / 2.0f;
    lay.content_x = lay.card_x + DC_PKM_PAD + DC_PKM_INSET;
    lay.content_w = lay.card_w - 2.0f * DC_PKM_PAD - 2.0f * DC_PKM_INSET;

    float y = lay.card_y + DC_PKM_PAD + DC_PKM_TOP;
    lay.title_y = y;
    y += DC_PKM_TITLE_H + DC_PKM_GAP;
    lay.msg_y = y;
    y += DC_PKM_MSG_H + DC_PKM_GAP;
    lay.identity_y = y;
    y += DC_PKM_IDENTITY_H + DC_PKM_GAP;
    lay.field_y = y;
    y += DC_PKM_FIELD_H + DC_PKM_GAP;
    lay.error_y = y;
    y += DC_PKM_ERROR_H + DC_PKM_GAP;
    lay.btn_y = y;

    lay.cancel_x = lay.content_x;
    lay.auth_w = (lay.content_w - DC_PKM_GAP) * 0.55f;
    lay.cancel_w = lay.content_w - DC_PKM_GAP - lay.auth_w;
    lay.auth_x = lay.content_x + lay.cancel_w + DC_PKM_GAP;
    return lay;
}

static void pkm_render(dc_polkit_modal *m);
static void pkm_teardown(dc_polkit_modal *m);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_polkit_modal *m = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    m->frame_cb = NULL;
    if (!m->visible)
        return;
    if (dc_anim_active(&m->anim)) {
        pkm_render(m);
    } else if (m->closing) {
        pkm_teardown(m);
    }
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

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

static void recompute_physical(dc_polkit_modal *m)
{
    m->phys_width = (m->logical_width * m->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    m->phys_height = (m->logical_height * m->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* --- render --------------------------------------------------------------
 */

static void draw_button(dc_render *render, float x, float y, float w, float h, const char *label,
                        bool primary, bool disabled)
{
    NVGcontext *vg = render->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, h / 2.0f);
    dc_color fill = primary ? t->primary : t->surface_variant;
    nvgFillColor(vg, disabled ? tc_alpha(fill, 90) : tc(fill));
    nvgFill(vg);

    dc_color txt = primary ? t->primary_text : t->surface_text;
    nvgFontFaceId(vg, render->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, disabled ? tc_alpha(txt, 140) : tc(txt));
    nvgText(vg, x + w / 2.0f, y + h / 2.0f, label, NULL);
}

static void pkm_render(dc_polkit_modal *m)
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
    const float w = (float)m->logical_width, h = (float)m->logical_height;

    glViewport(0, 0, m->phys_width, m->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)m->scale120 / DC_SCALE_BASE);

    float p = dc_anim_progress(&m->anim);
    if (m->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : (p < 0.0f ? 0.0f : p);
    float scale = 0.92f + 0.08f * (p > 1.0f ? 1.0f : p);

    pkm_layout lay = pkm_get_layout(w, h);
    float pivot_x = lay.card_x + lay.card_w / 2.0f;
    float pivot_y = lay.card_y + lay.card_h / 2.0f;

    /* Scrim (matches powermenu.c's centered-modal dim backdrop). */
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, (unsigned char)(alpha * 128.0f)));
    nvgFill(vg);

    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, pivot_x, pivot_y);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -pivot_x, -pivot_y);

    NVGpaint shadow =
        nvgBoxGradient(vg, lay.card_x, lay.card_y + 2.0f, lay.card_w, lay.card_h, 16.0f, 24.0f,
                       nvgRGBA(0, 0, 0, 130), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, lay.card_x - 40.0f, lay.card_y - 40.0f, lay.card_w + 80.0f, lay.card_h + 80.0f);
    nvgRoundedRect(vg, lay.card_x, lay.card_y, lay.card_w, lay.card_h, 18.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, lay.card_x, lay.card_y, lay.card_w, lay.card_h, 18.0f);
    nvgFillColor(vg, tc(t->surface_container));
    nvgFill(vg);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* Icon + title. */
    dc_render_icon(m->render, DC_ICON_LOCK, lay.content_x + 11.0f, lay.title_y + DC_PKM_TITLE_H / 2.0f,
                  20.0f, t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, m->render->font_ui);
    nvgFontSize(vg, 17.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, lay.content_x + 28.0f, lay.title_y + DC_PKM_TITLE_H / 2.0f, "Authentication Required",
           NULL);

    /* Message (wrapped, clamped to the fixed message-area height). */
    nvgFontSize(vg, 14.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, tc_alpha(t->surface_text, 220));
    dc_shape_draw_textbox(m->render, lay.content_x, lay.msg_y, lay.content_w,
                          m->message[0] ? m->message : "An application is requesting elevated privileges.",
                          NULL);

    /* Identity. */
    nvgFontSize(vg, 12.5f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_variant_text, 220));
    char idline[DC_PKM_IDENTITY_MAX + 32];
    snprintf(idline, sizeof(idline), "Authenticating as %s",
            m->identity[0] ? m->identity : "the current user");
    nvgText(vg, lay.content_x, lay.identity_y + DC_PKM_IDENTITY_H / 2.0f, idline, NULL);

    /* Password field: rounded pill with dots (ui/lock.c's pattern). */
    float fx = lay.content_x, fy = lay.field_y, fw = lay.content_w, fh = DC_PKM_FIELD_H;
    bool has_error = m->error[0] != '\0';
    nvgBeginPath(vg);
    nvgRoundedRect(vg, fx, fy, fw, fh, fh / 2.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    if (has_error) {
        nvgStrokeColor(vg, tc(t->error));
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);
    }
    dc_render_icon(m->render, DC_ICON_LOCK, fx + 22.0f, fy + fh / 2.0f, 18.0f, t->surface_variant_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    if (m->pw_len == 0) {
        nvgFontSize(vg, 14.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 110));
        nvgText(vg, fx + 44.0f, fy + fh / 2.0f, m->busy ? "Verifying..." : "Password", NULL);
    } else {
        int dots = m->pw_len > 20 ? 20 : m->pw_len;
        for (int i = 0; i < dots; i++) {
            nvgBeginPath(vg);
            nvgCircle(vg, fx + 50.0f + i * 14.0f, fy + fh / 2.0f, 4.0f);
            nvgFillColor(vg, tc(t->surface_text));
            nvgFill(vg);
        }
    }

    /* Inline error line (reserved space so the layout never jumps). */
    if (has_error) {
        nvgFontSize(vg, 12.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->error));
        nvgText(vg, lay.content_x, lay.error_y + DC_PKM_ERROR_H / 2.0f, m->error, NULL);
    }

    /* Cancel / Authenticate row, or a busy label in its place. */
    if (m->busy) {
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 160));
        nvgText(vg, lay.card_x + lay.card_w / 2.0f, lay.btn_y + DC_PKM_BTN_H / 2.0f,
               "Verifying password\xe2\x80\xa6", NULL);
    } else {
        draw_button(m->render, lay.cancel_x, lay.btn_y, lay.cancel_w, DC_PKM_BTN_H, "Cancel", false,
                   false);
        draw_button(m->render, lay.auth_x, lay.btn_y, lay.auth_w, DC_PKM_BTN_H, "Authenticate", true,
                   m->pw_len == 0);
    }

    nvgEndFrame(vg);

    if ((dc_anim_active(&m->anim) || m->closing) && !m->frame_cb) {
        m->frame_cb = wl_surface_frame(m->surface);
        wl_callback_add_listener(m->frame_cb, &frame_listener, m);
    }
    dc_egl_swap(m->egl, &m->egl_window);
}

/* --- surface lifecycle ---------------------------------------------------
 */

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_polkit_modal *m = data;
    DC_UNUSED(fs);
    m->scale120 = (int)scale;
    recompute_physical(m);
    pkm_render(m);
}
static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                           uint32_t serial, uint32_t width, uint32_t height)
{
    dc_polkit_modal *m = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    m->logical_width = width > 0 ? (int)width : (int)DC_PKM_WIDTH;
    m->logical_height = height > 0 ? (int)height : (int)DC_PKM_HEIGHT;
    m->configured = true;
    recompute_physical(m);
    pkm_render(m);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_polkit_modal *m = data;
    DC_UNUSED(surface);
    m->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_polkit_modal *dc_polkit_modal_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_polkit_modal *m = calloc(1, sizeof(*m));
    m->wl = wl;
    m->egl = egl;
    m->render = render;
    m->scale120 = DC_SCALE_BASE;
    return m;
}

static void pkm_teardown(dc_polkit_modal *m)
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
    dc_debug("polkit-modal hidden");
}

void dc_polkit_modal_show(dc_polkit_modal *m, dc_output *output, const char *message,
                          const char *identity, dc_polkit_submit_cb on_submit,
                          dc_polkit_cancel_cb on_cancel, void *user_data)
{
    /* Re-showing while already visible (e.g. a second BeginAuthentication
     * cookie racing the first) just refreshes the text/callbacks in place --
     * safer than tearing down a layer-surface mid-frame. */
    snprintf(m->message, sizeof(m->message), "%s", message ? message : "");
    snprintf(m->identity, sizeof(m->identity), "%s", identity ? identity : "");
    m->on_submit = on_submit;
    m->on_cancel = on_cancel;
    m->user_data = user_data;
    m->pw_len = 0;
    m->password[0] = '\0';
    m->error[0] = '\0';
    m->busy = false;

    if (m->visible) {
        pkm_render(m);
        return;
    }

    m->output = output;
    m->configured = false;
    m->egl_ready = false;
    m->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
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
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:polkit-agent");

    zwlr_layer_surface_v1_set_anchor(m->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                           ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(m->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        m->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(m->layer_surface, &layer_surface_listener, m);

    wl_surface_commit(m->surface);
    m->visible = true;
    m->closing = false;
    dc_debug("polkit-modal shown (identity=%s)", m->identity);
}

static void pkm_begin_close(dc_polkit_modal *m)
{
    if (!m->visible || m->closing)
        return;
    dc_anim_start(&m->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    m->closing = true;
    if (!dc_anim_active(&m->anim)) {
        pkm_teardown(m);
        return;
    }
    pkm_render(m);
}

void dc_polkit_modal_hide(dc_polkit_modal *m)
{
    pkm_begin_close(m);
}

bool dc_polkit_modal_visible(dc_polkit_modal *m)
{
    return m->visible;
}

struct wl_surface *dc_polkit_modal_surface(dc_polkit_modal *m)
{
    return m->surface;
}

void dc_polkit_modal_set_error(dc_polkit_modal *m, const char *text)
{
    if (!m)
        return;
    snprintf(m->error, sizeof(m->error), "%s", text ? text : "");
    m->pw_len = 0;
    m->password[0] = '\0';
    m->busy = false;
    if (m->visible)
        pkm_render(m);
}

void dc_polkit_modal_set_busy(dc_polkit_modal *m, bool busy)
{
    if (!m)
        return;
    m->busy = busy;
    if (m->visible)
        pkm_render(m);
}

void dc_polkit_modal_handle_key(dc_polkit_modal *m, uint32_t keysym, const char *utf8)
{
    if (!m->visible || m->closing || m->busy)
        return;

    switch (keysym) {
    case XKB_KEY_Escape:
        if (m->on_cancel)
            m->on_cancel(m->user_data);
        return;
    case XKB_KEY_BackSpace:
        if (m->pw_len > 0)
            m->password[--m->pw_len] = '\0';
        pkm_render(m);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (m->pw_len > 0 && m->on_submit)
            m->on_submit(m->password, m->user_data);
        return;
    default:
        if (utf8 && utf8[0] && !((unsigned char)utf8[0] < 0x20) && (unsigned char)utf8[0] != 0x7f) {
            size_t add = strlen(utf8);
            if ((size_t)m->pw_len + add < sizeof(m->password)) {
                memcpy(m->password + m->pw_len, utf8, add);
                m->pw_len += (int)add;
                m->password[m->pw_len] = '\0';
                if (m->error[0]) {
                    m->error[0] = '\0'; /* typing again clears the stale error */
                }
            }
        }
        pkm_render(m);
        return;
    }
}

void dc_polkit_modal_handle_click(dc_polkit_modal *m, double x, double y)
{
    if (!m->visible || m->closing)
        return;

    pkm_layout lay = pkm_get_layout((float)m->logical_width, (float)m->logical_height);
    bool in_card = in_rect(x, y, lay.card_x, lay.card_y, lay.card_x + lay.card_w,
                           lay.card_y + lay.card_h);
    if (!in_card) {
        if (!m->busy && m->on_cancel)
            m->on_cancel(m->user_data);
        return;
    }
    if (m->busy)
        return;

    if (in_rect(x, y, lay.cancel_x, lay.btn_y, lay.cancel_x + lay.cancel_w, lay.btn_y + DC_PKM_BTN_H)) {
        if (m->on_cancel)
            m->on_cancel(m->user_data);
        return;
    }
    if (m->pw_len > 0 &&
        in_rect(x, y, lay.auth_x, lay.btn_y, lay.auth_x + lay.auth_w, lay.btn_y + DC_PKM_BTN_H)) {
        if (m->on_submit)
            m->on_submit(m->password, m->user_data);
        return;
    }
}

void dc_polkit_modal_handle_motion(dc_polkit_modal *m, double x, double y)
{
    DC_UNUSED(m);
    DC_UNUSED(x);
    DC_UNUSED(y);
    /* No hover-highlight state (2 static buttons -- unlike powermenu's
     * selectable row list, there's nothing to move a keyboard-selection
     * cursor between). */
}

void dc_polkit_modal_destroy(dc_polkit_modal *m)
{
    if (!m)
        return;
    if (m->visible)
        pkm_teardown(m);
    free(m);
}
