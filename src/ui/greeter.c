/* greeter.c — greetd login greeter UI surface + widgets + state machine.
 *
 * See greeter.h for the module's place in the architecture. This file has
 * three parts: (1) per-output surface/EGL/render plumbing, copied from
 * lock.c's pattern but using powermenu.c's fullscreen-overlay layer-shell
 * surface instead of ext-session-lock; (2) the widget draw code (clock/date,
 * user list, password pill, session picker), visually matching lock.c's
 * clock/date/password-pill-with-dots/error-stroke language; (3) the greetd
 * IPC state machine.
 *
 * STATE MACHINE (docs/28-GREETER-PLAN.md T3):
 *
 *   IDLE ---Enter (create_session)--> CREATING
 *   CREATING --AUTH_MESSAGE(secret/visible)--> PROMPT
 *   CREATING --SUCCESS (no auth needed)--> STARTING
 *   PROMPT --Enter (respond)--> VALIDATING
 *   VALIDATING --AUTH_MESSAGE(secret/visible)--> PROMPT (another round)
 *   VALIDATING --SUCCESS (auth complete)--> STARTING
 *   STARTING --SUCCESS (session launched)--> DONE (remember + done_cb)
 *   <any of CREATING/PROMPT/VALIDATING/STARTING> --ERROR--> FAILED
 *   FAILED ---Enter (create_session, fresh)--> CREATING
 *   <any non-IDLE, non-DONE state> --Escape (cancel_session)--> IDLE
 *
 * AUTH_MESSAGE events whose auth_type is INFO/ERROR are not prompts at all
 * (services/greetd.h) -- they never change state; the handler just displays
 * the text on the status line and immediately posts a null response to keep
 * greetd's exchange moving, so the greeter can never deadlock waiting on
 * input the user was never asked to give.
 *
 * A protocol-level ERROR (kind == DC_GREETD_ERROR, e.g. a wrong password)
 * always resets by calling dc_greetd_cancel() and returning to FAILED (which
 * looks identical to IDLE's password state to the user, distinguished only
 * by the status-line message) rather than trying to resume the now-dead
 * greetd-side exchange -- greetd tears down its session state server-side
 * once it has sent an error, so continuing to call dc_greetd_respond() would
 * be a protocol violation.
 *
 * That cancel_session is fire-and-forget: greetd's wire protocol carries no
 * request IDs, so a response is only ever attributable to "whatever we most
 * recently sent" (one outstanding request at a time). If a FAILED-state
 * retry's fresh create_session goes out before the cancel's own reply comes
 * back, two responses are briefly in flight and the wrong one could be
 * misread as "the retry needs no auth, start the session" -- verified as a
 * real, reproducible failure by this file's development smoke-test harness,
 * not a hypothetical. `cancel_pending` (see the struct field comment) is the
 * fix: it makes that one specific reply a swallowed no-op, by kind, before
 * the normal state-based dispatch ever sees it, regardless of what request
 * was sent after it.
 */
#include "ui/greeter.h"

#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/greetd.h"
#include "services/greeter_data.h"
#include "theme/theme.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_SCALE_BASE 120
#define DC_GR_MAX_OUTPUTS 16
#define DC_GR_MAX_USERS 64
#define DC_GR_MAX_SESSIONS 32
#define DC_GR_PW_MAX 256

/* Card geometry (logical px), same family of constants as powermenu.c's
 * DC_PM_* -- a centered card, fixed width, height derived from content. */
#define DC_GR_CARD_W 380.0f
#define DC_GR_PAD 20.0f
#define DC_GR_ROW_H 48.0f
#define DC_GR_ROW_GAP 8.0f
#define DC_GR_MAX_VISIBLE_USERS 5
#define DC_GR_PILL_H 46.0f
#define DC_GR_STATUS_H 20.0f
#define DC_GR_SESSION_ROW_H 30.0f
#define DC_GR_SECTION_GAP 18.0f
#define DC_GR_CHEVRON_W 28.0f

typedef enum {
    DC_GR_IDLE = 0,
    DC_GR_CREATING,
    DC_GR_PROMPT,
    DC_GR_VALIDATING,
    DC_GR_STARTING,
    DC_GR_DONE,
    DC_GR_FAILED,
} dc_greeter_state;

