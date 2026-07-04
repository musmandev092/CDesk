/* Link-only stubs for tests/test_text_edit.c.
 *
 * tests/test_text_edit.c exercises ONLY text_edit.c's pure buffer ops (UTF-8
 * boundary nav, insert/delete at cursor, buffer growth/cap, cursor
 * clamping) -- none of which touch nanovg/GL, per text_edit.h's "pure buffer
 * ops" section. But text_edit.c is one translation unit that *also* contains
 * the EGL-dependent layout/cursor-math/draw code (te_ensure_layout() and
 * everything built on it), which references real nanovg entry points,
 * dc_shape_draw_text(), dc_theme_current, and dc_anim_now_ms(). Linking
 * text_edit.o directly (not pulled a symbol at a time out of an archive)
 * requires every one of those references to resolve, even though this test
 * binary never actually calls into that code path.
 *
 * Rather than link the real nanovg/HarfBuzz/Fontconfig/theme stack just to
 * satisfy the linker (defeating the point of a GL-free test), this file
 * provides trivial no-op stand-ins -- keeping bin/test_text_edit buildable
 * and runnable with zero GL/EGL/HarfBuzz/Fontconfig dependency, exactly like
 * bin/test_calc's standalone precedent. */
#include "core/anim.h"
#include "render/shape.h"
#include "theme/theme.h"

#include "nanovg.h"

#include <time.h>

void nvgFontFaceId(NVGcontext *ctx, int font)
{
    (void)ctx;
    (void)font;
}
void nvgFontSize(NVGcontext *ctx, float size)
{
    (void)ctx;
    (void)size;
}
void nvgTextLineHeight(NVGcontext *ctx, float lineHeight)
{
    (void)ctx;
    (void)lineHeight;
}
void nvgTextAlign(NVGcontext *ctx, int align)
{
    (void)ctx;
    (void)align;
}
int nvgTextBreakLines(NVGcontext *ctx, const char *string, const char *end, float breakRowWidth,
                      NVGtextRow *rows, int maxRows)
{
    (void)ctx;
    (void)string;
    (void)end;
    (void)breakRowWidth;
    (void)rows;
    (void)maxRows;
    return 0;
}
int nvgTextGlyphPositions(NVGcontext *ctx, float x, float y, const char *string, const char *end,
                          NVGglyphPosition *positions, int maxPositions)
{
    (void)ctx;
    (void)x;
    (void)y;
    (void)string;
    (void)end;
    (void)positions;
    (void)maxPositions;
    return 0;
}
void nvgSave(NVGcontext *ctx)
{
    (void)ctx;
}
void nvgRestore(NVGcontext *ctx)
{
    (void)ctx;
}
void nvgScissor(NVGcontext *ctx, float x, float y, float w, float h)
{
    (void)ctx;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
void nvgBeginPath(NVGcontext *ctx)
{
    (void)ctx;
}
void nvgRect(NVGcontext *ctx, float x, float y, float w, float h)
{
    (void)ctx;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}
void nvgRoundedRect(NVGcontext *ctx, float x, float y, float w, float h, float r)
{
    (void)ctx;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)r;
}
void nvgFillColor(NVGcontext *ctx, NVGcolor color)
{
    (void)ctx;
    (void)color;
}
void nvgFill(NVGcontext *ctx)
{
    (void)ctx;
}
float nvgText(NVGcontext *ctx, float x, float y, const char *string, const char *end)
{
    (void)ctx;
    (void)y;
    (void)string;
    (void)end;
    return x;
}
NVGcolor nvgRGBA(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    NVGcolor c;
    c.r = (float)r / 255.0f;
    c.g = (float)g / 255.0f;
    c.b = (float)b / 255.0f;
    c.a = (float)a / 255.0f;
    return c;
}

float dc_shape_draw_text(dc_render *render, float x, float y, const char *text, const char *end)
{
    (void)render;
    (void)y;
    (void)text;
    (void)end;
    return x;
}

static const dc_theme g_stub_theme;
const dc_theme *dc_theme_current = &g_stub_theme;

int64_t dc_anim_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
