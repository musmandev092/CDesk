/* hct.h — HCT color space (Hue, Chroma, Tone) per Google's Material Color
 * Utilities (MCU), header-only C++17, pure math (no image I/O).
 *
 * HCT = CAM16's hue + chroma correlates, paired with L* (CIE Lab lightness)
 * for "tone" instead of CAM16's own J, exactly as MCU defines it. This file
 * implements:
 *   - sRGB <-> XYZ (D65) <-> CIECAM16 forward transform (hue, chroma, J)
 *   - hct_from_argb(): the forward direction (real CAM16, not an HSL stand-in)
 *   - hct_solve(): the inverse direction (hue, chroma, tone) -> sRGB
 *   - tonal_palette_color(): hue + fixed role-chroma -> sRGB at a given tone
 *     (the exact quantity matugen calls a "TonalPalette")
 *
 * On solveToInt vs. MCU's own HctSolver: MCU's reference implementation finds
 * the inverse via an exact geometric line/plane-intersection search over a
 * precomputed 255-entry gamut table ("critical planes"), for speed. This file
 * gets the same fixed point (same CIECAM16 equations, same CIE L*) via plain
 * numerical bisection instead:
 *   - inner: for a fixed (hue, chroma), bisect CAM16 J until the resulting
 *     XYZ's Y matches the Y implied by the target L* tone (Y is monotonic in
 *     J for fixed hue/chroma) — this step is otherwise closed-form (a 3x3
 *     linear solve for the post-adaptation cone responses from
 *     (a, b, achromatic-response), then a straight algebraic inversion of the
 *     adaptation nonlinearity).
 *   - outer: if the fully-saturated request doesn't fit in the sRGB cube
 *     (common near tone 0/100), bisect chroma down to the largest value that
 *     still fits, re-running the inner solve each step.
 * Slower than MCU's table lookup (a few dozen iterations vs. one), but that's
 * fine here — this runs a few dozen times per theme generation (on wallpaper
 * change / stock-theme regen), never per frame. Verified against `matugen`
 * (docs/14-COMPLETION-PLAN.md W4.1) — see dynamic.cpp's header comment.
 */
#ifndef DC_THEME_HCT_H
#define DC_THEME_HCT_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dc_hct {

struct Argb {
    uint8_t r, g, b;
};

/* ---- sRGB <-> linear <-> XYZ (D65), all on the "Y=100 for white" scale CAM16
 * expects. Standard, widely published coefficients (Bruce Lindbloom / CIE). */