struct greeter_output {
    dc_greeter *g;
    dc_output *output;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;
    int logical_w, logical_h, scale120, phys_w, phys_h;
    bool configured, egl_ready;
};

struct dc_greeter {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    struct dc_loop *loop;
    dc_greetd *greetd;

    dc_greeter_done_cb done_cb;
    void *done_ud;

    struct greeter_output outputs[DC_GR_MAX_OUTPUTS];
    int n_outputs;

    dc_greeter_user users[DC_GR_MAX_USERS];
    int n_users;
    int user_idx; /* selection; valid iff n_users > 0 */

    dc_greeter_session sessions[DC_GR_MAX_SESSIONS];
    int n_sessions;
    int session_idx; /* selection; valid iff n_sessions > 0 */

    dc_greeter_state state;
    enum dc_greetd_auth_type auth_type; /* meaningful only in DC_GR_PROMPT */
    char prompt_label[64];              /* e.g. "Password" or greetd's own text */
    char status_text[256];              /* info/error message on the status line */
    bool status_is_error;

    /* greetd's wire protocol has no request IDs -- a response is only ever
     * attributable to "whatever we most recently sent" (proto.md: one
     * outstanding request at a time). dc_greetd_cancel() is sent as a
     * fire-and-forget side effect of both Escape and an ERROR event, without
     * waiting for its own reply; if the *next* real request (e.g. a FAILED-
     * state retry's fresh create_session) goes out before that cancel's ack
     * comes back, two responses are now in flight and the wrong one could
     * be read as "the retry succeeded with no auth needed". This flag makes
     * that ack a one-shot no-op, swallowed by kind regardless of `state`,
     * so it can never be misattributed to a later request. */
    bool cancel_pending;

    char password[DC_GR_PW_MAX];
    int pw_len;
};

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

/* --- selection helpers --------------------------------------------------- */

static const dc_greeter_user *current_user(const dc_greeter *g)
{
    if (g->n_users <= 0)
        return NULL;
    return &g->users[g->user_idx];
}

static const dc_greeter_session *current_session(const dc_greeter *g)
{
    if (g->n_sessions <= 0)
        return NULL;
    return &g->sessions[g->session_idx];
}

/* Whether the user list / user-picker keys are live right now -- only while
 * no greetd exchange is in flight (picking a different user mid-auth would
 * desync the greetd-side session, which is keyed to whichever username was
 * sent in create_session). */
static bool user_picker_active(const dc_greeter *g)
{
    return g->state == DC_GR_IDLE || g->state == DC_GR_FAILED;
}

/* --- layout ---------------------------------------------------------------
 * Recomputed every render/hit-test from the (whole-output) surface size,
 * same "shared layout struct" convention as powermenu.c's pm_layout. */
typedef struct {
    float clock_y, date_y;
    float card_x, card_y, card_w, card_h;
    int visible_users; /* min(n_users, DC_GR_MAX_VISIBLE_USERS), may be 0 */
    float user_row_y[DC_GR_MAX_VISIBLE_USERS];
    float row_x, row_w;
    float pill_x, pill_y, pill_w, pill_h;
    float status_y;
    float session_y;
    float chevron_l_x, chevron_r_x;
} gr_layout;

