# DankC — Visual Design System (exact tokens, extracted from DMS)

Concrete values pulled from `Common/Appearance.qml`, `Common/Theme.qml`, `Common/Anims.qml`,
`Common/AnimVariants.qml`, the `Widgets/Dank*.qml`, `Shaders/frag/*.frag`, and `Common/StockThemes.js`.
These are the numbers DankC must reproduce so it *looks* like Dank, not just functions like it.

## 1. Tokens

### Spacing (px)
`XS=4, S=8, M=12, L=16, XL=24` (Theme flat scale). Appearance names: small/normal/large/extraLarge/huge = 4/8/12/16/24.

### Corner radius (px)
- Base `cornerRadius` = **12** (user-configurable, animatable).
- Derived: `notificationButtonRadius = cornerRadius/2`; connected-frame uses `frameRounding`.
- Appearance rounding: small/normal/large/extraLarge/full = 8/12/16/24/1000. `StyledRect` default 12.
- Pills/circles: inline `radius = width/2`.

### Font
- Families: UI `Inter Variable`, mono `Fira Code`; default weight Normal, labels/buttons often Medium.
  *(DankC bundles Inter + JetBrains Mono per `07`; keep the same ramp.)*
- Sizes (×`fontScale`, default 1.0): small=12, medium=14, large=16, xlarge=20.
- Bar text scales by `barThickness/48`: ≤0.75→`small*0.9`, ≥1.25→medium, else small (×1.5 if maximize).

### Icons (px)
`iconSize=24, iconSizeSmall=16, iconSizeLarge=32`. Bar icon = `round((barThickness/48)*(size-6)*scale)`.

### Bar
`barHeight=48` (reference divisor for all bar scaling). `panelTransparency=0.85`, `popupTransparency=1.0` (config).

### Elevation / shadow (5 levels)
Each `{blurPx, offsetMag, spread, alpha}` (direction=top ⇒ offsetY=+mag, offsetX=0; diagonal ×0.55):

| Level | blur | offset | spread | alpha |
|---|---|---|---|---|
| 1 | 4 | 1 | 0 | 0.20 |
| 2 | 8 | 4 | 0 | 0.25 |
| 3 | 12 | 6 | 0 | 0.30 |
| 4 | 16 | 8 | 0 | 0.30 |
| 5 | 20 | 10 | 0 | 0.30 |

- **Ambient second shadow:** blur×1.75, spread 1, alpha×0.5. Combine: `1−(1−covKey·a)(1−covAmb·a)`, drawn only outside the shape.
- **Elevation tint overlay** per level: 0.05/0.08/0.11/0.12/0.14.
- Default shadow color pure black; modes can tint (surfaceText/primary/custom). Rendered by the
  `elevation_rect` SDF shader (§3), pad = `ceil(max(blur+spread+maxOffset, ambientBlur+1)+2)`.

### State-layer opacities (M3)
hover **0.08**, focus **0.12**, pressed **0.12**, drag **0.16**. Disabled content 0.38 / containers 0.12
(buttons use `opacity = enabled?1:0.4`). Key fixed-alpha roles: `onSurface_12=0.12`, `onSurface_38=0.38`,
`primaryHover=0.12`, `primaryPressed=0.16`, `primarySelected=0.30`, `surfaceHover=0.08`,
`surfacePressed=0.12`, `surfaceTextSecondary=0.60`, `outlineMedium=0.12` (`layerOutlineOpacity` default 0.12),
`outlineHeavy=0.20`, `outlineButton=0.5`.

### Animation durations & curves
- **Speed presets** by `animationSpeed` (None/1/2/3/4) → `{shorter,short,medium,long,extraLong}`:
  0/0/0/0/0 · 50/75/150/250/500 · 100/150/300/500/1000 · **150/225/450/750/1500 (default "Short"=idx3)** · 200/300/600/1000/2000.
- `popoutAnimationDuration`/`modalAnimationDuration`: preset `[0,150,300,500]`, **default 150**.
- `currentAnimationBaseDuration`: `[0,250,500,750]`, default 500. `expressiveDurations` = base × {fast .4,
  normal .8, large 1.2, extraLarge 2.0, spatialFast .7, spatialDefault 1.0, effects .4}.
