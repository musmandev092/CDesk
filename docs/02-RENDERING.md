# DankC — Rendering, Text, Input & Animation

Stack: **libwayland-client + wlr-layer-shell + EGL/OpenGL ES 3 + nanovg** for vector chrome,
**Pango/HarfBuzz/FreeType/Fontconfig** for shaped text, **libxkbcommon** for keyboard,
a **frame-callback-driven tween engine** for animation. No Qt, no GTK.

Target verified: niri on Intel/Mesa `iris` (GLES 3.x). Bind every Wayland global defensively
(`min(advertised, supported)` version); no-op absent globals.

## 1. Layer-shell surfaces (`zwlr_layer_shell_v1`)

- `get_layer_surface(id, wl_surface, wl_output, layer, namespace)`; layers: background/bottom/top/overlay.
  Pass explicit `wl_output` for per-monitor bars; `namespace` (e.g. `"dankc:bar"`) is matchable by niri
  layer-rules.
- Surface requests: `set_size(w,h)` (0 needs both opposite edges anchored), `set_anchor(bitfield)`,
  `set_exclusive_zone(int)`, `set_margin`, `set_keyboard_interactivity(none/exclusive/on_demand)`,
  `get_popup(xdg_popup)`, `ack_configure`, `set_layer` (v2+), `set_exclusive_edge` (v5+).
- Events: `configure(serial,w,h)` (**logical** size, must ack) and `closed()`.

**Top bar recipe:** anchor `top|left|right`, `set_size(0,height)`, `set_exclusive_zone(height)`
(positive reserves; `-1` fills to edge ignoring others — for wallpaper; `0` reserves nothing but stays
clear). **Mandatory handshake:** create → set props → `wl_surface_commit()` **with no buffer** →
receive first `configure` → `ack_configure(serial)` → attach buffer sized to configured `w×h` → commit.
Attaching before first configure = protocol error / client killed.

**Popups/overlays** (control center, launcher, notifications, OSD): create `xdg_surface` from
`xdg_wm_base`, `get_popup(id, parent=NULL, positioner)` (**parent NULL**), then
`zwlr_layer_surface_v1.get_popup(xdg_popup)` to reparent. Standard `xdg_positioner` for placement.

**Per-output & hotplug:** one `wl_surface` + one layer surface per `wl_output`; track
`wl_registry.global`/`global_remove`.

**Protocol XML / build:** core `wayland.xml` (in `wayland`); stable/staging from `wayland-protocols`
(`pkg-config --variable=pkgdatadir wayland-protocols`); **wlr-layer-shell & wlr-* are NOT in
wayland-protocols — vendor the XML** into `protocol/`. Generate via `wayland-scanner
{client-header,private-code}`. Meson custom_target per XML (see `01-ARCHITECTURE.md §4`).

## 2. EGL + GLES render loop

```c
EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display, NULL);
eglInitialize(dpy,0,0); eglBindAPI(EGL_OPENGL_ES_API);
// config MUST include EGL_STENCIL_SIZE 8 (nanovg needs stencil) + EGL_ALPHA_SIZE 8
EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, (EGLint[]){EGL_CONTEXT_MAJOR_VERSION,3,EGL_NONE});
struct wl_egl_window *win = wl_egl_window_create(wl_surface, phys_w, phys_h);   // PHYSICAL px
EGLSurface surf = eglCreatePlatformWindowSurface(dpy, cfg, win, NULL);
eglMakeCurrent(dpy, surf, surf, ctx);
eglSwapInterval(dpy, 0);   // pace via frame callbacks, never block on swap
```

`eglSwapBuffers` **attaches+commits** the surface — don't also `wl_surface_attach`/`commit` the EGL
buffer. `wl_egl_window_resize(win,w,h,0,0)` **before** drawing on size/scale change.

**Frame/animation loop — one-shot frame callbacks:**
```c
void redraw(win){
  if (win->needs_resize){ wl_egl_window_resize(...); wp_viewport_set_destination(vp, log_w, log_h); }
  glViewport(0,0,phys_w,phys_h);
  /* nanovg frame here */
  struct wl_callback *cb = wl_surface_frame(wl_surface);   // request BEFORE commit
  wl_callback_add_listener(cb, &frame_listener, win);      // .done re-arms only if still animating
  wl_surface_damage_buffer(wl_surface, x,y,w,h);           // buffer px
  eglSwapBuffers(dpy, surf);                               // commits
}
```
**Critical:** a hidden surface stops receiving frame events, so never rely on `eglSwapBuffers` for
pacing. Re-arm the frame callback **only while a tween is active**; stop when idle (→ ~0% CPU),
re-kick on the next hover/click/data change. `eglSwapBuffersWithDamageEXT` for partial updates.

## 3. HiDPI / fractional scaling (highest-risk area)

- **Integer scale:** render at `logical×scale` px, `wl_surface_set_buffer_scale(N)`. Buffer dims must be
  an exact multiple of the scale or the compositor emits `wl_surface` error 2 and **kills the client**.
