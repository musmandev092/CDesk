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

/* Load `image_path`, pick a seed colour, and fill `out` with a generated dark
 * palette. Returns false if the image can't be read. */
bool dc_dynamic_from_image(const char *image_path, dc_theme *out);

#ifdef __cplusplus
}
#endif

#endif /* DC_THEME_DYNAMIC_H */