static gr_layout gr_get_layout(const dc_greeter *g, float screen_w, float screen_h)
{
    gr_layout lay = {0};
    lay.clock_y = screen_h * 0.16f;
    lay.date_y = lay.clock_y + 36.0f;

    lay.visible_users = g->n_users < DC_GR_MAX_VISIBLE_USERS ? g->n_users : DC_GR_MAX_VISIBLE_USERS;
    float users_h = lay.visible_users > 0
                        ? (lay.visible_users * DC_GR_ROW_H + (lay.visible_users - 1) * DC_GR_ROW_GAP)
                        : 0.0f;

    lay.card_w = DC_GR_CARD_W;
    lay.card_h = 2.0f * DC_GR_PAD + users_h + (lay.visible_users > 0 ? DC_GR_SECTION_GAP : 0.0f) +
                 DC_GR_PILL_H + DC_GR_STATUS_H + DC_GR_SECTION_GAP + DC_GR_SESSION_ROW_H;
    lay.card_x = (screen_w - lay.card_w) / 2.0f;
    lay.card_y = (screen_h - lay.card_h) / 2.0f + 20.0f; /* nudge below the clock/date */

    lay.row_x = lay.card_x + DC_GR_PAD;
    lay.row_w = lay.card_w - 2.0f * DC_GR_PAD;

    float y = lay.card_y + DC_GR_PAD;
    for (int i = 0; i < lay.visible_users; i++) {
        lay.user_row_y[i] = y;
        y += DC_GR_ROW_H + DC_GR_ROW_GAP;
    }
    if (lay.visible_users > 0)
        y += DC_GR_SECTION_GAP - DC_GR_ROW_GAP;

    lay.pill_x = lay.row_x;
    lay.pill_y = y;
    lay.pill_w = lay.row_w;
    lay.pill_h = DC_GR_PILL_H;
    y += DC_GR_PILL_H;

    lay.status_y = y + DC_GR_STATUS_H / 2.0f;
    y += DC_GR_STATUS_H + DC_GR_SECTION_GAP;

    lay.session_y = y + DC_GR_SESSION_ROW_H / 2.0f;
    lay.chevron_l_x = lay.row_x;
    lay.chevron_r_x = lay.row_x + lay.row_w - DC_GR_CHEVRON_W;

    return lay;
}

/* --- greetd wire flow ------------------------------------------------------
 */

static void reset_password(dc_greeter *g)
{
    g->pw_len = 0;
    g->password[0] = '\0';
}

static void clear_status(dc_greeter *g)
{
    g->status_text[0] = '\0';
    g->status_is_error = false;
}

/* Build cmd=["/bin/sh","-c","exec <session.exec>"] and
 * env=["XDG_SESSION_TYPE=wayland","XDG_SESSION_DESKTOP=<names>",
 * "XDG_CURRENT_DESKTOP=<names>",NULL], then send start_session. Falls back to
 * a plain login shell if no session was enumerated (better than refusing to
 * ever finish the greeter over a missing .desktop file). */
static void begin_start_session(dc_greeter *g)
{
    if (!g->greetd)
        return;
    const dc_greeter_session *sess = current_session(g);
    const char *exec = (sess && sess->exec[0]) ? sess->exec : "/bin/sh -l";
    const char *desktop = (sess && sess->desktop_names[0]) ? sess->desktop_names
                          : sess                            ? sess->name
                                                             : "";

    char cmdbuf[DC_GREETER_SESSION_EXEC + 8];
    snprintf(cmdbuf, sizeof(cmdbuf), "exec %s", exec);
    char *cmd[] = {"/bin/sh", "-c", cmdbuf, NULL};

    char env_desktop[DC_GREETER_SESSION_DESKTOP_NAMES + 32];
    char env_current[DC_GREETER_SESSION_DESKTOP_NAMES + 32];
    snprintf(env_desktop, sizeof(env_desktop), "XDG_SESSION_DESKTOP=%s", desktop);
    snprintf(env_current, sizeof(env_current), "XDG_CURRENT_DESKTOP=%s", desktop);
    char *env[] = {"XDG_SESSION_TYPE=wayland", env_desktop, env_current, NULL};

    dc_greetd_start_session(g->greetd, cmd, env);
    g->state = DC_GR_STARTING;
    dc_info("greeter: authenticated -- starting session %s", sess ? sess->name : "(fallback shell)");
}

