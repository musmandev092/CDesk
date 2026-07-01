#include "render/nvg.h"

#include "core/log.h"

#include <GLES3/gl3.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "nanosvg.h"
#include "nanosvgrast.h"
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

static const char *const ICON_FONT_CANDIDATES[] = {
    "assets/fonts/MaterialSymbolsRounded.ttf",
    "/usr/share/dankc/fonts/MaterialSymbolsRounded.ttf",
};

static int load_font(NVGcontext *vg, const char *name, const char *const *candidates, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (access(candidates[i], R_OK) != 0)
            continue;
        int font = nvgCreateFont(vg, name, candidates[i]);
        if (font >= 0) {
            dc_debug("loaded font '%s': %s", name, candidates[i]);
            return font;
        }
    }
    return -1;
}

/* Encode a Unicode codepoint as UTF-8. Returns the byte count. */
static int utf8_encode(int cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
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

    render->font_ui =
        load_font(render->vg, "ui", FONT_CANDIDATES, sizeof(FONT_CANDIDATES) / sizeof(char *));
    if (render->font_ui < 0) {
        dc_error("no usable UI font found");
        nvgDeleteGLES3(render->vg);
        render->vg = NULL;
        return false;
    }

    render->font_icons = load_font(render->vg, "icons", ICON_FONT_CANDIDATES,
                                   sizeof(ICON_FONT_CANDIDATES) / sizeof(char *));
    if (render->font_icons < 0)
        dc_warn("Material Symbols icon font not found; icons disabled");

    render->ready = true;
    dc_debug("nanovg render context ready");
    return true;
}

void dc_render_icon(dc_render *render, int codepoint, float x, float y, float size, dc_color color,
                    int align_nvg)
{
    if (!render->vg || render->font_icons < 0)
        return;

    char glyph[5];
    int len = utf8_encode(codepoint, glyph);
    glyph[len] = '\0';

    nvgFontFaceId(render->vg, render->font_icons);
    nvgFontSize(render->vg, size);
    nvgFillColor(render->vg, nvgRGBA(color.r, color.g, color.b, color.a));
    nvgTextAlign(render->vg, align_nvg);
    nvgText(render->vg, x, y, glyph, NULL);
}

int dc_render_load_icon(dc_render *render, const char *path, int size)
{
    if (!render->vg || !path || size <= 0)
        return 0;

    size_t len = strlen(path);
    if (len > 4 && strcasecmp(path + len - 4, ".svg") == 0) {
        NSVGimage *svg = nsvgParseFromFile(path, "px", 96.0f);
        if (!svg)
            return 0;
        float src = svg->width > svg->height ? svg->width : svg->height;
        float scale = src > 0.0f ? (float)size / src : 1.0f;

        unsigned char *pixels = calloc((size_t)size * size * 4, 1);
        if (!pixels) {
            nsvgDelete(svg);
            return 0;
        }
        NSVGrasterizer *rast = nsvgCreateRasterizer();
        nsvgRasterize(rast, svg, 0.0f, 0.0f, scale, pixels, size, size, size * 4);
        int handle = nvgCreateImageRGBA(render->vg, size, size, 0, pixels);
        nsvgDeleteRasterizer(rast);
        free(pixels);
        nsvgDelete(svg);
        return handle;
    }

    return nvgCreateImage(render->vg, path, 0);
}

void dc_render_finish(dc_render *render)
{
    if (render->vg)
        nvgDeleteGLES3(render->vg);
    render->vg = NULL;
    render->ready = false;
}
