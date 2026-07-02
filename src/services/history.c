#include "services/history.h"

#include "core/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"

#define DC_HISTORY_CAP 512
#define DC_HISTORY_ID 192

typedef struct {
    char id[DC_HISTORY_ID];
    int count;
    long long last_used;
} dc_history_entry;

struct dc_history {
    dc_history_entry items[DC_HISTORY_CAP];
    int n;
};

/* Resolve launch_history.json's path — same XDG-state convention as
 * config.c's config_path() but under XDG_STATE_HOME/~/.local/state, per
 * docs/POLISH.md P4 item 2. Returns false if neither is set. */
static bool history_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%.470s/dankc/launch_history.json", xdg);
    else if (home)
        snprintf(out, n, "%.470s/.local/state/dankc/launch_history.json", home);
    else
        return false;
    return true;
}

/* Create the parent directory of `path` (one level: .../dankc/) — identical
 * two-mkdir convention to config.c's ensure_parent_dir(). */
static void ensure_parent_dir(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';
    char *slash2 = strrchr(dir, '/');
    if (slash2) {
        *slash2 = '\0';
        mkdir(dir, 0755);
        *slash2 = '/';
    }
    mkdir(dir, 0755);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1 << 20) { /* sanity cap: 1 MiB */
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

dc_history *dc_history_load(void)
{
    dc_history *h = calloc(1, sizeof(*h));

    char path[512];
    if (!history_path(path, sizeof(path)))
        return h;
    char *text = read_file(path);
    if (!text)
        return h;

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root)
        return h;

    cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (cJSON_IsObject(apps)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, apps)
        {
            if (h->n >= DC_HISTORY_CAP || !item->string)
                continue;
            cJSON *count = cJSON_GetObjectItemCaseSensitive(item, "count");
            cJSON *last = cJSON_GetObjectItemCaseSensitive(item, "lastUsed");
            dc_history_entry *e = &h->items[h->n++];
            snprintf(e->id, sizeof(e->id), "%s", item->string);
            e->count = cJSON_IsNumber(count) ? count->valueint : 0;
            e->last_used = cJSON_IsNumber(last) ? (long long)last->valuedouble : 0;
        }
    }
    cJSON_Delete(root);
    dc_debug("launch history loaded: %d apps", h->n);
    return h;
}

void dc_history_destroy(dc_history *h)
{
    free(h);
}

static dc_history_entry *find(dc_history *h, const char *id)
{
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->items[i].id, id) == 0)
            return &h->items[i];
    return NULL;
}

static void save(const dc_history *h)
{
    char path[512];
    if (!history_path(path, sizeof(path)))
        return;
    ensure_parent_dir(path);

    cJSON *root = cJSON_CreateObject();
    cJSON *apps = cJSON_CreateObject();
    for (int i = 0; i < h->n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "count", h->items[i].count);
        cJSON_AddNumberToObject(o, "lastUsed", (double)h->items[i].last_used);
        cJSON_AddItemToObject(apps, h->items[i].id, o);
    }
    cJSON_AddItemToObject(root, "apps", apps);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text)
        return;

    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
    free(text);
}

void dc_history_record(dc_history *h, const char *app_id)
{
    if (!h || !app_id || !app_id[0])
        return;

    dc_history_entry *e = find(h, app_id);
    if (!e) {
        if (h->n >= DC_HISTORY_CAP)
            return; /* history full; drop silently (rare, non-fatal) */
        e = &h->items[h->n++];
        snprintf(e->id, sizeof(e->id), "%s", app_id);
        e->count = 0;
        e->last_used = 0;
    }
    e->count++;
    e->last_used = (long long)time(NULL);
    save(h);
}

int dc_history_count(const dc_history *h, const char *app_id)
{
    if (!h || !app_id)
        return 0;
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->items[i].id, app_id) == 0)
            return h->items[i].count;
    return 0;
}

long long dc_history_last_used(const dc_history *h, const char *app_id)
{
    if (!h || !app_id)
        return 0;
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->items[i].id, app_id) == 0)
            return h->items[i].last_used;
    return 0;
}
