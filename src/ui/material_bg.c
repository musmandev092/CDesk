#include "ui/material_bg.h"

#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/nvg.h"
#include "theme/theme.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nanovg.h"

/* Declarations only (no STB_IMAGE_IMPLEMENTATION) — nanovg.c already provides
 * the stbi_load/stbi_image_free symbols this TU links against, same as
 * dashboard.c's wallpaper-thumbnail decoder. */
#include "stb_image.h"

/* "downscale aggressively (e.g. to 1/8)" (docs/POLISH.md P2). */
#define DC_MATERIAL_BG_DIVISOR 8
/* Cap on the cached texture's long edge: keeps decode+box-sample+blur well
 * under the 100ms panel-open budget regardless of the source wallpaper's
 * resolution, and keeps the GPU upload tiny. */
#define DC_MATERIAL_BG_MAX_DIM 200
#define DC_MATERIAL_BG_MIN_DIM 8

/* Themed scrim drawn over the blurred image so panel text stays legible —
 * the "dim it" step (docs/POLISH.md P2). Alpha out of 255. */
#define DC_MATERIAL_BG_SCRIM_ALPHA 150

/* One process-wide cache: the source is already blurred past the point where
 * per-panel/per-output accuracy matters (see material_bg.h). */
static int cached_image = 0;
static NVGcontext *cached_vg = NULL;
static char cached_path[DC_CONFIG_PATH_MAX];
static bool cached_failed = false; /* last attempt at cached_path failed to decode -- don't retry every frame */

void dc_material_bg_invalidate(void)
{
    if (cached_vg && cached_image > 0)
        nvgDeleteImage(cached_vg, cached_image);
    cached_image = 0;
    cached_vg = NULL;
    cached_path[0] = '\0';
    cached_failed = false;
}

/* In-place 3x3 box blur (edge-clamped) on a tiny RGBA8 buffer. Two passes
 * approximate a gaussian well enough at this size and stay cheap since the
 * buffer is already capped at DC_MATERIAL_BG_MAX_DIM per edge. */
static void box_blur_pass(unsigned char *buf, int w, int h)
{
    unsigned char *tmp = malloc((size_t)w * (size_t)h * 4);
    if (!tmp)
        return;
    memcpy(tmp, buf, (size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned acc[4] = {0, 0, 0, 0};
            int cnt = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int sy = y + dy;
                if (sy < 0)
                    sy = 0;
                if (sy >= h)
                    sy = h - 1;
                for (int dx = -1; dx <= 1; dx++) {
                    int sx = x + dx;
                    if (sx < 0)
                        sx = 0;
                    if (sx >= w)
                        sx = w - 1;
                    const unsigned char *p = tmp + ((size_t)sy * (size_t)w + (size_t)sx) * 4;
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    acc[3] += p[3];
                    cnt++;
                }
            }
            unsigned char *o = buf + ((size_t)y * (size_t)w + (size_t)x) * 4;
            o[0] = (unsigned char)(acc[0] / (unsigned int)cnt);
            o[1] = (unsigned char)(acc[1] / (unsigned int)cnt);
            o[2] = (unsigned char)(acc[2] / (unsigned int)cnt);
            o[3] = (unsigned char)(acc[3] / (unsigned int)cnt);
        }
    }
    free(tmp);
}

/* Decode `path`, box-sample it down by DC_MATERIAL_BG_DIVISOR (clamped to
 * DC_MATERIAL_BG_MAX_DIM), then blur the tiny result. Returns an nvg image
 * handle (>0) or 0 on any failure (missing file, unsupported format, OOM). */
