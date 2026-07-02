/* processes.h — the Processes (system monitor) popout.
 *
 * A bar-adjacent popout (docs/13-POPOUTS-SPEC.md; matches DMS's
 * ProcessListPopout.qml/ProcessesView.qml): All/User/System filter tabs, a
 * live search field, a distro/uptime info card with CPU + memory rings, and
 * a scrollable, sortable process table. Opened from the bar's cpuUsage
 * (CPU-sorted) and memUsage (memory-sorted) chips. Fed by
 * services/sysmon.h's per-process scan, which only runs while this popout
 * is visible.
 */
#ifndef DC_UI_PROCESSES_H
#define DC_UI_PROCESSES_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct wl_surface;

typedef struct dc_processes dc_processes;

/* Which column the table is sorted by, descending. Matches the two triggers
 * that can open this popout (docs/13-POPOUTS-SPEC.md: cpuUsage -> CPU-
 * sorted, memUsage -> Memory-sorted), and the table's clickable CPU/Memory
 * column headers. */
typedef enum {
    DC_PROCESSES_SORT_CPU = 0,
    DC_PROCESSES_SORT_MEM = 1,
} dc_processes_sort;

dc_processes *dc_processes_create(struct dc_wayland *wl, struct dc_egl *egl,
                                  struct dc_render *render);
void dc_processes_destroy(dc_processes *ps);

/* Show on `output` (sorted by `sort`) if hidden, hide if shown. Re-opening
 * while already open just re-sorts in place rather than toggling closed --
 * see processes.c's dc_processes_toggle() for the exact rule. */
void dc_processes_toggle(dc_processes *ps, struct dc_output *output, dc_processes_sort sort);
void dc_processes_hide(dc_processes *ps);
bool dc_processes_visible(dc_processes *ps);
struct wl_surface *dc_processes_surface(dc_processes *ps);

/* Re-scan + re-render if visible (call once per tick from main.c, after
 * dc_sysmon_poll_processes() -- both are cheap no-ops while hidden). */
void dc_processes_refresh(dc_processes *ps);

void dc_processes_handle_key(dc_processes *ps, uint32_t keysym, const char *utf8);
void dc_processes_handle_click(dc_processes *ps, double x, double y);
void dc_processes_handle_scroll(dc_processes *ps, int steps_v);

#endif /* DC_UI_PROCESSES_H */