static void on_greetd_event(const struct dc_greetd_event *ev, void *user_data)
{
    dc_greeter *g = user_data;

    /* A pending fire-and-forget cancel_session's own reply -- see the
     * cancel_pending field comment. Swallow it unconditionally before it
     * can be misread as the answer to whatever was sent *after* it. */
    if (g->cancel_pending) {
        g->cancel_pending = false;
        if (ev->kind != DC_GREETD_SUCCESS && ev->kind != DC_GREETD_ERROR)
            dc_warn("greeter: unexpected %d while a cancel_session ack was pending", (int)ev->kind);
        return;
    }

    switch (ev->kind) {
    case DC_GREETD_SUCCESS:
        switch (g->state) {
        case DC_GR_CREATING:
        case DC_GR_VALIDATING:
            clear_status(g);
            begin_start_session(g);
            break;
        case DC_GR_STARTING: {
            const dc_greeter_user *u = current_user(g);
            const dc_greeter_session *s = current_session(g);
            dc_greeter_remember(u ? u->name : NULL, s ? s->name : NULL);
            g->state = DC_GR_DONE;
            dc_info("greeter: session started, handing off to greetd");
            if (g->done_cb)
                g->done_cb(g->done_ud);
            break;
        }
        default:
            dc_warn("greeter: unexpected greetd SUCCESS in state %d", (int)g->state);
            break;
        }
        break;

    case DC_GREETD_AUTH_MESSAGE:
        if (ev->auth_type == DC_GREETD_AUTH_SECRET || ev->auth_type == DC_GREETD_AUTH_VISIBLE) {
            g->auth_type = ev->auth_type;
            snprintf(g->prompt_label, sizeof(g->prompt_label), "%s",
                    ev->text[0] ? ev->text : "Password");
            reset_password(g);
            clear_status(g);
            g->state = DC_GR_PROMPT;
        } else {
            /* info/error auth messages are not prompts -- display and ack. */
            snprintf(g->status_text, sizeof(g->status_text), "%s", ev->text);
            g->status_is_error = (ev->auth_type == DC_GREETD_AUTH_ERROR);
            dc_greetd_respond(g->greetd, NULL);
        }
        break;

    case DC_GREETD_ERROR:
        if (g->greetd) {
            dc_greetd_cancel(g->greetd);
            g->cancel_pending = true;
        }
        reset_password(g);
        snprintf(g->status_text, sizeof(g->status_text), "%s",
                ev->text[0] ? ev->text : "Authentication failed");
        g->status_is_error = true;
        g->state = DC_GR_FAILED;
        dc_warn("greeter: greetd error (%s): %s", ev->error_type, ev->text);
        break;
    }
}

/* Enter/click "submit": IDLE/FAILED create a fresh session for the selected
 * user; PROMPT answers the pending auth message. Every other state is
 * mid-round-trip -- ignored, so a stray Enter can never double-send. */
static void gr_submit(dc_greeter *g)
{
    switch (g->state) {
    case DC_GR_IDLE:
    case DC_GR_FAILED:
        if (!g->greetd) {
            snprintf(g->status_text, sizeof(g->status_text), "%s", "greetd unavailable");
            g->status_is_error = true;
            break;
        }
        clear_status(g);
        reset_password(g);
        if (dc_greetd_create_session(g->greetd, current_user(g) ? current_user(g)->name : ""))
            g->state = DC_GR_CREATING;
        break;
    case DC_GR_PROMPT:
        if (g->greetd) {
            dc_greetd_respond(g->greetd, g->password);
            g->state = DC_GR_VALIDATING;
        }
        break;
    case DC_GR_CREATING:
    case DC_GR_VALIDATING:
    case DC_GR_STARTING:
    case DC_GR_DONE:
        break; /* awaiting greetd, or already finished */
    }
}

static void gr_cancel_to_idle(dc_greeter *g)
{
    if (g->greetd) {
        dc_greetd_cancel(g->greetd);
        g->cancel_pending = true;
    }
    reset_password(g);
    clear_status(g);
    g->state = DC_GR_IDLE;
}

/* --- render --------------------------------------------------------------
 */

/* `selected` doubles as the hover highlight: motion handling moves
 * g->user_idx to whatever row is under the pointer (see
 * dc_greeter_handle_motion()), the same "selection follows the mouse"
 * convention powermenu.c's rows use, so there is no separate hover state to
 * track here. */
static void draw_user_row(dc_greeter *g, const gr_layout *lay, int visible_idx)
{
    NVGcontext *vg = g->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_greeter_user *u = &g->users[visible_idx];
    bool selected = user_picker_active(g) && g->user_idx == visible_idx;
    float x = lay->row_x, y = lay->user_row_y[visible_idx], w = lay->row_w, h = DC_GR_ROW_H;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    if (selected)
        nvgFillColor(vg, tc_alpha(t->primary, 46));
    else
        nvgFillColor(vg, tc_alpha(t->surface_variant, 30));
    nvgFill(vg);
    if (selected) {
        nvgStrokeColor(vg, tc(t->primary));
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);
    }

    float cy = y + h / 2.0f;
    dc_render_icon(g->render, DC_ICON_PERSON, x + 22.0f, cy, 22.0f,
                   selected ? t->primary : t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, g->render->font_ui);
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x + 44.0f, cy, u->display[0] ? u->display : u->name, NULL);
}