- **Fractional (niri supports `wp_fractional_scale_v1` + `wp_viewporter`):** keep
  `buffer_scale = 1`; bind `wp_fractional_scale_manager_v1.get_fractional_scale(surface)`; its
  `preferred_scale` event is **numerator/120** (180 ⇒ 1.5×). Render buffer at
  `logical × preferred/120` px, then `wp_viewport_set_destination(logical_w, logical_h)` to map down.
  Example: 100×50 logical @1.5× → 150×75 buffer, viewport destination 100×50.
- Order each change: `wl_egl_window_resize(phys)` → `set_destination(logical)` → render at phys → swap.
- nanovg: `nvgBeginFrame(vg, logical_w, logical_h, /*pixelRatio=*/scale)` with `glViewport` in physical px.

## 4. 2D drawing — nanovg (GLES3)

```c
#define NANOVG_GLES3_IMPLEMENTATION
#include "nanovg_gl.h"
NVGcontext *vg = nvgCreateGLES3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
```
Per frame: `nvgBeginFrame(vg, log_w, log_h, dpr)` … draw … `nvgEndFrame(vg)`.
Material primitives:
- Cards/pills: `nvgBeginPath` + `nvgRoundedRect` + `nvgFillColor(nvgRGBA(...))`.
- Gradients: `nvgLinearGradient` / `nvgRadialGradient`.
- **Elevation/drop shadow: `nvgBoxGradient(x,y,w,h,radius,feather,inner,outer)`** (feathered rounded rect).
- Clipping: `nvgScissor`/`nvgResetScissor`; state stack `nvgSave`/`nvgRestore`;
  cheap fades via `nvgGlobalAlpha`.

**Pitfalls:** nanovg **requires a stencil buffer** (clear it each frame or scissor/fills render garbage)
and uses **premultiplied alpha** — set `glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, ...)` and
keep the translucent layer-surface premultiplied or edges get halos.

**Blur (M6):** dual-Kawase in GLSL. Behind-panel blur ideally uses the compositor
`ext-background-effect` if niri exposes it; otherwise sample our own wallpaper texture. Blur is
**off by default** (matches DMS config on this machine) and user-toggleable.

## 5. Text — Pango + HarfBuzz + FreeType + Fontconfig

Two paths:
- **Rich/i18n path (panels, notifications):** PangoCairo → cached GL texture. `pango_cairo_create_layout`
  + `pango_cairo_show_layout` onto a Cairo ARGB32 surface (**premultiplied BGRA LE** — upload as
  `GL_BGRA`), re-rasterize only when the string changes.
- **Hot/simple path (bar labels, numbers, icons):** direct HarfBuzz shaping → FreeType raster →
  **R8 glyph atlas** via `glTexSubImage2D`:
```c
hb_buffer_add_utf8(buf,text,len,0,len); hb_buffer_guess_segment_properties(buf); hb_shape(font,buf,0,0);
for each glyph: gid = info[i].codepoint;   // ALREADY a glyph index — feed straight to FT_Load_Glyph
  FT_Load_Glyph(face,gid,FT_LOAD_DEFAULT); FT_Render_Glyph(face->glyph,FT_RENDER_MODE_NORMAL);
  pen_x += pos[i].x_advance/64.0;          // 26.6 fixed point → /64  (forgetting this = text blob)
```
Reference: `freetype-gl` `demos/harfbuzz.c`.

**Icon fonts:** Material Symbols + Nerd Font. Two access modes: (1) **ligature-by-name** ("wifi")
needs HarfBuzz/Pango shaping; (2) **direct codepoint** (Private Use Area) works everywhere including
the nanovg fontstash. For the hot path use **codepoints**. Material Symbols variable axes
(`FILL`,`wght`,`GRAD`,`opsz`) via `FT_Set_Var_Design_Coordinates` (invalidates cached atlas entries).
Atlas is **per size and per variation** — plan eviction. Use `FT_LOAD_NO_HINTING` for smoothly
animated/scaled text (hinting shimmers).

## 6. Input — seats + xkbcommon

Bind `wl_seat`; on `capabilities` create/destroy pointer/keyboard/touch (can change at runtime).

- **Pointer:** dispatch on the grouped `frame` event (v5+), not per sub-event. Prefer
  `axis_value120` (v8+, detent=120) for scroll. Coords via `wl_fixed_to_double`; buttons = Linux codes
  (`BTN_LEFT 0x110`).
- **Keyboard + xkbcommon:** keymap fd must be `mmap(MAP_PRIVATE)` then `munmap`/`close` (leaking the fd
  is a classic bug). `xkb_keycode = wl_key + 8` (mandatory offset).
  `xkb_state_key_get_one_sym` / `..._get_utf8`; modifiers via
  `xkb_state_mod_name_is_active(st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE)`. **Key repeat is the
  client's job** — arm a `timerfd` from `repeat_info(rate,delay)` if `xkb_keymap_key_repeats`.
- **Hit-testing:** widget rects in surface-local coords; on `motion` find topmost containing rect →
  hover (emit enter/leave on change); press sets `pressed=hovered`; release fires click if
  `release-hovered==pressed`; scroll routes to hovered. Commit on pointer `frame`.
