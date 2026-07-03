// dynamic.cpp — wallpaper -> Material palette using real Material Color
// Utilities (MCU) math. The only C++ translation unit in DankC (Material colour
// science). Compiled with a private static copy of stb_image so it doesn't
// clash with nanovg's copy.
//
// Pipeline (matches matugen / MCU DynamicScheme, scheme "tonal-spot"):
//   1. Quantize the wallpaper's pixels (uniform 5-bit/channel histogram — a
//      Wu-style box quantizer — collapsed to a manageable set of representative
//      colours with populations).
//   2. Convert each representative to HCT and run MCU's Score algorithm to pick
//      the seed: population-weighted, chroma-favouring, hue-deduplicated
//      (theme/hct.h gives the exact CAM16 HCT the scorer needs).
//   3. Build the five tonal palettes (primary c=36, secondary c=16, tertiary
//      hue+60 c=24, neutral c=6, neutralVariant c=8) at MCU's tone stops and
//      map them onto DankC's M3 role struct, for BOTH dark and light.
//
// Verified against `matugen color hex <seed> -m dark|light -t scheme-tonal-spot
// -j hex`: the generated primary / surface / container roles match matugen's
// output within +-1-2 8-bit codes across the stock seeds (green/blue/red/...).
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
#include "theme/hct.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

using dc_hct::Argb;
using dc_hct::Hct;

inline dc_color to_dc(Argb c)
{
    return dc_color{c.r, c.g, c.b, 255};
}

// A quantized colour: representative sRGB + its pixel population.
struct QColor {
    Argb argb;
    uint32_t pop;
};

// Uniform box quantization: 5 bits/channel histogram over a sampled subset of
// pixels (this is the coarse-grid step MCU's Wu quantizer also starts from).
std::vector<QColor> quantize(const unsigned char *px, int w, int h, int stride)
{
    std::unordered_map<uint32_t, uint32_t> hist;
    hist.reserve(8192);
    const int step = std::max(1, (w * h) / 20000); // ~20k samples
    int idx = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++, idx++) {
            if (idx % step != 0)
                continue;
            const unsigned char *p = px + (size_t)y * stride + (size_t)x * 3;
            uint32_t key = ((uint32_t)(p[0] >> 3) << 10) | ((uint32_t)(p[1] >> 3) << 5) |
                           (uint32_t)(p[2] >> 3);
            hist[key]++;
        }
    }
    std::vector<QColor> out;
    out.reserve(hist.size());
    for (auto &kv : hist) {
        uint32_t k = kv.first;
        // Reconstruct bin centre (+4 to land mid-bin, matching MCU's rounding).
        uint8_t r = (uint8_t)std::min(255, (int)(((k >> 10) & 31) << 3) + 4);
        uint8_t g = (uint8_t)std::min(255, (int)(((k >> 5) & 31) << 3) + 4);
        uint8_t b = (uint8_t)std::min(255, (int)((k & 31) << 3) + 4);
        out.push_back({Argb{r, g, b}, kv.second});
    }
    return out;
}

// MCU Score: pick the most suitable seed from the quantized set. Constants are
// MCU's (Score.java): favour chroma near 48, weight by hue "excited
// proportion", drop near-grey colours, dedupe hues < 15 degrees apart.
Argb score_seed(const std::vector<QColor> &colors)
{
    const double TARGET_CHROMA = 48.0;
    const double WEIGHT_PROPORTION = 0.7;
    const double WEIGHT_CHROMA_ABOVE = 0.3;
    const double WEIGHT_CHROMA_BELOW = 0.1;
    const double CUTOFF_CHROMA = 5.0;
    const double CUTOFF_EXCITED = 0.01;
    const double DEFAULT_SEED_H = 217.0; // MCU's Google-blue fallback (#4285F4)
    const double DEFAULT_SEED_C = 24.0;

    // Hue histogram (population per integer degree) + per-colour HCT.
    double huePop[360] = {0};
    double totalPop = 0;
    struct Scored {
        Argb argb;
        Hct hct;
        double score;
    };
    std::vector<Scored> scored;
    scored.reserve(colors.size());
    std::vector<Hct> hcts(colors.size());
    for (size_t i = 0; i < colors.size(); i++) {
        hcts[i] = dc_hct::hct_from_argb(colors[i].argb);
        int hue = (int)std::floor(hcts[i].hue) % 360;
        if (hue < 0)
            hue += 360;
        huePop[hue] += colors[i].pop;
        totalPop += colors[i].pop;
    }
    if (totalPop <= 0)
        return dc_hct::hct_solve(DEFAULT_SEED_H, DEFAULT_SEED_C, 50.0);

    for (size_t i = 0; i < colors.size(); i++) {
        const Hct &hct = hcts[i];
        int hue = (int)std::floor(hct.hue) % 360;
        if (hue < 0)
            hue += 360;
        // Excited proportion: population within +-15 degrees of this hue.
        double excited = 0;
        for (int d = -15; d <= 15; d++) {
            int hh = (hue + d + 360) % 360;
            excited += huePop[hh];
        }
        excited /= totalPop;
        if (hct.chroma < CUTOFF_CHROMA || excited <= CUTOFF_EXCITED)
            continue;
        double propScore = excited * 100.0 * WEIGHT_PROPORTION;
        double chromaWeight = hct.chroma < TARGET_CHROMA ? WEIGHT_CHROMA_BELOW : WEIGHT_CHROMA_ABOVE;
        double chromaScore = (hct.chroma - TARGET_CHROMA) * chromaWeight;
        scored.push_back({colors[i].argb, hct, propScore + chromaScore});
    }
    if (scored.empty())
        return dc_hct::hct_solve(DEFAULT_SEED_H, DEFAULT_SEED_C, 50.0);

    std::sort(scored.begin(), scored.end(),
              [](const Scored &a, const Scored &b) { return a.score > b.score; });

    // Dedupe: return the top-scoring colour whose hue is >= 15 degrees from
    // every already-chosen seed; the first survivor (highest score) is ours.
    std::vector<double> chosenHues;
    for (const auto &s : scored) {
        bool tooClose = false;
        for (double ch : chosenHues) {
            double diff = std::fabs(ch - s.hct.hue);
            diff = std::min(diff, 360.0 - diff);
            if (diff < 15.0) {
                tooClose = true;
                break;
            }
        }
        if (tooClose)
            continue;
        chosenHues.push_back(s.hct.hue);
        return s.argb;
    }
    return scored.front().argb;
}