- Easing enums: standard `OutCubic`, emphasized `OutQuart`.
- **Bézier curves** ([x1,y1,x2,y2], multi-seg for emphasized):
  - standard `[.2,0,0,1]`, standardAccel `[.3,0,1,1]`, standardDecel `[0,0,0,1]`
  - emphasized `[.05,0, .1333,.06, .1667,.4, .2083,.82, .25,1]` (multi-segment → **256-sample LUT**)
  - emphasizedAccel `[.3,0,.8,.15]`, emphasizedDecel `[.05,.7,.1,1]`
  - expressiveFastSpatial `[.42,1.67,.21,.9]` (overshoot), expressiveDefaultSpatial `[.38,1.21,.22,1]`
    (overshoot), expressiveEffects `[.34,.8,.34,1]`
- Variant tuning (`AnimVariants`): enter factors `[1.0,0.9,1.08]`, exit `[1.0,0.85,0.92]`, collapsed scales
  `[0.96,1.0,0.88]`, anim offsets `[16,144,56]` (Standard/Directional/Depth). Fluent opacity at 55% of pos duration.

## 2. Widget anatomy (dimensions + per-state colors)

| Widget | Size | Radius | Normal | Hover | Pressed | Notes |
|---|---|---|---|---|---|---|
| **DankButton** | h40, min-w64, padX16 | 12 | bg `buttonBg`, text `buttonText` | state-layer textColor@0.12 | @0.20 + scale 0.98 | ripple(textColor); icon16, text14 Medium |
| **DankActionButton** | 32² | 12 | icon `surfaceText`, bg transparent | primary@0.08 | @0.12 | icon20; StateLayer(primary); tooltip |
| **DankToggle** (switch) | track 52×30, thumb 24(on)/20(off) | full | track: on `primary`, off `surfaceVariant@0.2` | — | — | thumb on=`surface`, off=`outline`; x/color 300ms emphasizedDecel; check icon20 |
| **DankSlider** | h48, track h12, handle 4×20 | 12 | track `outline@op`, fill `primary` | handle+onPrimary@0.08 | @0.16, handle scale1.05 | tap ripple→28px 180ms; value tooltip |
| **DankTextField** | w200, h~42 | 12 | bg `surfaceContainerHigh@popupα`, border `outlineMedium` w1 | — | focus border `primary` w2 | placeholder `outlineButton`; selection primaryContainer |
| **DankDropdown** | trigger h40 | 12 | bg `surfaceContainer@α`, border `outlineHeavy` w1 | bg `surfaceContainerHigh` | open border `primary` w2 | popup `surfaceContainer`+shadowL2; rows h32, sel `primaryHover`, hover `primaryHoverLight` |
| **DankTooltip** | w clamp120–300, h auto | 12 | bg `surfaceContainerHigh@popupα`, border `outlineMedium` w1 | — | — | text `small`, WindowBlur radius=cornerRadius |
| **StateLayer** | fills parent | parent | 0 | 0.08 | 0.12 | color default `surfaceText`; DankColorAnim `shorterDuration` standardDecel; triggers ripple |
| **DankRipple** | GLSL | — | opacity 0.10 | — | grows 0→maxCornerDist | holds to 60% then fades; duration `spatialDefault` |
| **StyledRect** | — | 12 | transparent | — | — | Behavior radius/opacity 300ms standard |

`DankListView`: momentum scroll (`ScrollConstants.js`), `StopAtBounds`, `DankScrollbar`, shared
`ListViewTransitions` for add/remove/move. Bar text/icon helpers scale off `barHeight=48`.

## 3. Shaders (13 GLSL frag; SDF rounded-box + `smoothstep(-fw,fw,d)` AA, `fw=fwidth(d)`)

**UI/effect (port these for the Dank look):**
- **`ripple`** — growing circle masked by parent rounded-rect; out = rippleCol × col.a × opacity × mask.
- **`elevation_rect`** — *the shadow/border engine*: rounded rect (per-corner radius vec4) + border ring +
  key shadow (SDF offset+blur) + ambient shadow, drawn outside the shape. Uniforms: rectPx, cornerRadius(4),
  fill/border/shadow colors, shadowParam(blur,spread,offX,offY), ambientParam(blur,spread,alpha). Used by
  `ElevationShadow`.
- **`frame_arc`** — screen-edge frame ring with rounded cutout (`max(sdBox(outer), −sdRoundBox(cutout))`).
- **`connected_arc`** — frame ring + up to 4 chrome bodies unioned via smooth-min (`smin`, per-corner fillet
  k) + the elevation shadow model. For the connected-frame aesthetic.
