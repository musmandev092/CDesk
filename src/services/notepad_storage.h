/* notepad_storage.h — persistence for the Notepad feature (docs/22-NOTEPAD-PLAN.md
 * sec.3): tab metadata in ~/.local/state/dankc/notepad-session.json + one
 * plain-text file per tab under notepad-files/, named the same way DMS lays
 * its notes out (untitled-<msEpochId>.txt) so a later dms_import can reuse
 * the files verbatim. All reads/writes are local file IO only -- no
 * system()/popen (main.c sets SIGCHLD=SIG_IGN, which breaks both). */
#ifndef DC_SERVICES_NOTEPAD_STORAGE_H
#define DC_SERVICES_NOTEPAD_STORAGE_H

#include <stdbool.h>

typedef struct dc_notepad_storage dc_notepad_storage;

/* Load notepad-session.json (creating ~/.local/state/dankc and its
 * notepad-files/ subdir as needed). Never NULL: a missing/corrupt session
 * file, or one with zero valid tabs, gets a single default tab (mirrors
 * DMS's createDefaultTab) and is persisted immediately. */
dc_notepad_storage *dc_notepad_storage_load(void);
void dc_notepad_storage_destroy(dc_notepad_storage *st);

/* Tab metadata. `i` out of range returns a safe default (0 / ""). */
int dc_notepad_storage_count(const dc_notepad_storage *st);
const char *dc_notepad_storage_title(const dc_notepad_storage *st, int i);
int dc_notepad_storage_current(const dc_notepad_storage *st);
void dc_notepad_storage_set_current(dc_notepad_storage *st, int i);

/* Tab content. Read returns a malloc'd buffer the caller must free() --
 * "" (never NULL) if the tab's file doesn't exist yet. Write is atomic
 * (temp file + rename()) and updates the tab's lastModified stamp but does
 * NOT itself persist notepad-session.json (call save_meta, or rely on the
 * mutating calls below which already do). */
char *dc_notepad_storage_read(const dc_notepad_storage *st, int i);
bool dc_notepad_storage_write(dc_notepad_storage *st, int i, const char *content);

/* Tab list mutation. All three persist notepad-session.json before
 * returning. create_tab returns the new tab's index (always appended at the
 * end). delete_tab unlinks the tab's file and re-indexes remaining tabs;
 * deleting the last remaining tab recreates a fresh default tab instead of
 * leaving zero tabs. rename truncates `title` to a bounded length. */
int dc_notepad_storage_create_tab(dc_notepad_storage *st);
void dc_notepad_storage_delete_tab(dc_notepad_storage *st, int i);
void dc_notepad_storage_rename(dc_notepad_storage *st, int i, const char *title);

/* Persist notepad-session.json (tab list + currentTabIndex) now. */
void dc_notepad_storage_save_meta(dc_notepad_storage *st);

#endif /* DC_SERVICES_NOTEPAD_STORAGE_H */