/* Password pill: same rounded-pill + dots + error-stroke visual language as
 * lock.c's password field, plus a status line underneath for greetd's
 * info/error text (lock.c has no such line -- PAM only ever reports
 * pass/fail, never an intermediate message). A "visible" auth prompt (rare;
 * e.g. an OTP/username re-ask) echoes the typed text in the clear instead of
 * dots, per services/greetd.h's auth_type contract. */
static void draw_password_pill(dc_greeter *g, const gr_layout *lay)
{
    NVGcontext *vg = g->render->vg;
    const dc_theme *t = dc_theme_current;
    bool active = g->state == DC_GR_PROMPT;
    float x = lay->pill_x, y = lay->pill_y, w = lay->pill_w, h = lay->pill_h;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, h / 2.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    if (g->status_is_error) {
        nvgStrokeColor(vg, tc(t->error));
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);
    } else if (active) {
        nvgStrokeColor(vg, tc_alpha(t->primary, 160));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }

    dc_render_icon(g->render, DC_ICON_LOCK, x + 22.0f, y + h / 2.0f, 20.0f, t->surface_variant_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    if (g->pw_len == 0) {
        const char *placeholder = g->state == DC_GR_CREATING || g->state == DC_GR_VALIDATING
                                      ? "Please wait\xe2\x80\xa6"
                                      : (active ? g->prompt_label : "Password");
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 110));
        nvgFontFaceId(vg, g->render->font_ui);
        nvgText(vg, x + 44.0f, y + h / 2.0f, placeholder, NULL);
    } else if (g->auth_type == DC_GREETD_AUTH_VISIBLE) {
        nvgFontFaceId(vg, g->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, x + 44.0f, y + h / 2.0f, g->password, NULL);
    } else {
        int dots = g->pw_len > 16 ? 16 : g->pw_len;
        for (int i = 0; i < dots; i++) {
            nvgBeginPath(vg);
            nvgCircle(vg, x + 50.0f + i * 15.0f, y + h / 2.0f, 4.5f);
            nvgFillColor(vg, tc(t->surface_text));
            nvgFill(vg);
        }
    }
}

static void draw_status_line(dc_greeter *g, const gr_layout *lay)
{
    if (!g->status_text[0])
        return;
    NVGcontext *vg = g->render->vg;
    const dc_theme *t = dc_theme_current;
    float cx = lay->pill_x + lay->pill_w / 2.0f;

    nvgFontFaceId(vg, g->render->font_ui);
    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(g->status_is_error ? t->error : t->surface_variant_text));
    nvgText(vg, cx, lay->status_y, g->status_text, NULL);
}

