/* icons.h — resolve an application to an icon file via the XDG icon theme.
 *
 * Maps a niri app_id (or a freedesktop icon name) to an on-disk icon file:
 * looks up the app's .desktop Icon= key, then searches the icon-theme
 * directories, hicolor, and /usr/share/pixmaps. See docs/07-GAPS §G1.
 */
#ifndef DC_SERVICES_ICONS_H
#define DC_SERVICES_ICONS_H

/* Resolve `app_id` to an icon file path. Returns a malloc'd path (caller frees)
 * or NULL if none found. PNG is preferred (decodable by nanovg/stb_image); SVG
 * paths are only returned when `svg_ok` is non-zero. `size_hint` is the desired
 * pixel size. */
char *dc_icon_resolve(const char *app_id, int size_hint, int svg_ok);

#endif /* DC_SERVICES_ICONS_H */
