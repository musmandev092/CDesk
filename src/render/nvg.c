#include "render/nvg.h"

#include "core/log.h"

#include <GLES3/gl3.h>
#include <unistd.h>

#include "nanovg.h"
#define NANOVG_GLES3
#include "nanovg_gl.h"

/* Searched in order; first existing wins. Inter is the DankC UI font (bundled),
 * with system fallbacks for a bare dev checkout. */
static const char *const FONT_CANDIDATES[] = {
    "assets/fonts/InterVariable.ttf",
    "/usr/share/dankc/fonts/InterVariable.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
};

static int load_ui_font(NVGcontext *vg)
{
    for (size_t i = 0; i < sizeof(FONT_CANDIDATES) / sizeof(FONT_CANDIDATES[0]); i++) {
        if (access(FONT_CANDIDATES[i], R_OK) != 0)
            continue;
        int font = nvgCreateFont(vg, "ui", FONT_CANDIDATES[i]);
        if (font >= 0) {
            dc_debug("loaded UI font: %s", FONT_CANDIDATES[i]);
            return font;
        }
    }
    dc_error("no usable UI font found");
    return -1;
}

bool dc_render_ensure(dc_render *render)
{
    if (render->ready)
        return true;

    render->vg = nvgCreateGLES3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!render->vg) {
        dc_error("nvgCreateGLES3 failed");
        return false;
    }

    render->font_ui = load_ui_font(render->vg);
    if (render->font_ui < 0) {
        nvgDeleteGLES3(render->vg);
        render->vg = NULL;
        return false;
    }

    render->ready = true;
    dc_debug("nanovg render context ready");
    return true;
}

void dc_render_finish(dc_render *render)
{
    if (render->vg)
        nvgDeleteGLES3(render->vg);
    render->vg = NULL;
    render->ready = false;
}
