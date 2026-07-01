/* apps.h — desktop-entry index for the app launcher.
 *
 * Scans XDG applications directories once, parses the [Desktop Entry] Name/
 * Exec/Icon (skipping NoDisplay/Hidden and non-Application types), and offers a
 * ranked fuzzy search. See docs/04-FEATURES (launcher).
 */
#ifndef DC_SERVICES_APPS_H
#define DC_SERVICES_APPS_H

#define DC_APP_NAME 128
#define DC_APP_EXEC 320
#define DC_APP_ID 128

typedef struct {
    char name[DC_APP_NAME];
    char exec[DC_APP_EXEC]; /* field codes (%f/%u/...) stripped */
    char id[DC_APP_ID];     /* desktop-file basename, for icon resolution */
    int score;              /* scratch, set during search */
} dc_app;

typedef struct dc_apps dc_apps;

/* Scan all applications directories. Never NULL (may hold zero apps). */
dc_apps *dc_apps_load(void);
void dc_apps_destroy(dc_apps *apps);

/* Total indexed apps. */
int dc_apps_count(const dc_apps *apps);

/* Rank apps against `query` (case-insensitive subsequence). Writes up to `max`
 * best matches into `out` (pointers into the index) and returns the count. An
 * empty query returns the first `max` apps alphabetically. */
int dc_apps_search(dc_apps *apps, const char *query, const dc_app **out, int max);

/* Launch an app's Exec line, detached from DankC. */
void dc_app_launch(const dc_app *app);

#endif /* DC_SERVICES_APPS_H */
