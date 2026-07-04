#include "services/notepad_storage.h"

#include "core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"

#define DC_NOTEPAD_CAP 64
#define DC_NOTEPAD_TITLE_MAX 128

typedef struct {
    long long id;
    char title[DC_NOTEPAD_TITLE_MAX];
    char file_path[160]; /* relative to state_dir, e.g. "notepad-files/untitled-<id>.txt" */
    long long last_modified;
} dc_notepad_tab;

struct dc_notepad_storage {
    dc_notepad_tab tabs[DC_NOTEPAD_CAP];
    int n;
    int current;
    long long last_id;  /* monotonic guard so back-to-back create_tab() calls
                          * within the same millisecond still get unique ids */
    char state_dir[512]; /* .../dankc, no trailing slash */
    bool have_state_dir;
};

/* Resolve ~/.local/state/dankc — same XDG-state convention as
 * services/history.c's history_path()/config.c's config_path(), minus the
 * filename (notepad has two files under this dir: the session json and the
 * notepad-files/ subdir). Returns false if neither XDG_STATE_HOME nor HOME
 * is set. */
static bool state_dir(char *out, size_t n)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%.470s/dankc", xdg);
    else if (home)
        snprintf(out, n, "%.470s/.local/state/dankc", home);
    else
        return false;
    return true;
}

/* mkdir every path component of `path` (idempotent: EEXIST is fine). Used
 * instead of history.c's fixed two-level ensure_parent_dir() because
 * notepad-files/ is one level deeper than notepad-session.json. */
static void ensure_dir_chain(const char *path)
{
    char buf[600];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1 << 20) { /* sanity cap: 1 MiB, matches text_edit's buffer cap */
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long new_id(dc_notepad_storage *st)
{
    long long id = now_ms();
    if (id <= st->last_id)
        id = st->last_id + 1;
    st->last_id = id;
    return id;
}

/* Append a new tab titled `title`, deriving its id + on-disk file_path.
 * Returns its index, or the last valid index if the tab table is full
 * (DC_NOTEPAD_CAP is generous; hitting it is not expected in practice). */
static int add_tab(dc_notepad_storage *st, const char *title)
{
    if (st->n >= DC_NOTEPAD_CAP)
        return st->n > 0 ? st->n - 1 : 0;

    dc_notepad_tab *t = &st->tabs[st->n];
    memset(t, 0, sizeof(*t));
    t->id = new_id(st);
    snprintf(t->title, sizeof(t->title), "%s", title);
    snprintf(t->file_path, sizeof(t->file_path), "notepad-files/untitled-%lld.txt", t->id);
    t->last_modified = t->id;
    return st->n++;
}

static bool tab_abs_path(const dc_notepad_storage *st, int i, char *out, size_t n)
{
    if (!st || !st->have_state_dir || i < 0 || i >= st->n)
        return false;
    snprintf(out, n, "%s/%s", st->state_dir, st->tabs[i].file_path);
    return true;
}

dc_notepad_storage *dc_notepad_storage_load(void)
{
    dc_notepad_storage *st = calloc(1, sizeof(*st));

    st->have_state_dir = state_dir(st->state_dir, sizeof(st->state_dir));
    if (st->have_state_dir) {
        ensure_dir_chain(st->state_dir);
        char files_dir[600];
        snprintf(files_dir, sizeof(files_dir), "%s/notepad-files", st->state_dir);
        ensure_dir_chain(files_dir);
    }

    if (st->have_state_dir) {
        char meta[700];
        snprintf(meta, sizeof(meta), "%s/notepad-session.json", st->state_dir);
        char *text = read_file(meta);
        if (text) {
            cJSON *root = cJSON_Parse(text);
            free(text);
            if (root) {
                cJSON *tabs = cJSON_GetObjectItemCaseSensitive(root, "tabs");
                if (cJSON_IsArray(tabs)) {
                    cJSON *item = NULL;
                    cJSON_ArrayForEach(item, tabs)
                    {
                        if (st->n >= DC_NOTEPAD_CAP)
                            break;
                        cJSON *id_j = cJSON_GetObjectItemCaseSensitive(item, "id");
                        cJSON *title_j = cJSON_GetObjectItemCaseSensitive(item, "title");
                        cJSON *lm_j = cJSON_GetObjectItemCaseSensitive(item, "lastModified");
                        /* Guard: ids must be numeric and positive -- reject
                         * anything else rather than trust arbitrary input. */
                        if (!cJSON_IsNumber(id_j) || id_j->valuedouble <= 0)
                            continue;

                        dc_notepad_tab *t = &st->tabs[st->n];
                        memset(t, 0, sizeof(*t));
                        t->id = (long long)id_j->valuedouble;

                        const char *title = (cJSON_IsString(title_j) && title_j->valuestring)
                                                 ? title_j->valuestring
                                                 : "Untitled";
                        snprintf(t->title, sizeof(t->title), "%.*s", DC_NOTEPAD_TITLE_MAX - 1,
                                 title);

                        /* Guard against path traversal: the on-disk path is
                         * always DERIVED from the numeric id, never taken
                         * from the JSON's own "filePath" string. */
                        snprintf(t->file_path, sizeof(t->file_path),
                                 "notepad-files/untitled-%lld.txt", t->id);

                        t->last_modified = cJSON_IsNumber(lm_j) ? (long long)lm_j->valuedouble
                                                                 : t->id;
                        if (t->id > st->last_id)
                            st->last_id = t->id;
                        st->n++;
                    }
                }
                cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "currentTabIndex");
                if (cJSON_IsNumber(cur))
                    st->current = cur->valueint;
                cJSON_Delete(root);
            } else {
                dc_warn("notepad-session.json parse error; starting fresh");
            }
        }
    }

    bool need_save = false;
    if (st->n == 0) {
        add_tab(st, "Note 1");
        need_save = true;
    }
    if (st->current < 0 || st->current >= st->n)
        st->current = 0;

    if (need_save)
        dc_notepad_storage_save_meta(st);

    dc_debug("notepad storage loaded: %d tab(s), current=%d", st->n, st->current);
    return st;
}

