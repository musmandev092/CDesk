/* dynamic.h — wallpaper-derived Material palette (the one C++ module).
 *
 * Extracts a vibrant seed colour from an image and generates a coherent dark
 * Material-style palette, approximating DMS/matugen's dynamic theming. The
 * implementation lives in dynamic.cpp (Material colour math is the sole
 * sanctioned use of C++ in DankC).
 */
#ifndef DC_THEME_DYNAMIC_H
#define DC_THEME_DYNAMIC_H

#include "theme/theme.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load `image_path`, pick a seed colour via MCU quantize+score, and fill `out`
 * with a generated Material palette in the requested mode (`light` -> light
 * variant, else dark). Returns false if the image can't be read. */
bool dc_dynamic_from_image(const char *image_path, bool light, dc_theme *out);

/* Derive a 16-colour ANSI terminal palette (out[0..7] = normal
 * black/red/green/yellow/blue/magenta/cyan/white, out[8..15] = the bright
 * variants, same order) from `t`'s primary hue, for the system-theming
 * terminal emitters (services/systheme_term.c: alacritty/kitty/foot). Each
 * chromatic slot is anchored to a canonical hue on a 60-degree colour wheel
 * and harmonized slightly toward `t->primary`'s hue (MCU's Blend.harmonize:
 * rotate up to half the hue gap, capped at 15 degrees) so the whole ANSI-16
 * set reads as "this theme's terminal colours" rather than generic stock
 * ANSI. black/white (and their bright variants) are derived from whichever
 * of `t->surface`/`t->surface_text` is darker/lighter, so they come out
 * correct in both dark and light mode without needing `light` for that part;
 * `light` instead mirrors (100 - tone) the six chromatic slots' target tones
 * so they read as saturated-but-legible on a light background instead of a
 * dark one. */
void dc_dynamic_ansi16(const dc_theme *t, bool light, dc_color out[16]);

#ifdef __cplusplus
}
#endif

#endif /* DC_THEME_DYNAMIC_H */
