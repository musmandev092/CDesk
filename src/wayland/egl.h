/* egl.h — EGL/OpenGL ES bring-up on Wayland.
 *
 * dc_egl is the process-wide EGL display/context (created once). dc_egl_window
 * wraps one on-screen surface (one per layer surface). The GL context is shared
 * across all windows.
 */
#ifndef DC_WAYLAND_EGL_H
#define DC_WAYLAND_EGL_H

#include <EGL/egl.h>
#include <stdbool.h>
#include <wayland-client.h>

typedef struct dc_egl {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
} dc_egl;

typedef struct dc_egl_window {
    struct wl_egl_window *native;
    EGLSurface surface;
    int width;
    int height;
} dc_egl_window;

/* Initialise the shared EGL display + context. Returns false on failure. */
bool dc_egl_init(dc_egl *egl, struct wl_display *wl_display);
void dc_egl_finish(dc_egl *egl);

/* Create a GL-backed surface `width`x`height` (physical pixels) on `wl_surface`. */
bool dc_egl_window_init(dc_egl_window *win, dc_egl *egl, struct wl_surface *wl_surface, int width,
                        int height);
void dc_egl_window_finish(dc_egl_window *win, dc_egl *egl);
void dc_egl_window_resize(dc_egl_window *win, int width, int height);

bool dc_egl_make_current(dc_egl *egl, dc_egl_window *win);
/* Attaches the rendered buffer and commits the wl_surface. */
void dc_egl_swap(dc_egl *egl, dc_egl_window *win);

#endif /* DC_WAYLAND_EGL_H */
