# DankC — Theming & Material Color Engine

Replaces DMS's matugen(Rust) pipeline with an **in-process C++ island** (Material Color Utilities) +
C code for dank16 and application/output color output. Same visual result, no external `matugen` process.

## 1. Pipeline (wallpaper → colors → UI)

```
wallpaper.png ──stb_image──► pixels ──resize 128×128──► MCU QuantizeCelebi(128)
   ──► RankedSuggestions[0] = seed ARGB ──► Hct(seed)
   ──► Scheme<variant>(hct, isDark, contrast) ──► MaterialDynamicColors::Role().GetArgb(scheme)
   ──► {dark,light} role maps ──► dank16 (C) ──► theme.c live roles ──► every widget
                                            └─► apply.c → ~/.config/niri/dms/colors.kdl (+ app templates, opt)
```

Run on: startup, wallpaper change, theme change, light/dark toggle, `matugenScheme`/`matugenContrast`
change. Debounce; serialize (one generation at a time). Both `dark` and `light` are always generated so
mode-switch is instant.

## 2. The C++ island (`src/theme/material.cpp`)

Library: **`material-foundation/material-color-utilities`**, `cpp/`, namespace
`material_color_utilities`, **C++17**. Wrapped behind a C ABI in `material.h`:

```c
// material.h  (pure C)
typedef struct { uint32_t roles[DANKC_ROLE_COUNT]; } dankc_scheme; // ARGB per role
int  dankc_scheme_from_image(const char *path, int variant, int is_dark, float contrast, dankc_scheme *out);
int  dankc_scheme_from_color(uint32_t seed_argb, int variant, int is_dark, float contrast, dankc_scheme *out);
```

Inside `material.cpp`:
1. `Argb` = `uint32_t` 0xAARRGGBB (`cpp/utils/utils.h`).
2. `QuantizerResult QuantizeCelebi(const std::vector<Argb>& pixels, uint16_t /*128*/)` (`cpp/quantize/celebi.h`).
3. `std::vector<Argb> RankedSuggestions(color_to_count, ScoreOptions{})` (`cpp/score/score.h`), take `[0]`.
4. `Hct src(seed)` (`cpp/cam/hct.h`) — construct, there is **no** `Hct::FromInt`.
5. `SchemeTonalSpot scheme(src, is_dark, contrast_level)` etc. (`cpp/scheme/scheme_*.h`); all 9 share
   the ctor shape. `contrast_level` ∈ **[-1.0,1.0]**, 0=standard (maps to DMS `matugenContrast`).
6. Roles via `MaterialDynamicColors::Primary().GetArgb(scheme)` etc.
   (`cpp/dynamiccolor/material_dynamic_colors.h`, 63 roles available).

**Variant enum** (`variant.h`) ↔ DMS `matugenScheme` (1:1, matugen uses the same algorithm):
`kTonalSpot`←`scheme-tonal-spot` **(default)**, `kContent`,`kExpressive`,`kVibrant`,`kFruitSalad`,
`kRainbow`,`kNeutral`,`kMonochrome`,`kFidelity`.

**Build without Bazel:** compile only the `.cc` used —
`utils/utils.cc cam/{hct,cam,viewing_conditions}.cc quantize/{celebi,wsmeans,wu,lab}.cc score/score.cc
palettes/tones.cc dynamiccolor/{dynamic_scheme,dynamic_color,material_dynamic_colors}.cc scheme/scheme_*.cc
contrast/contrast.cc` (+ `blend/ dislike/ temperature/` only if using fidelity/content) with repo root on
`-I`. **Abseil:** the only use is `absl::StrCat` in `utils.cc HexFromArgb` — patch that one line to
`snprintf` to drop Abseil entirely.

**Image decode:** `stb_image.h` (`stbi_load(path,&x,&y,&n,4)` → RGBA8). MCU wants packed `0xAARRGGBB`, so
build each `uint32_t` explicitly from R,G,B,A bytes (no memcpy/reinterpret). Skip `a==0`. Downscale to
~128×128 first (`stb_image_resize2.h` `stbir_resize_uint8_srgb`) to match matugen and cut cost.

## 3. Color roles DankC must expose

MCU roles are camelCase; DMS's `Theme.qml` consumes **snake_case** keys under `colors.{dark,light}`.
`apply.c`/`theme.c` re-key to this exact contract (both modes):

`primary, on_primary, primary_container, secondary, secondary_container, tertiary, tertiary_container,
surface, on_background, background, surface_variant, on_surface_variant, surface_tint, outline,
surface_container_lowest, surface_container_low, surface_container, surface_container_high,
surface_container_highest` (+ error/on_error/inverse_* and the fixed accents as needed).

`theme.c` also holds **design tokens** (ported from DMS `Appearance.qml`): corner radius (`cornerRadius`,
default 12/16), spacing scale (S/M/L/XL), font sizes, icon size, and derived alpha variants
(`primaryHover = α0.12`, `surfacePressed`, `onSurface_12/_38`, …). Widgets read `theme.*`, never hardcode.

## 4. dank16 (you must generate this yourself)

**matugen never produced dank16** — DMS generates it in Go and injects it. DankC reproduces it in
`src/theme/dank16.c`: a 16-color ANSI terminal palette derived from the scheme's seed/surface, each color
with `dark`/`light`/`default` variants. Needed for: terminal theming templates and any widget using ANSI
colors. Port DMS's `generateDank16Variants` logic.

## 5. Applying colors outward

`apply.c` writes generated colors to consumers (like DMS's matugen templates, but we own it):
- **niri:** `~/.config/niri/dms/colors.kdl` (focus-ring/border colors) — included by niri config.
- **Optional app templates** (feature-gated, off unless enabled): GTK, Qt (qt5ct/qt6ct/kde), terminals
  (foot/alacritty/kitty/ghostty/wezterm), etc. Each guarded by a `matugenTemplate<App>` setting like DMS.
  For v1, only DankC's own UI + niri colors are required; app-theming templates are a later nicety.
- **Portal sync:** to make GNOME/GTK apps follow dark/light, set gsettings
  `org.gnome.desktop.interface color-scheme` (see `05-PORTABILITY.md §3`); the GTK portal then serves
  `org.freedesktop.appearance color-scheme`. (DMS does this via `--sync-mode-with-portal`.)

## 6. Custom themes & stock themes

- **Stock themes:** named palettes (DMS default `"purple"`; this machine uses `"green"`). Stored as
  seed colors; still run through the color engine as the seed (`from_color`) so surfaces/containers and
  dank16 are generated consistently.
- **Custom themes (JSON):** keep DMS's `CUSTOM_THEMES.md` format — a JSON file with `dark`/`light`
  objects (or flat) providing the ~16 required roles + optional `error/warning/info` + `matugen_type`.
  Config `currentThemeName:"custom"` + `customThemeFile:"/path.json"`; watch the file (inotify) and
  re-theme live on edit. Parse with cJSON.
- Config keys driving all this: `currentThemeName`, `currentThemeCategory`, `customThemeFile`,
  `matugenScheme`, `matugenContrast`, `runUserMatugenTemplates`, `matugenTemplate<App>` toggles — see
  `04-FEATURES.md §config`. State (mode, per-monitor wallpaper) lives in `session.json`.

## 7. Caching

Generated colors cached under `$XDG_CACHE_HOME/DankC/` (mirrors DMS's `dms-colors.json`), written
atomically (tmp + rename). On startup, load cache first for instant paint, then regenerate if the
wallpaper mtime is newer.