- **`connected_chrome`** — popout body fused to its bar edge: `smin(distToBarEdge, roundedBody, k)` + shadow.
  Uniforms: bodyRect, cornerRadius(4), edgeParam(barSide 0/1/2/3, fillet k).

**Wallpaper transitions** (shared `sampleWithFillMode()` fillMode 0–7; uniforms progress, fillMode,
image/screen sizes, fillColor; samplers source1/source2):
- `wp_fade` mix · `wp_wipe` (direction, smoothness moving edge) · `wp_disc` (aspect-corrected expanding
  circle) · `wp_iris_bloom` (disc + vertical slit-squash eye-open) · `wp_parallax_scroll` (live pan/zoom,
  no progress) · `wp_pixelate` (cell size shrinks to 1px) · `wp_portal` (old collapses to center) ·
  `wp_stripes` (alternating stripes wipe opposite, staggered wave + vignette).

All compile to `Shaders/qsb/*.frag.qsb` (Vulkan/QSB). For DankC: rewrite as **GLSL ES** fragment shaders
for our nanovg/GL pipeline (same math).

## 4. Material 3 color-role → surface mapping

| Surface | Role |
|---|---|
| Bar background | `widgetBaseBackgroundColor` (default `surfaceContainerHigh`; `sch`) |
| Card / widget | mode: s=`surface`, sc=`surfaceContainer`, sch=`surfaceContainerHigh` (default), *Container roles, or custom=`blend(surfaceContainerHigh, custom, ~0.4)`; hover `blend(base,primary,0.10)` |
| Popout / modal | `withAlpha(surfaceContainer, popupTransparency)`; higher `surfaceContainerHigh` |
| Primary text | `surfaceText`(=onSurface); secondary `surfaceVariantText`; on-accent `onPrimary` |
| Borders | `outlineMedium`(0.12) inputs/layers; `outlineHeavy`(0.2) dropdown; `primary` focused; `outlineButton`(0.5) placeholder |
| Accents | `primary/secondary/tertiary` + their containers |
| Semantic (fixed) | error `#F2B8B5`, warning `#FF9800`, info `#2196F3`, success `#4CAF50`, tempWarning `#ff9933`, tempDanger `#ff5555` |

**Container derivation** (when theme omits them): `outlineVariant=withAlpha(outline,0.6)`,
`surfaceContainerLowest=blend(surfaceContainer,surface,1.2)`, `surfaceContainerLow=blend(surface,surfaceContainer,0.667)`,
`primaryContainer=blend(surfaceContainerHigh,primary,0.45)`, `secondary/tertiaryContainer=blend(...,0.35)`.

**Helpers to port:** `withAlpha(c,a)` (replace alpha), `blendAlpha(c,a)` (multiply alpha),
`blend(c1,c2,r)` (per-channel lerp, r may exceed 1 → extrapolate), `isColorDark = 0.299r+0.587g+0.114b<0.5`.
**Transparent-blur variants:** when blur enabled + `!blurForegroundLayers`, tile/track alphas drop
(tiles 0.16/0.08, slider track 0.18).

**Fallback palette (stock "purple" dark)** — the guaranteed baseline before matugen:
`primary #D0BCFF, primaryText #381E72, primaryContainer #4F378B, secondary #CCC2DC, surface #141218,
surfaceText #e6e0e9, surfaceVariant #49454e, surfaceVariantText #cac4cf, background #141218, outline #948f99,
surfaceContainerLowest #100e14, surfaceContainerLow #1d1b20, surfaceContainer #211f24,
surfaceContainerHigh #2b292f, surfaceContainerHighest #36343a`.

## 5. What this means for DankC's renderer
- Implement **one SDF "panel" shader** = `elevation_rect` (rounded rect + per-corner radius + border + key
  + ambient shadow). Almost every DMS surface (bar, card, popout, dropdown, dock, tooltip) is that shader
  with different params — build it once, reuse everywhere. nanovg's `nvgBoxGradient` covers simpler shadows;
  the SDF shader gives exact parity.
- Port the **ripple** and **connected_chrome** shaders for interaction + connected mode.
- Bake the **elevation table, spacing/radius/font ramps, state opacities, and the bezier/duration tables**
  into `ui/theme.c` + `render/anim.c` as constants (all listed above).
- Wallpaper transitions (§3) are M6; the 8 transition shaders port directly to GLSL ES.