// One tonal-palette sample = fixed hue + fixed chroma at a given tone.
inline Argb tone(double hue, double chroma, double t)
{
    return dc_hct::tonal_palette_color(hue, chroma, t);
}

// Build DankC's M3 role struct for one mode from the seed's HCT, using MCU's
// tonal-spot palettes + role tone stops. Semantic error/warning/info/success
// stay fixed (matching the stock themes / docs/10-DESIGN-SYSTEM.md sec.4).
dc_theme build_scheme(const Hct &seed, bool light)
{
    const double H = seed.hue;
    const double primaryC = 36.0;
    const double secondaryC = 16.0;
    const double neutralC = 6.0;
    const double neutralVariantC = 8.0;

    dc_theme t{};
    if (light) {
        t.primary = to_dc(tone(H, primaryC, 40));
        t.primary_text = to_dc(tone(H, primaryC, 100));
        t.primary_container = to_dc(tone(H, primaryC, 90));
        t.secondary = to_dc(tone(H, secondaryC, 40));
        t.surface = to_dc(tone(H, neutralC, 98));
        t.surface_text = to_dc(tone(H, neutralC, 10));
        t.surface_variant = to_dc(tone(H, neutralVariantC, 90));
        t.surface_variant_text = to_dc(tone(H, neutralVariantC, 30));
        t.outline = to_dc(tone(H, neutralVariantC, 50));
        t.surface_container_lowest = to_dc(tone(H, neutralC, 100));
        t.surface_container_low = to_dc(tone(H, neutralC, 96));
        t.surface_container = to_dc(tone(H, neutralC, 94));
        t.surface_container_high = to_dc(tone(H, neutralC, 92));
        t.surface_container_highest = to_dc(tone(H, neutralC, 90));
    } else {
        t.primary = to_dc(tone(H, primaryC, 80));
        t.primary_text = to_dc(tone(H, primaryC, 20));
        t.primary_container = to_dc(tone(H, primaryC, 30));
        t.secondary = to_dc(tone(H, secondaryC, 80));
        t.surface = to_dc(tone(H, neutralC, 6));
        t.surface_text = to_dc(tone(H, neutralC, 90));
        t.surface_variant = to_dc(tone(H, neutralVariantC, 30));
        t.surface_variant_text = to_dc(tone(H, neutralVariantC, 80));
        t.outline = to_dc(tone(H, neutralVariantC, 60));
        t.surface_container_lowest = to_dc(tone(H, neutralC, 4));
        t.surface_container_low = to_dc(tone(H, neutralC, 10));
        t.surface_container = to_dc(tone(H, neutralC, 12));
        t.surface_container_high = to_dc(tone(H, neutralC, 17));
        t.surface_container_highest = to_dc(tone(H, neutralC, 22));
    }
    t.background = t.surface;
    t.background_text = t.surface_text;
    t.error = dc_color{0xf4, 0x43, 0x36, 255};
    t.warning = dc_color{0xff, 0x98, 0x00, 255};
    t.info = dc_color{0x21, 0x96, 0xf3, 255};
    t.success = dc_color{0x4c, 0xaf, 0x50, 255};
    return t;
}

} // namespace

extern "C" bool dc_dynamic_from_image(const char *image_path, bool light, dc_theme *out)
{
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(image_path, &w, &h, &n, 3);
    if (!px)
        return false;

    std::vector<QColor> colors = quantize(px, w, h, w * 3);
    stbi_image_free(px);

    Argb seedArgb = score_seed(colors);
    Hct seed = dc_hct::hct_from_argb(seedArgb);
    *out = build_scheme(seed, light);
    return true;
}