static void draw_session_row(dc_greeter *g, const gr_layout *lay)
{
    NVGcontext *vg = g->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_greeter_session *s = current_session(g);
    float cx = lay->row_x + lay->row_w / 2.0f;

    if (g->n_sessions > 1) {
        dc_render_icon(g->render, DC_ICON_CHEVRON_LEFT, lay->chevron_l_x + DC_GR_CHEVRON_W / 2.0f,
                       lay->session_y, 20.0f, t->surface_variant_text,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        dc_render_icon(g->render, DC_ICON_CHEVRON_RIGHT, lay->chevron_r_x + DC_GR_CHEVRON_W / 2.0f,
                       lay->session_y, 20.0f, t->surface_variant_text,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    }

    nvgFontFaceId(vg, g->render->font_ui);
    nvgFontSize(vg, 13.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 190));
    nvgText(vg, cx, lay->session_y, s ? s->name : "No session found", NULL);
}

static void draw_clock(dc_greeter *g, float w, const gr_layout *lay)
{
    NVGcontext *vg = g->render->vg;
    const dc_theme *t = dc_theme_current;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char hhmm[16];
    strftime(hhmm, sizeof(hhmm), "%H:%M", &tm);
    nvgFontFaceId(vg, g->render->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 72.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, w / 2.0f, lay->clock_y, hhmm, NULL);

    char date[64];
    strftime(date, sizeof(date), "%A, %B %-d", &tm);
    nvgFontSize(vg, 18.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, w / 2.0f, lay->date_y, date, NULL);
}

static void render_output(struct greeter_output *o)
{
    dc_greeter *g = o->g;
    if (!o->configured || o->phys_w <= 0)
        return;
    if (!o->egl_ready) {
        if (!dc_egl_window_init(&o->egl_window, g->egl, o->surface, o->phys_w, o->phys_h))
            return;
        o->egl_ready = true;
    } else {
        dc_egl_window_resize(&o->egl_window, o->phys_w, o->phys_h);
    }
    if (!dc_egl_make_current(g->egl, &o->egl_window))
        return;
    if (!dc_render_ensure(g->render))
        return;
    if (o->viewport)
        wp_viewport_set_destination(o->viewport, o->logical_w, o->logical_h);

    NVGcontext *vg = g->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = (float)o->logical_w, h = (float)o->logical_h;

    glViewport(0, 0, o->phys_w, o->phys_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); /* opaque -- fullscreen, nothing behind us */
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)o->scale120 / DC_SCALE_BASE);

    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgFillColor(vg, tc(t->surface));
    nvgFill(vg);

    gr_layout lay = gr_get_layout(g, w, h);
    draw_clock(g, w, &lay);

    /* Card. */
    NVGpaint shadow =
        nvgBoxGradient(vg, lay.card_x, lay.card_y + 2.0f, lay.card_w, lay.card_h, 16.0f, 24.0f,
                       nvgRGBA(0, 0, 0, 110), nvgRGBA(0, 0, 0, 0));
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

    for (int i = 0; i < lay.visible_users; i++)
        draw_user_row(g, &lay, i);
    draw_password_pill(g, &lay);
    draw_status_line(g, &lay);
    draw_session_row(g, &lay);

    nvgEndFrame(vg);
    dc_egl_swap(g->egl, &o->egl_window);
}

static void render_all(dc_greeter *g)
{
    for (int i = 0; i < g->n_outputs; i++)
        render_output(&g->outputs[i]);
}

/* --- fractional scale / layer-surface configure (per output) -------------
 */