static int build_image(dc_render *render, const char *path)
{
    int sw = 0, sh = 0, n = 0;
    unsigned char *src = stbi_load(path, &sw, &sh, &n, 4);
    if (!src || sw <= 0 || sh <= 0) {
        if (src)
            stbi_image_free(src);
        return 0;
    }

    int tw = sw / DC_MATERIAL_BG_DIVISOR;
    int th = sh / DC_MATERIAL_BG_DIVISOR;
    if (tw > DC_MATERIAL_BG_MAX_DIM || th > DC_MATERIAL_BG_MAX_DIM) {
        float s = (float)DC_MATERIAL_BG_MAX_DIM / (float)(tw > th ? tw : th);
        tw = (int)((float)tw * s);
        th = (int)((float)th * s);
    }
    if (tw < DC_MATERIAL_BG_MIN_DIM)
        tw = DC_MATERIAL_BG_MIN_DIM;
    if (th < DC_MATERIAL_BG_MIN_DIM)
        th = DC_MATERIAL_BG_MIN_DIM;

    unsigned char *dst = malloc((size_t)tw * (size_t)th * 4);
    if (!dst) {
        stbi_image_free(src);
        return 0;
    }

    /* Box-sample downscale (same algorithm as dashboard.c's wallpaper
     * thumbnails) -- averaging every source texel into its destination cell
     * is itself most of the blur; the box_blur_pass() calls below just
     * smooth the remaining cell-sized blockiness. */
    for (int y = 0; y < th; y++) {
        int sy0 = (int)((long)y * sh / th);
        int sy1 = (int)((long)(y + 1) * sh / th);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < tw; x++) {
            int sx0 = (int)((long)x * sw / tw);
            int sx1 = (int)((long)(x + 1) * sw / tw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            unsigned acc[4] = {0, 0, 0, 0};
            int cnt = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const unsigned char *row = src + ((size_t)yy * (size_t)sw + (size_t)sx0) * 4;
                for (int xx = sx0; xx < sx1; xx++, row += 4) {
                    acc[0] += row[0];
                    acc[1] += row[1];
                    acc[2] += row[2];
                    acc[3] += row[3];
                    cnt++;
                }
            }
            unsigned char *o = dst + ((size_t)y * (size_t)tw + (size_t)x) * 4;
            o[0] = (unsigned char)(acc[0] / (unsigned int)cnt);
            o[1] = (unsigned char)(acc[1] / (unsigned int)cnt);
            o[2] = (unsigned char)(acc[2] / (unsigned int)cnt);
            o[3] = (unsigned char)(acc[3] / (unsigned int)cnt);
        }
    }
    stbi_image_free(src);

    box_blur_pass(dst, tw, th);
    box_blur_pass(dst, tw, th);

    int img = nvgCreateImageRGBA(render->vg, tw, th, 0, dst);
    free(dst);
    return img;
}

/* Ensure the cache reflects the current config's wallpaper; returns the
 * cached image handle, or 0 if disabled/unavailable. */
static int ensure_image(dc_render *render)
{
    const dc_config *cfg = dc_config_current;
    if (!cfg->material_blur)
        return 0;
    if (!cfg->wallpaper[0] || cfg->wallpaper[0] == '#')
        return 0; /* no image configured (e.g. a solid-color "wallpaper") */

    if (cached_image > 0 && cached_vg == render->vg && strcmp(cached_path, cfg->wallpaper) == 0)
        return cached_image;
    if (cached_failed && cached_vg == render->vg && strcmp(cached_path, cfg->wallpaper) == 0)
        return 0; /* already tried and failed to decode this path -- don't retry every draw */

    if (cached_image > 0 && cached_vg)
        nvgDeleteImage(cached_vg, cached_image);
    cached_image = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int img = build_image(render, cfg->wallpaper);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    dc_debug("material bg: built from %s in %.1fms (image=%d)", cfg->wallpaper, ms, img);

    cached_vg = render->vg;
    snprintf(cached_path, sizeof(cached_path), "%s", cfg->wallpaper);
    if (img <= 0) {
        /* Missing/unreadable wallpaper: fall back to the flat fill, quietly
         * (this is a normal, common state -- no wallpaper configured yet). */
        cached_failed = true;
        return 0;
    }
    cached_failed = false;
    cached_image = img;
    return cached_image;
}

void dc_material_bg_fill_card_varying(NVGcontext *vg, dc_render *render, float x, float y, float w,
                                      float h, float r_tl, float r_tr, float r_br, float r_bl)
{
    const dc_theme *t = dc_theme_current;
    int img = render ? ensure_image(render) : 0;

    nvgBeginPath(vg);
    nvgRoundedRectVarying(vg, x, y, w, h, r_tl, r_tr, r_br, r_bl);
    if (img > 0) {
        /* Stretch-fit: the source is already blurred well past the point
         * where aspect-ratio distortion is visible. */
        NVGpaint pat = nvgImagePattern(vg, x, y, w, h, 0.0f, img, 1.0f);
        nvgFillPaint(vg, pat);
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgRoundedRectVarying(vg, x, y, w, h, r_tl, r_tr, r_br, r_bl);
        nvgFillColor(vg, nvgRGBA(t->surface_container.r, t->surface_container.g,
                                 t->surface_container.b, DC_MATERIAL_BG_SCRIM_ALPHA));
        nvgFill(vg);
    } else {
        nvgFillColor(vg, nvgRGBA(t->surface_container.r, t->surface_container.g,
                                 t->surface_container.b, 255));
        nvgFill(vg);
    }
}

void dc_material_bg_fill_card(NVGcontext *vg, dc_render *render, float x, float y, float w, float h,
                              float radius)
{
    dc_material_bg_fill_card_varying(vg, render, x, y, w, h, radius, radius, radius, radius);
}
