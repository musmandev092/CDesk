// dynamic.cpp — wallpaper -> Material palette. The only C++ translation unit in
// DankC (Material colour math). Compiled with a private static copy of stb_image
// so it doesn't clash with nanovg's copy.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wshift-negative-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wtype-limits"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
#pragma GCC diagnostic pop

#include "theme/dynamic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace {

struct Rgb {
    float r, g, b; // 0..1
};

struct Hsl {
    float h; // 0..360
    float s; // 0..1
    float l; // 0..1
};

Hsl rgb_to_hsl(Rgb c)
{
    float max = std::max({c.r, c.g, c.b});
    float min = std::min({c.r, c.g, c.b});
    float l = (max + min) * 0.5f;
    float h = 0.0f, s = 0.0f;
    float d = max - min;
    if (d > 1e-6f) {
        s = l > 0.5f ? d / (2.0f - max - min) : d / (max + min);
        if (max == c.r)
            h = (c.g - c.b) / d + (c.g < c.b ? 6.0f : 0.0f);
        else if (max == c.g)
            h = (c.b - c.r) / d + 2.0f;
        else
            h = (c.r - c.g) / d + 4.0f;
        h *= 60.0f;
    }
    return {h, s, l};
}

float hue_channel(float p, float q, float t)
{
    if (t < 0.0f)
        t += 1.0f;
    if (t > 1.0f)
        t -= 1.0f;
    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

dc_color hsl_to_color(float h, float s, float l)
{
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 360.0f;
    s = std::clamp(s, 0.0f, 1.0f);
    l = std::clamp(l, 0.0f, 1.0f);
    float r, g, b;
    if (s < 1e-6f) {
        r = g = b = l;
    } else {
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        float p = 2.0f * l - q;
        r = hue_channel(p, q, h + 1.0f / 3.0f);
        g = hue_channel(p, q, h);
        b = hue_channel(p, q, h - 1.0f / 3.0f);
    }
    auto u8 = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return dc_color{u8(r), u8(g), u8(b), 255};
}

// Pick a vibrant, well-populated seed colour: histogram in a coarse RGB grid,
// scored by population * chroma (matugen/MCU also weight toward chroma).
Rgb pick_seed(const unsigned char *px, int w, int h, int stride)
{
    std::unordered_map<uint32_t, uint32_t> hist;
    hist.reserve(4096);
    const int step = std::max(1, (w * h) / 20000); // sample ~20k pixels
    int idx = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++, idx++) {
            if (idx % step != 0)
                continue;
            const unsigned char *p = px + (size_t)y * stride + (size_t)x * 3;
            uint32_t key = ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3); // 5 bits/chan
            hist[key]++;
        }
    }

    float best_score = -1.0f;
    Rgb best{0.5f, 0.5f, 0.5f};
    Rgb fallback{0.5f, 0.5f, 0.5f};
    uint32_t fallback_pop = 0;
    for (auto &kv : hist) {
        uint32_t k = kv.first;
        Rgb c{(float)(((k >> 10) & 31) << 3) / 255.0f, (float)(((k >> 5) & 31) << 3) / 255.0f,
              (float)((k & 31) << 3) / 255.0f};
        if (kv.second > fallback_pop) {
            fallback_pop = kv.second;
            fallback = c;
        }
        Hsl hsl = rgb_to_hsl(c);
        // Ignore near-black / near-white / washed-out for the vibrant pick.
        if (hsl.l < 0.12f || hsl.l > 0.9f || hsl.s < 0.2f)
            continue;
        float score = (float)kv.second * (0.25f + hsl.s);
        if (score > best_score) {
            best_score = score;
            best = c;
        }
    }
    return best_score < 0.0f ? fallback : best;
}

} // namespace

extern "C" bool dc_dynamic_from_image(const char *image_path, dc_theme *out)
{
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(image_path, &w, &h, &n, 3);
    if (!px)
        return false;

    Rgb seed = pick_seed(px, w, h, w * 3);
    stbi_image_free(px);

    Hsl s = rgb_to_hsl(seed);
    float H = s.h;
    float chroma = std::clamp(s.s, 0.35f, 0.85f);

    // Dark Material-style scheme (tones approximated in HSL lightness). Surfaces
    // are subtly tinted with the seed hue; primary is a light vivid accent.
    dc_theme t{};
    t.primary = hsl_to_color(H, chroma * 0.9f, 0.72f);
    t.primary_text = hsl_to_color(H, 0.35f, 0.12f);
    t.primary_container = hsl_to_color(H, chroma * 0.7f, 0.28f);
    t.secondary = hsl_to_color(H, 0.35f, 0.68f);
    t.surface = hsl_to_color(H, 0.12f, 0.07f);
    t.surface_text = hsl_to_color(H, 0.10f, 0.90f);
    t.surface_variant = hsl_to_color(H, 0.12f, 0.28f);
    t.surface_variant_text = hsl_to_color(H, 0.10f, 0.78f);
    t.background = t.surface;
    t.background_text = t.surface_text;
    t.outline = hsl_to_color(H, 0.10f, 0.55f);
    t.surface_container_lowest = hsl_to_color(H, 0.14f, 0.05f);
    t.surface_container_low = hsl_to_color(H, 0.14f, 0.09f);
    t.surface_container = hsl_to_color(H, 0.14f, 0.11f);
    t.surface_container_high = hsl_to_color(H, 0.13f, 0.15f);
    t.surface_container_highest = hsl_to_color(H, 0.13f, 0.19f);
    t.error = dc_color{0xf4, 0x43, 0x36, 255};
    t.warning = dc_color{0xff, 0x98, 0x00, 255};
    t.info = dc_color{0x21, 0x96, 0xf3, 255};
    t.success = dc_color{0x4c, 0xaf, 0x50, 255};

    *out = t;
    return true;
}
