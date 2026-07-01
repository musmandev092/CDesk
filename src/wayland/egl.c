#include "wayland/egl.h"

#include "core/log.h"

#include <EGL/eglext.h>
#include <wayland-egl.h>

bool dc_egl_init(dc_egl *egl, struct wl_display *wl_display)
{
    egl->display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display, NULL);
    if (egl->display == EGL_NO_DISPLAY) {
        dc_error("eglGetPlatformDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl->display, &major, &minor)) {
        dc_error("eglInitialize failed");
        return false;
    }
    dc_debug("EGL %d.%d initialised", major, minor);

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        dc_error("eglBindAPI(OpenGL ES) failed");
        return false;
    }

    /* Stencil is required by nanovg; alpha lets the panel be translucent. */
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_STENCIL_SIZE,    8,
        EGL_NONE,
    };
    EGLint num_configs = 0;
    if (!eglChooseConfig(egl->display, config_attribs, &egl->config, 1, &num_configs) ||
        num_configs == 0) {
        dc_error("eglChooseConfig found no matching config");
        return false;
    }

    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_NONE,
    };
    egl->context =
        eglCreateContext(egl->display, egl->config, EGL_NO_CONTEXT, context_attribs);
    if (egl->context == EGL_NO_CONTEXT) {
        dc_error("eglCreateContext failed");
        return false;
    }

    return true;
}

void dc_egl_finish(dc_egl *egl)
{
    if (egl->context != EGL_NO_CONTEXT)
        eglDestroyContext(egl->display, egl->context);
    if (egl->display != EGL_NO_DISPLAY)
        eglTerminate(egl->display);
}

bool dc_egl_window_init(dc_egl_window *win, dc_egl *egl, struct wl_surface *wl_surface, int width,
                        int height)
{
    win->width = width;
    win->height = height;
    win->native = wl_egl_window_create(wl_surface, width, height);
    if (!win->native) {
        dc_error("wl_egl_window_create failed");
        return false;
    }

    win->surface = eglCreatePlatformWindowSurface(egl->display, egl->config, win->native, NULL);
    if (win->surface == EGL_NO_SURFACE) {
        dc_error("eglCreatePlatformWindowSurface failed");
        wl_egl_window_destroy(win->native);
        win->native = NULL;
        return false;
    }
    return true;
}

void dc_egl_window_finish(dc_egl_window *win, dc_egl *egl)
{
    if (win->surface != EGL_NO_SURFACE)
        eglDestroySurface(egl->display, win->surface);
    if (win->native)
        wl_egl_window_destroy(win->native);
    win->surface = EGL_NO_SURFACE;
    win->native = NULL;
}

void dc_egl_window_resize(dc_egl_window *win, int width, int height)
{
    if (width == win->width && height == win->height)
        return;
    win->width = width;
    win->height = height;
    wl_egl_window_resize(win->native, width, height, 0, 0);
}

bool dc_egl_make_current(dc_egl *egl, dc_egl_window *win)
{
    if (!eglMakeCurrent(egl->display, win->surface, win->surface, egl->context)) {
        dc_error("eglMakeCurrent failed");
        return false;
    }
    /* Pace with frame callbacks, not by blocking on swap. */
    eglSwapInterval(egl->display, 0);
    return true;
}

void dc_egl_swap(dc_egl *egl, dc_egl_window *win)
{
    eglSwapBuffers(egl->display, win->surface);
}