void dc_notepad_storage_destroy(dc_notepad_storage *st)
{
    free(st);
}

int dc_notepad_storage_count(const dc_notepad_storage *st)
{
    return st ? st->n : 0;
}

const char *dc_notepad_storage_title(const dc_notepad_storage *st, int i)
{
    if (!st || i < 0 || i >= st->n)
        return "";
    return st->tabs[i].title;
}

int dc_notepad_storage_current(const dc_notepad_storage *st)
{
    return st ? st->current : 0;
}

void dc_notepad_storage_set_current(dc_notepad_storage *st, int i)
{
    if (!st || i < 0 || i >= st->n)
        return;
    st->current = i;
}

char *dc_notepad_storage_read(const dc_notepad_storage *st, int i)
{
    char path[700];
    if (!tab_abs_path(st, i, path, sizeof(path)))
        return strdup("");
    char *text = read_file(path);
    return text ? text : strdup("");
}

bool dc_notepad_storage_write(dc_notepad_storage *st, int i, const char *content)
{
    char path[700];
    if (!tab_abs_path(st, i, path, sizeof(path)))
        return false;
    if (!content)
        content = "";

    char tmp[720];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    if (fclose(f) != 0 || written != len) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }

    st->tabs[i].last_modified = now_ms();
    return true;
}

int dc_notepad_storage_create_tab(dc_notepad_storage *st)
{
    if (!st)
        return 0;
    char title[32];
    snprintf(title, sizeof(title), "Note %d", st->n + 1);
    int idx = add_tab(st, title);
    dc_notepad_storage_save_meta(st);
    return idx;
}

void dc_notepad_storage_delete_tab(dc_notepad_storage *st, int i)
{
    if (!st || i < 0 || i >= st->n)
        return;

    char path[700];
    if (tab_abs_path(st, i, path, sizeof(path)))
        unlink(path);

    for (int j = i; j < st->n - 1; j++)
        st->tabs[j] = st->tabs[j + 1];
    st->n--;

    if (st->n == 0)
        add_tab(st, "Note 1"); /* never allow zero tabs */

    if (st->current >= st->n)
        st->current = st->n - 1;
    if (st->current < 0)
        st->current = 0;

    dc_notepad_storage_save_meta(st);
}

void dc_notepad_storage_rename(dc_notepad_storage *st, int i, const char *title)
{
    if (!st || i < 0 || i >= st->n || !title)
        return;
    snprintf(st->tabs[i].title, sizeof(st->tabs[i].title), "%.*s", DC_NOTEPAD_TITLE_MAX - 1,
             title);
    dc_notepad_storage_save_meta(st);
}

void dc_notepad_storage_save_meta(dc_notepad_storage *st)
{
    if (!st || !st->have_state_dir)
        return;

    cJSON *root = cJSON_CreateObject();
    cJSON *tabs = cJSON_CreateArray();
    for (int i = 0; i < st->n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)st->tabs[i].id);
        cJSON_AddStringToObject(o, "title", st->tabs[i].title);
        cJSON_AddStringToObject(o, "filePath", st->tabs[i].file_path);
        cJSON_AddNumberToObject(o, "lastModified", (double)st->tabs[i].last_modified);
        cJSON_AddItemToArray(tabs, o);
    }
    cJSON_AddItemToObject(root, "tabs", tabs);
    cJSON_AddNumberToObject(root, "currentTabIndex", st->current);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text)
        return;

    char path[700];
    snprintf(path, sizeof(path), "%s/notepad-session.json", st->state_dir);
    char tmp[720];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
        rename(tmp, path); /* atomic on the same filesystem */
    } else {
        dc_warn("could not write %s", tmp);
    }
    free(text);
}
