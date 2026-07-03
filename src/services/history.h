/* history.h — launch-history tracking for the app launcher (docs/POLISH.md
 * P4 item 2): counts + last-launch timestamps per desktop-entry id,
 * persisted to ~/.local/state/dankc/launch_history.json (cJSON), mirroring
 * DMS's AppUsageHistoryData.qml (appusage.json: usageCount/lastUsed) so the
 * empty-query launcher view can be frequency-ranked the same way DMS's
 * Scorer.js does (score = usageCount*100 when there's no query — see
 * Scorer.js's `score()`). */
#ifndef DC_SERVICES_HISTORY_H
#define DC_SERVICES_HISTORY_H

typedef struct dc_history dc_history;

/* Load launch_history.json (or start empty if missing/unreadable). Never
 * NULL. */
dc_history *dc_history_load(void);
void dc_history_destroy(dc_history *h);

/* Record one launch of `app_id`: bumps its usage count, stamps "now" as its
 * last-used time, and persists immediately (launches are infrequent enough
 * that a synchronous write is fine). */
void dc_history_record(dc_history *h, const char *app_id);

/* Total recorded launches for `app_id`, or 0 if it has never been launched. */
int dc_history_count(const dc_history *h, const char *app_id);

/* Unix timestamp of the last launch of `app_id`, or 0 if never launched. */
long long dc_history_last_used(const dc_history *h, const char *app_id);

#endif /* DC_SERVICES_HISTORY_H */
