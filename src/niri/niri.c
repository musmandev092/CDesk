#include "niri/niri.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sort workspaces by output then index so the bar renders them in order. */
static int workspace_cmp(const void *a, const void *b)
{
    const dc_niri_workspace *wa = a, *wb = b;
    int out = strcmp(wa->output, wb->output);
    if (out != 0)
        return out;
    return (int)wa->idx - (int)wb->idx;
}
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cJSON.h"

#define DC_NIRI_BUF_CAP (128 * 1024)

struct dc_niri {
    int fd;
    char buf[DC_NIRI_BUF_CAP];
    size_t buf_len;

    dc_niri_workspace workspaces[DC_NIRI_MAX_WORKSPACES];
    int workspace_count;

    dc_niri_window windows[DC_NIRI_MAX_WINDOWS];
    int window_count;

    dc_niri_changed_cb on_changed;
    void *cb_data;
};

/* --- small cJSON helpers ------------------------------------------------ */

static double json_num(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : 0.0;
}

static bool json_bool(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsTrue(item);
}

static void json_str(char *dst, size_t cap, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

/* --- event handlers ----------------------------------------------------- */

static void handle_workspaces_changed(dc_niri *niri, const cJSON *value)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(value, "workspaces");
    if (!cJSON_IsArray(array))
        return;

    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, array)
    {
        if (n >= DC_NIRI_MAX_WORKSPACES)
            break;
        dc_niri_workspace *ws = &niri->workspaces[n++];
        memset(ws, 0, sizeof(*ws));
        ws->id = (uint64_t)json_num(entry, "id");
        ws->idx = (uint8_t)json_num(entry, "idx");
        json_str(ws->output, sizeof(ws->output), entry, "output");
        json_str(ws->name, sizeof(ws->name), entry, "name");
        ws->is_focused = json_bool(entry, "is_focused");
        ws->is_active = json_bool(entry, "is_active");
        ws->is_urgent = json_bool(entry, "is_urgent");
    }
    niri->workspace_count = n;
    qsort(niri->workspaces, (size_t)n, sizeof(niri->workspaces[0]), workspace_cmp);
}

static dc_niri_workspace *find_workspace(dc_niri *niri, uint64_t id)
{
    for (int i = 0; i < niri->workspace_count; i++)
        if (niri->workspaces[i].id == id)
            return &niri->workspaces[i];
    return NULL;
}

static void handle_workspace_activated(dc_niri *niri, const cJSON *value)
{
    dc_niri_workspace *target = find_workspace(niri, (uint64_t)json_num(value, "id"));
    if (!target)
        return;
    bool focused = json_bool(value, "focused");

    for (int i = 0; i < niri->workspace_count; i++) {
        dc_niri_workspace *ws = &niri->workspaces[i];
        if (strcmp(ws->output, target->output) != 0)
            continue;
        ws->is_active = false;
        if (focused)
            ws->is_focused = false;
    }
    target->is_active = true;
    if (focused)
        target->is_focused = true;
}

/* --- windows ------------------------------------------------------------ */

static void parse_window(dc_niri_window *win, const cJSON *entry)
{
    memset(win, 0, sizeof(*win));
    win->id = (uint64_t)json_num(entry, "id");
    win->workspace_id = (uint64_t)json_num(entry, "workspace_id");
    json_str(win->title, sizeof(win->title), entry, "title");
    json_str(win->app_id, sizeof(win->app_id), entry, "app_id");
    win->is_focused = json_bool(entry, "is_focused");
}

static void clear_focus_except(dc_niri *niri, uint64_t keep_id)
{
    for (int i = 0; i < niri->window_count; i++)
        if (niri->windows[i].id != keep_id)
            niri->windows[i].is_focused = false;
}

static void handle_windows_changed(dc_niri *niri, const cJSON *value)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(value, "windows");
    if (!cJSON_IsArray(array))
        return;

    int n = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, array)
    {
        if (n >= DC_NIRI_MAX_WINDOWS)
            break;
        parse_window(&niri->windows[n++], entry);
    }
    niri->window_count = n;
}

static void handle_window_opened_or_changed(dc_niri *niri, const cJSON *value)
{
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(value, "window");
    if (!cJSON_IsObject(entry))
        return;

    dc_niri_window win;
    parse_window(&win, entry);

    for (int i = 0; i < niri->window_count; i++) {
        if (niri->windows[i].id != win.id)
            continue;
        niri->windows[i] = win;
        if (win.is_focused)
            clear_focus_except(niri, win.id);
        return;
    }
    if (niri->window_count < DC_NIRI_MAX_WINDOWS) {
        niri->windows[niri->window_count++] = win;
        if (win.is_focused)
            clear_focus_except(niri, win.id);
    }
}

static void handle_window_closed(dc_niri *niri, const cJSON *value)
{
    uint64_t id = (uint64_t)json_num(value, "id");
    for (int i = 0; i < niri->window_count; i++) {
        if (niri->windows[i].id != id)
            continue;
        niri->windows[i] = niri->windows[--niri->window_count];
        return;
    }
}

static void handle_window_focus_changed(dc_niri *niri, const cJSON *value)
{
    /* "id" is null when nothing is focused. */
    uint64_t id = (uint64_t)json_num(value, "id");
    for (int i = 0; i < niri->window_count; i++)
        niri->windows[i].is_focused = (niri->windows[i].id == id && id != 0);
}

