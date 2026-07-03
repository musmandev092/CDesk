/* apps.h — desktop-entry index for the app launcher.
 *
 * Scans XDG applications directories once, parses the [Desktop Entry] Name/
 * Exec/Icon/Comment/GenericName (skipping NoDisplay/Hidden and non-Application
 * types), and offers a ranked fuzzy search. See docs/04-FEATURES (launcher).
 */
#ifndef DC_SERVICES_APPS_H
#define DC_SERVICES_APPS_H

#define DC_APP_NAME 128
#define DC_APP_EXEC 320
#define DC_APP_ID 128
#define DC_APP_DESC 192

/* Raw ';'-joined MimeType=/Categories= values straight from the .desktop
 * file, used by dc_apps_find_by_mime()/dc_apps_find_by_category() below (the
 * Default Apps settings tab, docs/17-DEFAULT-APPS-PLAN.md) instead of
 * guessing from the app's name/id. */
#define DC_APP_MIME_MAX 512
#define DC_APP_CATS_MAX 256

/* Desktop-entry "Actions=" (docs/POLISH.md P4 item 3) — extra commands a
 * .desktop file advertises beyond its main Exec=, e.g. a browser's "New
 * Private Window". Declared via `[Desktop Action <id>]` groups; see
 * parse_desktop_actions() in apps.c. */
#define DC_APP_ACTION_MAX 8
#define DC_APP_ACTION_NAME 64

typedef struct {
    char name[DC_APP_ACTION_NAME];
    char exec[DC_APP_EXEC]; /* field codes stripped, same convention as dc_app.exec */
} dc_app_action;

typedef struct {
    char name[DC_APP_NAME];
    char exec[DC_APP_EXEC]; /* field codes (%f/%u/...) stripped */
    char id[DC_APP_ID];     /* desktop-file basename, for icon resolution */
    /* Comment=, falling back to GenericName=; empty if the entry has neither
     * (launcher rows then show the name only). */
    char desc[DC_APP_DESC];
    int score; /* scratch, set during search */

    /* Raw ';'-terminated MimeType=/Categories= values (empty if the entry has
     * neither); see dc_apps_find_by_mime()/dc_apps_find_by_category(). */
    char mimetypes[DC_APP_MIME_MAX];
    char categories[DC_APP_CATS_MAX];

    dc_app_action actions[DC_APP_ACTION_MAX];
    int action_count;
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

/* Exact (case-insensitive) lookup by desktop-entry id -- the app dock's
 * pinned-apps launch path (ui/dock.c) and niri app_id -> icon/name mapping.
 * Returns NULL if no indexed entry matches. */
const dc_app *dc_apps_find(const dc_apps *apps, const char *id);

/* Apps whose MimeType= contains `mime` as an exact ';'-delimited token (not a
 * substring match -- "audio/mp" won't match "audio/mpeg"). Writes up to `max`
 * pointers into `out` (pointers into the index, in the same alphabetical
 * order as dc_apps_load() sorted them) and returns the count. Used by the
 * Default Apps settings tab (docs/17-DEFAULT-APPS-PLAN.md) to enumerate real
 * per-category candidates instead of a name-keyword heuristic. */
int dc_apps_find_by_mime(const dc_apps *apps, const char *mime, const dc_app **out, int max);

/* Apps whose Categories= contains `category` as an exact ';'-delimited token.
 * Used for the three role-based rows that aren't MIME-typed at all (Web
 * Browser -> "WebBrowser", File Manager -> "FileManager", Terminal ->
 * "TerminalEmulator"), matching KDE's/DMS's own Categories=-based filtering
 * for those roles (docs/17-DEFAULT-APPS-PLAN.md sec 1.6/1.7). */
int dc_apps_find_by_category(const dc_apps *apps, const char *category, const dc_app **out,
                             int max);

/* Launch an app's Exec line, detached from DankC. */
void dc_app_launch(const dc_app *app);

/* Launch one of `app`'s Actions= entries (index into app->actions[]),
 * detached the same way as dc_app_launch(). No-op if idx is out of range. */
void dc_app_launch_action(const dc_app *app, int idx);

/* Shared plumbing behind dc_app_launch()/dc_app_launch_action(): fork+exec a
 * raw (field-code-stripped) command line, detached from DankC. Exposed so
 * other launcher-adjacent code (e.g. a future "run" pseudo-action) can reuse
 * it without going through a dc_app. */
void dc_app_launch_exec(const char *exec);

#endif /* DC_SERVICES_APPS_H */