- **Cursor:** `wl_cursor_theme_load`→`get_cursor`→`wl_pointer_set_cursor(pointer, enter_serial, ...)`
  (needs the serial from `enter`), scaled by output scale. Or `cursor-shape-v1` if niri exposes it.

## 7. Extra Wayland protocols (bind-or-skip; niri 25.11 supports all)

- **ext-session-lock-v1** (lock): `lock()` → on `locked`, one `get_lock_surface(surface,output)` per
  output (+ hotplug); on surface `configure` ack + render exactly `w×h`; after PAM auth
  `unlock_and_destroy()`. Commit before first ack = protocol error.
- **ext-idle-notify-v1** + **idle-inhibit**: `get_idle_notification(timeout_ms, seat)` → `idled`/
  `resumed`. Inhibit via `zwp_idle_inhibit_manager_v1.create_inhibitor(surface)` (effective while
  surface visible). (niri lacks `org_kde_kwin_idle` — use ext-idle-notify.)
- **wlr-screencopy** (screenshot / color pick): `capture_output` → `buffer`/`buffer_done` → wl_shm
  buffer → `copy` → `ready` (honor `Y_INVERT`). Prefer **ext-image-copy-capture-v1** (toplevel/cursor),
  fall back to wlr-screencopy.
- **wlr-gamma-control** (night mode): `get_gamma_control(output)` → `gamma_size` → memfd of 3×N uint16
  ramps → `set_gamma`. One client at a time.
- **wlr/ext-data-control** (clipboard): `get_data_device(seat)` → `selection`/`primary_selection`;
  read via `offer.receive(mime,fd)` (drain fd promptly); set via `create_data_source`→`set_selection`.
  Prefer **ext-data-control-v1**, fall back to wlr.

## 8. Animation engine (DMS-identical, user-configurable)

Driven from the `wl_surface.frame` `done` callback (timestamp is ms, deltas only). Compute `dt` and
**clamp** (`dt=min(dt,0.05)`) so a hidden→shown gap doesn't teleport. Re-arm the callback only while any
tween is active.

```c
typedef float (*easing_fn)(float t);           // t∈[0,1] → eased (may overshoot)
typedef struct { float from,to,duration,elapsed,value; easing_fn ease; bool active; } tween_t;
bool tween_update(tween_t*tw,float dt){ if(!tw->active) return false;
  tw->elapsed+=dt; float p = tw->duration>0 ? tw->elapsed/tw->duration : 1.f;
  if(p>=1.f){p=1.f;tw->active=false;} tw->value=tw->from+(tw->to-tw->from)*(tw->ease?tw->ease(p):p);
  return tw->active; }
```

- **Easing:** Penner set (`out_cubic(t){float f=t-1;return f*f*f+1;}` …). CSS cubic-bezier: Newton-solve
  `u` for `x` then eval `y`; for the M3 two-segment **Emphasized** curve use a **256-sample LUT**
  (also cheaper per frame). Springs via semi-implicit Euler when overshoot is wanted.
- **M3 motion tokens (cubic-bezier, P0=(0,0) P3=(1,1)):** Standard `(0.2,0,0,1)`,
  Standard-decelerate `(0,0,0,1)`, Standard-accelerate `(0.3,0,1,1)`,
  Emphasized-decelerate `(0.05,0.7,0.1,1)`, Emphasized-accelerate `(0.3,0,0.8,0.15)`; Emphasized =
  two-segment spline (LUT).

**Port DMS's exact duration model** (this is the "same as DMS but user-settable" requirement). Config
key `animationSpeed` ∈ {None,Short,Medium,Long,Custom} indexes a 5-tier table
(shorter/short/medium/long/extraLong):

| speed | shorter | short | medium | long | extraLong |
|---|---|---|---|---|---|
| None | 0 | 0 | 0 | 0 | 0 |
| Short | 50 | 75 | 150 | 250 | 500 |
| Medium | 100 | 150 | 300 | 500 | 1000 |
| Long | 150 | 225 | 450 | 750 | 1500 |

`Custom` collapses all tiers to `customAnimationDuration`. Separate axes: `popoutAnimationSpeed`,
`modalAnimationSpeed`, `notificationAnimationSpeed` (notif base preset `[0,200,400,600]`, scaled
enter×0.875 / exit×0.75 / expand×1.0 / collapse×0.85). `syncComponentAnimationSpeeds` ties them to the
main speed. `enableRippleEffects`, `animationVariant`, `motionEffect` toggles too. Standard easing
`OutCubic`, emphasized `OutQuart`. Full table + keys: `04-FEATURES.md §config`.

## 9. Reference projects to learn from

`swaybg` (smallest layer-shell client), `foot` (gold-standard minimal C event loop + damage),
`yambar` (raw-C bar, pixman/fcft — architecture, not GL), `Waybar`/`sfwbar` (module/feature design,
but GTK), `swaylock`/`gtklock` (lock structure), `nanovg` (the draw layer), LVGL v9.5 (precedent for
EGL+nanovg on Wayland). **No existing raw-C GL layer-shell bar to copy wholesale** — take the event
loop from foot/yambar, layer-shell plumbing from swaybg, render with nanovg + the tween engine above.