/* Returns true if the workspace view changed. */
static bool handle_line(dc_niri *niri, const char *line, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root)
        return false;

    /* The initial reply and request acks are {"Ok":...}/{"Err":...}. */
    if (cJSON_HasObjectItem(root, "Ok") || cJSON_HasObjectItem(root, "Err")) {
        cJSON_Delete(root);
        return false;
    }

    /* Events are single-key objects: {"<EventName>": {...}}. */
    bool changed = false;
    const cJSON *event = root->child;
    if (event && event->string) {
        if (strcmp(event->string, "WorkspacesChanged") == 0) {
            handle_workspaces_changed(niri, event);
            changed = true;
        } else if (strcmp(event->string, "WorkspaceActivated") == 0) {
            handle_workspace_activated(niri, event);
            changed = true;
        } else if (strcmp(event->string, "WindowsChanged") == 0) {
            handle_windows_changed(niri, event);
            changed = true;
        } else if (strcmp(event->string, "WindowOpenedOrChanged") == 0) {
            handle_window_opened_or_changed(niri, event);
            changed = true;
        } else if (strcmp(event->string, "WindowClosed") == 0) {
            handle_window_closed(niri, event);
            changed = true;
        } else if (strcmp(event->string, "WindowFocusChanged") == 0) {
            handle_window_focus_changed(niri, event);
            changed = true;
        }
    }

    cJSON_Delete(root);
    return changed;
}

/* --- socket ------------------------------------------------------------- */

static void niri_readable(int fd, uint32_t revents, void *data)
{
    DC_UNUSED(revents);
    dc_niri *niri = data;

    ssize_t got = read(fd, niri->buf + niri->buf_len, sizeof(niri->buf) - niri->buf_len - 1);
    if (got <= 0) {
        if (got == 0)
            dc_warn("niri socket closed");
        return;
    }
    niri->buf_len += (size_t)got;

    size_t start = 0;
    bool changed = false;
    for (size_t i = 0; i < niri->buf_len; i++) {
        if (niri->buf[i] != '\n')
            continue;
        changed |= handle_line(niri, niri->buf + start, i - start);
        start = i + 1;
    }
    if (start > 0) {
        memmove(niri->buf, niri->buf + start, niri->buf_len - start);
        niri->buf_len -= start;
    }
    /* A single oversized line would wedge the buffer; drop it defensively. */
    if (niri->buf_len >= sizeof(niri->buf) - 1)
        niri->buf_len = 0;

    if (changed && niri->on_changed)
        niri->on_changed(niri->cb_data);
}

dc_niri *dc_niri_connect(void)
{
    const char *path = getenv("NIRI_SOCKET");
    if (!path || !*path) {
        dc_warn("NIRI_SOCKET unset; workspaces disabled");
        return NULL;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dc_error("failed to connect NIRI_SOCKET (%s)", path);
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    dc_niri *niri = calloc(1, sizeof(*niri));
    niri->fd = fd;

    static const char request[] = "\"EventStream\"\n";
    if (write(fd, request, sizeof(request) - 1) < 0)
        dc_error("niri: failed to request EventStream");
    dc_info("niri IPC connected (EventStream)");
    return niri;
}

void dc_niri_destroy(dc_niri *niri)
{
    if (!niri)
        return;
    if (niri->fd >= 0)
        close(niri->fd);
    free(niri);
}

void dc_niri_integrate(dc_niri *niri, struct dc_loop *loop)
{
    if (niri)
        dc_loop_add_fd(loop, niri->fd, POLLIN, niri_readable, niri);
}

void dc_niri_set_changed_cb(dc_niri *niri, dc_niri_changed_cb cb, void *user_data)
{
    if (!niri)
        return;
    niri->on_changed = cb;
    niri->cb_data = user_data;
}

const dc_niri_workspace *dc_niri_workspaces(const dc_niri *niri, int *count)
{
    if (!niri) {
        *count = 0;
        return NULL;
    }
    *count = niri->workspace_count;
    return niri->workspaces;
}

const dc_niri_window *dc_niri_focused_window(const dc_niri *niri)
{
    if (!niri)
        return NULL;
    for (int i = 0; i < niri->window_count; i++)
        if (niri->windows[i].is_focused)
            return &niri->windows[i];
    return NULL;
}

void dc_niri_focus_workspace(int idx)
{
    char idx_str[16];
    snprintf(idx_str, sizeof(idx_str), "%d", idx);

    pid_t pid = fork();
    if (pid == 0) {
        execlp("niri", "niri", "msg", "action", "focus-workspace", idx_str, (char *)NULL);
        _exit(127);
    }
    /* Parent: fire-and-forget, reaped by main's SIGCHLD = SIG_IGN. */
}

/* Fire-and-forget `niri msg action <name>` with no further arguments — the
 * shared shape behind the scroll-driven focus actions below (docs/12-BAR-SPEC.md
 * sec.5). */
static void niri_spawn_action(const char *name)
{
    pid_t pid = fork();
    if (pid == 0) {
        execlp("niri", "niri", "msg", "action", name, (char *)NULL);
        _exit(127);
    }
    /* Parent: fire-and-forget, reaped by main's SIGCHLD = SIG_IGN. */
}

void dc_niri_focus_workspace_down(void)
{
    niri_spawn_action("focus-workspace-down");
}

void dc_niri_focus_workspace_up(void)
{
    niri_spawn_action("focus-workspace-up");
}

void dc_niri_focus_column_left(void)
{
    niri_spawn_action("focus-column-left");
}

void dc_niri_focus_column_right(void)
{
    niri_spawn_action("focus-column-right");
}