inline double srgb_to_linear(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.040449936 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

inline double linear_to_srgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

struct Xyz {
    double x, y, z; /* 0..100 scale (white Y = 100) */
};

inline Xyz linear_rgb_to_xyz(const double lin[3])
{
    double r = lin[0], g = lin[1], b = lin[2];
    return Xyz{41.24564 * r + 35.75761 * g + 18.04375 * b,
               21.26729 * r + 71.51522 * g + 7.21750 * b,
               1.93339 * r + 11.91920 * g + 95.03041 * b};
}

inline void xyz_to_linear_rgb(const Xyz &xyz, double out[3])
{
    double X = xyz.x / 100.0, Y = xyz.y / 100.0, Z = xyz.z / 100.0;
    out[0] = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    out[1] = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    out[2] = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
}

inline Xyz argb_to_xyz(Argb c)
{
    double lin[3] = {srgb_to_linear(c.r / 255.0), srgb_to_linear(c.g / 255.0),
                     srgb_to_linear(c.b / 255.0)};
    return linear_rgb_to_xyz(lin);
}

/* Clamp+round; used for the final answer once we're happy with the linear
 * RGB (either genuinely in-gamut, or clipped as a last resort). */
inline Argb linear_rgb_to_argb_clamped(const double lin[3])
{
    auto u8 = [](double v) {
        return (uint8_t)std::lround(std::clamp(linear_to_srgb(std::clamp(v, 0.0, 1.0)), 0.0, 1.0) *
                                     255.0);
    };
    return Argb{u8(lin[0]), u8(lin[1]), u8(lin[2])};
}

inline bool linear_rgb_in_gamut(const double lin[3])
{
    const double eps = 1e-4;
    return lin[0] > -eps && lin[0] < 1.0 + eps && lin[1] > -eps && lin[1] < 1.0 + eps &&
           lin[2] > -eps && lin[2] < 1.0 + eps;
}

/* ---- CIE L* (lightness) <-> Y (relative luminance, 0..100). Standard. */

inline double y_from_lstar(double lstar)
{
    return lstar <= 8.0 ? lstar * 100.0 / (24389.0 / 27.0)
                        : 100.0 * std::pow((lstar + 16.0) / 116.0, 3.0);
}

inline double lstar_from_y(double y)
{
    double yn = y / 100.0;
    return yn <= 216.0 / 24389.0 ? yn * (24389.0 / 27.0) : 116.0 * std::cbrt(yn) - 16.0;
}

/* ---- generic 3x3 linear solve (Cramer's rule) -- used both to invert the
 * fixed CAT16 matrix once at startup and to solve for post-adaptation cone
 * responses (rgbA) from (a, b, achromatic-response) during the HCT solver. */

inline double det3(const double m[3][3])
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

inline void solve3x3(const double m[3][3], const double v[3], double out[3])
{
    double d = det3(m);
    for (int col = 0; col < 3; col++) {
        double mm[3][3];
        for (int r = 0; r < 3; r++)
            for (int c2 = 0; c2 < 3; c2++)
                mm[r][c2] = (c2 == col) ? v[r] : m[r][c2];
        out[col] = det3(mm) / d;
    }
}

/* ---- CIECAM16 ("CAT16") matrix -- standard, from Li et al. 2016, Table 1. */
static const double CAM16_M[3][3] = {{0.401288, 0.650173, -0.051461},
                                     {-0.250268, 1.204414, 0.045854},
                                     {-0.002079, 0.048952, 0.953127}};

inline void xyz_to_cam16rgb(const Xyz &xyz, double out[3])
{
    double v[3] = {xyz.x, xyz.y, xyz.z};
    for (int i = 0; i < 3; i++)
        out[i] = CAM16_M[i][0] * v[0] + CAM16_M[i][1] * v[1] + CAM16_M[i][2] * v[2];
}

inline Xyz cam16rgb_to_xyz(const double rgb[3])
{
    double out[3];
    solve3x3(CAM16_M, rgb, out);
    return Xyz{out[0], out[1], out[2]};
}

/* Viewing conditions: D65 white, average surround, L*=50 background, no
 * discounting -- MCU's (and CIECAM16's reference implementations') default.
 * Computed once and cached. */
struct ViewingConditions {
    double aw, nbb, ncb, c, nc, n, z, fl;
    double rgbD[3];
};

inline const ViewingConditions &vc_default()
{
    static ViewingConditions vc = [] {
        ViewingConditions v{};
        const Xyz white{95.047, 100.0, 108.883};
        double la = (200.0 / M_PI) * y_from_lstar(50.0) / 100.0;
        double yb = y_from_lstar(50.0);
        v.n = yb / 100.0;
        v.z = 1.48 + std::sqrt(v.n);
        v.nbb = 0.725 / std::pow(v.n, 0.2);
        v.ncb = v.nbb;
        v.c = 0.69; /* average surround */
        v.nc = 1.0;
        double f = 1.0;
        double k = 1.0 / (5.0 * la + 1.0);
        double k4 = k * k * k * k;
        v.fl = k4 * la + 0.1 * (1.0 - k4) * (1.0 - k4) * std::cbrt(5.0 * la);
        double d = f * (1.0 - (1.0 / 3.6) * std::exp((-la - 42.0) / 92.0));
        d = std::clamp(d, 0.0, 1.0);

        double rgbW[3];
        xyz_to_cam16rgb(white, rgbW);
        for (int i = 0; i < 3; i++)
            v.rgbD[i] = d * (100.0 / rgbW[i]) + 1.0 - d;

        double rgbAW[3];
        for (int i = 0; i < 3; i++) {
            double applied = v.rgbD[i] * rgbW[i];
            double af = std::pow(v.fl * std::fabs(applied) / 100.0, 0.42);
            rgbAW[i] = 400.0 * af / (af + 27.13); /* white is always positive */
        }
        double p2w = 2.0 * rgbAW[0] + rgbAW[1] + 0.05 * rgbAW[2];
        v.aw = (p2w - 0.305) * v.nbb;
        return v;
    }();
    return vc;
}

struct Hct {
    double hue;    /* degrees, 0..360 */
    double chroma; /* CAM16 chroma correlate, unbounded but typically 0..~120 */
    double tone;   /* CIE L*, 0..100 */
};

/* Forward: real ARGB -> HCT (CAM16 hue/chroma + L* tone). */
inline Hct hct_from_argb(Argb c)
{
    const ViewingConditions &vc = vc_default();
    Xyz xyz = argb_to_xyz(c);

    double rgb[3];
    xyz_to_cam16rgb(xyz, rgb);
    double rgbA[3];
    for (int i = 0; i < 3; i++) {
        double applied = vc.rgbD[i] * rgb[i];
        double af = std::pow(vc.fl * std::fabs(applied) / 100.0, 0.42);
        rgbA[i] = (applied < 0 ? -1.0 : 1.0) * 400.0 * af / (af + 27.13);
    }
    double a = (11.0 * rgbA[0] - 12.0 * rgbA[1] + rgbA[2]) / 11.0;
    double b = (rgbA[0] + rgbA[1] - 2.0 * rgbA[2]) / 9.0;
    double u = (20.0 * rgbA[0] + 20.0 * rgbA[1] + 21.0 * rgbA[2]) / 20.0;
    double p2 = 2.0 * rgbA[0] + rgbA[1] + 0.05 * rgbA[2];

    double hueRad = std::atan2(b, a);
    if (hueRad < 0)
        hueRad += 2.0 * M_PI;
    double hueDeg = hueRad * 180.0 / M_PI;
    if (hueDeg >= 360.0)
        hueDeg -= 360.0;

    double ac = (p2 - 0.305) * vc.nbb;
    double J = 100.0 * std::pow(std::max(ac / vc.aw, 0.0), vc.c * vc.z);

    double eHue = 0.25 * (std::cos(hueRad + 2.0) + 3.8);
    double p1 = (50000.0 / 13.0) * eHue * vc.nc * vc.ncb;
    double t = p1 * std::sqrt(a * a + b * b) / (u + 0.305);
    double alpha = std::pow(std::max(t, 0.0), 0.9) * std::pow(1.64 - std::pow(0.29, vc.n), 0.73);
    double C = alpha * std::sqrt(std::max(J, 0.0) / 100.0);

    double tone = lstar_from_y(xyz.y);
    return Hct{hueDeg, C, tone};
}

/* Inner solve: given a CAM16 lightness J, a hue, and a *target CAM16 chroma*,
 * produce linear sRGB (may be out of [0,1] gamut) and the Y it implies.
 *
 * The CAM16 (a, b) opponent coordinates are NOT linear in the chroma
 * correlate C: C depends on the eccentricity factor `t`, which itself depends
 * on the post-adaptation cone responses (rgbA), which depend on (a, b). So we
 * cannot just set a = C*cos(h). Instead we bisect the raw (a,b) magnitude `r`
 * until the resulting CAM16 chroma matches the request (C is monotonic in r
 * for fixed J/hue), then invert the adaptation nonlinearity to get XYZ. */
inline double y_for_j_hue_chroma(double J, double hueDeg, double chroma, double lin[3])
{
    const ViewingConditions &vc = vc_default();
    double hueRad = hueDeg * M_PI / 180.0;
    double cosH = std::cos(hueRad), sinH = std::sin(hueRad);

    double ac = vc.aw * std::pow(std::max(J, 0.0) / 100.0, 1.0 / (vc.c * vc.z));
    double p2Target = ac / vc.nbb + 0.305;

    double eHue = 0.25 * (std::cos(hueRad + 2.0) + 3.8);
    double p1 = (50000.0 / 13.0) * eHue * vc.nc * vc.ncb;

    /* Solve: 11*r0-12*r1+r2=11a ; r0+r1-2*r2=9b ; 40*r0+20*r1+r2=20*p2Target */
    static const double M[3][3] = {{11, -12, 1}, {1, 1, -2}, {40, 20, 1}};
    double rgbA[3] = {0, 0, 0};

    auto chroma_for_r = [&](double r) -> double {
        double a = r * cosH, b = r * sinH;
        double v[3] = {11.0 * a, 9.0 * b, 20.0 * p2Target};
        solve3x3(M, v, rgbA);
        double u = rgbA[0] + rgbA[1] + 1.05 * rgbA[2];
        double tt = p1 * r / (u + 0.305);
        double alpha =
            std::pow(std::max(tt, 0.0), 0.9) * std::pow(1.64 - std::pow(0.29, vc.n), 0.73);
        return alpha * std::sqrt(std::max(J, 0.0) / 100.0);
    };

    if (chroma < 1e-9) {
        chroma_for_r(0.0);
    } else {
        /* Bracket r, then bisect. C is monotone increasing in r over the valid
         * range; grow the upper bound until it exceeds the request (or C stops
         * increasing, meaning we've hit the achromatic-pole singularity). */
        double lo = 0.0, hi = 1e-4, prev = 0.0;
        for (int guard = 0; guard < 64; guard++) {
            double c = chroma_for_r(hi);
            if (c >= chroma || c < prev - 1e-9 || hi > 1e6)
                break;
            prev = c;
            lo = hi;
            hi *= 2.0;
        }
        for (int i = 0; i < 60; i++) {
            double mid = 0.5 * (lo + hi);
            if (chroma_for_r(mid) < chroma)
                lo = mid;
            else
                hi = mid;
        }
        chroma_for_r(0.5 * (lo + hi));
    }

    double rgbDApplied[3];
    for (int i = 0; i < 3; i++) {
        double A = rgbA[i];
        double sign = A < 0 ? -1.0 : 1.0;
        double absA = std::fabs(A);
        if (absA >= 400.0)
            absA = 399.999; /* keep the inversion below defined for pathological J */
        double Fv = 27.13 * absA / (400.0 - absA);
        double afInv = Fv <= 0.0 ? 0.0 : std::pow(Fv, 1.0 / 0.42);
        rgbDApplied[i] = sign * 100.0 * afInv / vc.fl;
    }
    double rgb[3];
    for (int i = 0; i < 3; i++)
        rgb[i] = rgbDApplied[i] / vc.rgbD[i];
    Xyz xyz = cam16rgb_to_xyz(rgb);
    xyz_to_linear_rgb(xyz, lin);
    return xyz.y;
}

/* For a fixed (hue, chroma), bisect J so the resulting Y matches the target
 * tone's Y (Y is monotone in J). Fills `lin` with the linear sRGB result. */
inline void solve_j_for_tone(double hueDeg, double chroma, double targetY, double lin[3])
{
    double lo = 0.0, hi = 100.0;
    for (int i = 0; i < 50; i++) {
        double mid = 0.5 * (lo + hi);
        double y = y_for_j_hue_chroma(mid, hueDeg, chroma, lin);
        if (y < targetY)
            lo = mid;
        else
            hi = mid;
    }
    y_for_j_hue_chroma(0.5 * (lo + hi), hueDeg, chroma, lin);
}

/* Outer + inner solve: (hue, chroma, tone) -> sRGB, matching the requested
 * tone exactly and reducing chroma only if the full request can't fit in the
 * sRGB gamut (see file header). */
inline Argb hct_solve(double hueDeg, double chroma, double tone)
{
    tone = std::clamp(tone, 0.0, 100.0);
    /* MCU special-cases the achromatic poles to pure black/white (the D65
     * chromatic adaptation otherwise leaves white a hair off #ffffff). */
    if (tone >= 100.0)
        return Argb{255, 255, 255};
    if (tone <= 0.0)
        return Argb{0, 0, 0};
    double targetY = y_from_lstar(tone);
    chroma = std::max(chroma, 0.0);

    double lin[3];
    solve_j_for_tone(hueDeg, chroma, targetY, lin);
    if (linear_rgb_in_gamut(lin) || chroma < 1e-6)
        return linear_rgb_to_argb_clamped(lin);

    /* Full chroma doesn't fit (common near tone 0/100) -- find the largest
     * chroma that still lands in the sRGB cube while holding the tone. The
     * chroma=0 (achromatic) baseline is the guaranteed-tone fallback. */
    double bestLin[3];
    solve_j_for_tone(hueDeg, 0.0, targetY, bestLin);
    double loC = 0.0, hiC = chroma;
    for (int iter = 0; iter < 25; iter++) {
        double midC = 0.5 * (loC + hiC);
        double candLin[3];
        solve_j_for_tone(hueDeg, midC, targetY, candLin);
        if (linear_rgb_in_gamut(candLin)) {
            loC = midC;
            std::copy(candLin, candLin + 3, bestLin);
        } else {
            hiC = midC;
        }
    }
    return linear_rgb_to_argb_clamped(bestLin);
}

inline Argb tonal_palette_color(double hue, double chroma, double tone)
{
    return hct_solve(hue, chroma, tone);
}

} // namespace dc_hct

#endif /* DC_THEME_HCT_H */