static void fs_preferred(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale)
{
    struct greeter_output *o = data;
    DC_UNUSED(fs);
    o->scale120 = (int)scale;
    o->phys_w = (o->logical_w * o->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    o->phys_h = (o->logical_h * o->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    render_output(o);
}
static const struct wp_fractional_scale_v1_listener fs_listener = {.preferred_scale = fs_preferred};

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height)
{
    struct greeter_output *o = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    o->logical_w = width > 0 ? (int)width : 1920;
    o->logical_h = height > 0 ? (int)height : 1080;
    o->phys_w = (o->logical_w * o->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    o->phys_h = (o->logical_h * o->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    o->configured = true;
    render_output(o);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    struct greeter_output *o = data;
    DC_UNUSED(surface);
    o->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* --- selection defaults ---------------------------------------------------
 */

static void pick_default_user(dc_greeter *g)
{
    g->user_idx = 0;
    if (g->n_users <= 0)
        return;
    char last[DC_GREETER_USER_NAME];
    if (!dc_greeter_last_user(last, sizeof(last)))
        return;
    for (int i = 0; i < g->n_users; i++) {
        if (strcmp(g->users[i].name, last) == 0) {
            g->user_idx = i;
            return;
        }
    }
}

static void pick_default_session(dc_greeter *g)
{
    g->session_idx = 0;
    if (g->n_sessions <= 0)
        return;
    char last[DC_GREETER_SESSION_NAME];
    if (dc_greeter_last_session(last, sizeof(last))) {
        for (int i = 0; i < g->n_sessions; i++) {
            if (strcmp(g->sessions[i].name, last) == 0) {
                g->session_idx = i;
                return;
            }
        }
    }
    /* No remembered session (or it's gone) -- default to the first Wayland
     * session; dc_greeter_sessions() sorts wayland-sessions/ entries before
     * xsessions/ ones, so the first non-x11 entry (if any) is exactly that. */
    for (int i = 0; i < g->n_sessions; i++) {
        if (!g->sessions[i].is_x11) {
            g->session_idx = i;
            return;
        }
    }
}

/* --- public API ------------------------------------------------------------
 */

dc_greeter *dc_greeter_create(dc_wayland *wl, dc_egl *egl, dc_render *render, struct dc_loop *loop,
                              dc_greeter_done_cb done_cb, void *user_data)
{
    dc_greeter *g = calloc(1, sizeof(*g));
    g->wl = wl;
    g->egl = egl;
    g->render = render;
    g->loop = loop;
    g->done_cb = done_cb;
    g->done_ud = user_data;
    g->state = DC_GR_IDLE;

    g->n_users = dc_greeter_users(g->users, DC_GR_MAX_USERS);
    pick_default_user(g);
    g->n_sessions = dc_greeter_sessions(g->sessions, DC_GR_MAX_SESSIONS);
    pick_default_session(g);

    g->greetd = dc_greetd_create(loop, on_greetd_event, g);
    if (!g->greetd) {
        dc_warn("greeter: could not connect to greetd (no $GREETD_SOCK?) -- UI only");
        snprintf(g->status_text, sizeof(g->status_text), "%s", "greetd unavailable");
        g->status_is_error = true;
    }

    if (wl) {
        dc_output *output;
        wl_list_for_each(output, &wl->outputs, link) {
            if (g->n_outputs >= DC_GR_MAX_OUTPUTS)
                break;
            struct greeter_output *o = &g->outputs[g->n_outputs];
            memset(o, 0, sizeof(*o));
            o->g = g;
            o->output = output;
            o->scale120 = (output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
            o->surface = wl_compositor_create_surface(wl->compositor);
            if (wl->fractional_scale_mgr) {
                o->fractional_scale =
                    wp_fractional_scale_manager_v1_get_fractional_scale(wl->fractional_scale_mgr, o->surface);
                wp_fractional_scale_v1_add_listener(o->fractional_scale, &fs_listener, o);
            }
            if (wl->viewporter)
                o->viewport = wp_viewporter_get_viewport(wl->viewporter, o->surface);

            o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
                wl->layer_shell, o->surface, output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                "dankc:greeter");
            zwlr_layer_surface_v1_set_anchor(o->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
            zwlr_layer_surface_v1_set_size(o->layer_surface, 0, 0);
            zwlr_layer_surface_v1_set_keyboard_interactivity(
                o->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
            zwlr_layer_surface_v1_add_listener(o->layer_surface, &layer_surface_listener, o);

            wl_surface_commit(o->surface);
            g->n_outputs++;
        }
    }

    dc_info("greeter: ready (%d user%s, %d session%s, %d output%s)", g->n_users,
            g->n_users == 1 ? "" : "s", g->n_sessions, g->n_sessions == 1 ? "" : "s", g->n_outputs,
            g->n_outputs == 1 ? "" : "s");
    return g;
}

void dc_greeter_destroy(dc_greeter *g)
{
    if (!g)
        return;
    for (int i = 0; i < g->n_outputs; i++) {
        struct greeter_output *o = &g->outputs[i];
        if (o->egl_ready)
            dc_egl_window_finish(&o->egl_window, g->egl);
        if (o->viewport)
            wp_viewport_destroy(o->viewport);
        if (o->fractional_scale)
            wp_fractional_scale_v1_destroy(o->fractional_scale);
        if (o->layer_surface)
            zwlr_layer_surface_v1_destroy(o->layer_surface);
        if (o->surface)
            wl_surface_destroy(o->surface);
    }
    dc_greetd_destroy(g->greetd);
    free(g);
}

void dc_greeter_handle_key(dc_greeter *g, uint32_t keysym, const char *utf8)
{
    if (!g || g->state == DC_GR_DONE)
        return;

    switch (keysym) {
    case XKB_KEY_Escape:
        if (g->state != DC_GR_IDLE)
            gr_cancel_to_idle(g);
        break;
    case XKB_KEY_Tab:
    case XKB_KEY_Down:
        if (user_picker_active(g) && g->n_users > 0)
            g->user_idx = (g->user_idx + 1) % g->n_users;
        break;
    case XKB_KEY_Up:
        if (user_picker_active(g) && g->n_users > 0)
            g->user_idx = (g->user_idx - 1 + g->n_users) % g->n_users;
        break;
    case XKB_KEY_Left:
        if (g->state != DC_GR_STARTING && g->n_sessions > 0)
            g->session_idx = (g->session_idx - 1 + g->n_sessions) % g->n_sessions;
        break;
    case XKB_KEY_Right:
        if (g->state != DC_GR_STARTING && g->n_sessions > 0)
            g->session_idx = (g->session_idx + 1) % g->n_sessions;
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        gr_submit(g);
        break;
    case XKB_KEY_BackSpace:
        if (g->state == DC_GR_PROMPT && g->pw_len > 0)
            g->password[--g->pw_len] = '\0';
        break;
    default:
        if (g->state == DC_GR_PROMPT && utf8 && utf8[0] &&
            !((unsigned char)utf8[0] < 0x20) && (unsigned char)utf8[0] != 0x7f) {
            size_t add = strlen(utf8);
            if ((size_t)g->pw_len + add < sizeof(g->password)) {
                memcpy(g->password + g->pw_len, utf8, add);
                g->pw_len += (int)add;
                g->password[g->pw_len] = '\0';
            }
        }
        break;
    }
    render_all(g);
}

/* Find which of our own per-output surfaces `surface` is, or -1. */
static int output_index_for(dc_greeter *g, struct wl_surface *surface)
{
    for (int i = 0; i < g->n_outputs; i++) {
        if (g->outputs[i].surface == surface)
            return i;
    }
    return -1;
}

void dc_greeter_handle_click(dc_greeter *g, struct wl_surface *surface, double x, double y)
{
    if (!g || g->state == DC_GR_DONE)
        return;
    int oi = output_index_for(g, surface);
    if (oi < 0)
        return;
    struct greeter_output *o = &g->outputs[oi];
    gr_layout lay = gr_get_layout(g, (float)o->logical_w, (float)o->logical_h);

    if (user_picker_active(g)) {
        for (int i = 0; i < lay.visible_users; i++) {
            if (in_rect(x, y, lay.row_x, lay.user_row_y[i], lay.row_x + lay.row_w,
                        lay.user_row_y[i] + DC_GR_ROW_H)) {
                g->user_idx = i;
                gr_submit(g);
                render_all(g);
                return;
            }
        }
    }

    if (g->state != DC_GR_STARTING && g->n_sessions > 1) {
        if (in_rect(x, y, lay.chevron_l_x, lay.session_y - DC_GR_SESSION_ROW_H / 2.0f,
                    lay.chevron_l_x + DC_GR_CHEVRON_W, lay.session_y + DC_GR_SESSION_ROW_H / 2.0f)) {
            g->session_idx = (g->session_idx - 1 + g->n_sessions) % g->n_sessions;
            render_all(g);
            return;
        }
        if (in_rect(x, y, lay.chevron_r_x, lay.session_y - DC_GR_SESSION_ROW_H / 2.0f,
                    lay.chevron_r_x + DC_GR_CHEVRON_W, lay.session_y + DC_GR_SESSION_ROW_H / 2.0f)) {
            g->session_idx = (g->session_idx + 1) % g->n_sessions;
            render_all(g);
            return;
        }
    }
}

void dc_greeter_handle_motion(dc_greeter *g, struct wl_surface *surface, double x, double y)
{
    if (!g || !user_picker_active(g))
        return;
    int oi = output_index_for(g, surface);
    if (oi < 0)
        return;
    struct greeter_output *o = &g->outputs[oi];
    gr_layout lay = gr_get_layout(g, (float)o->logical_w, (float)o->logical_h);

    for (int i = 0; i < lay.visible_users; i++) {
        if (in_rect(x, y, lay.row_x, lay.user_row_y[i], lay.row_x + lay.row_w,
                    lay.user_row_y[i] + DC_GR_ROW_H)) {
            if (g->user_idx != i) {
                g->user_idx = i;
                render_all(g);
            }
            return;
        }
    }
}
