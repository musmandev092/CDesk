/* sysmon.h — CPU + RAM usage from /proc, for the bar's cpuUsage/memUsage
 * widgets (docs/12-BAR-SPEC.md §4: "3s poll only while visible") and the
 * Processes popout's per-process table (ui/processes.c).
 *
 * No threads, no per-poll heap allocation: small stack buffers are read and
 * parsed synchronously. dc_sysmon_poll() rate-limits itself internally so
 * callers can invoke it every frame without adding I/O.
 *
 * The per-process scan (dc_sysmon_poll_processes()) is a separate, heavier
 * operation (one open+read per PID in /proc) gated behind
 * dc_sysmon_set_process_scan_enabled() so it only runs while the Processes
 * popout is actually open — main.c's 1Hz tick calls it unconditionally, but
 * it's a no-op unless enabled, and self-rate-limits to 2s once enabled.
 */
#ifndef DC_SERVICES_SYSMON_H
#define DC_SERVICES_SYSMON_H

#include <stdbool.h>
#include <sys/types.h>

/* Refresh the cached CPU/RAM percentages if at least 3s have passed since the
 * last refresh. Cheap to call unconditionally (e.g. once per bar redraw). */
void dc_sysmon_poll(void);

/* Aggregate CPU utilisation (0-100) as a delta between the two most recent
 * dc_sysmon_poll() samples. 0 until a second sample exists. */
int dc_sysmon_cpu_percent(void);

/* (MemTotal - MemAvailable) / MemTotal * 100, from the most recent poll. */
int dc_sysmon_mem_percent(void);

/* Totals from the most recent dc_sysmon_poll(), in kB (0 if not yet polled or
 * /proc/meminfo lacks the field). Used by the Processes popout's memory ring
 * ("3.3 GB" of MemTotal, "+0.0 MB" of swap). */
unsigned long dc_sysmon_mem_total_kb(void);
unsigned long dc_sysmon_mem_used_kb(void);
unsigned long dc_sysmon_swap_total_kb(void);
unsigned long dc_sysmon_swap_used_kb(void);

#define DC_SYSMON_COMM_MAX 32
#define DC_SYSMON_PROC_MAX 200 /* top-N by CPU kept for the popout's table */

typedef struct {
    int pid;
    uid_t uid;
    char comm[DC_SYSMON_COMM_MAX];
    float cpu_percent;       /* may exceed 100 for multi-threaded/core procs */
    unsigned long mem_kb;    /* VmRSS */
} dc_sysmon_proc;

/* Enable/disable the per-process /proc scan (docs/13-POPOUTS-SPEC.md
 * Processes popout: "2s poll only while open"). Disabling drops the previous-
 * sample cache, so re-enabling always starts a fresh CPU-delta baseline
 * (first post-enable sample reports 0% per-process CPU, same as
 * dc_sysmon_poll()'s own have_prev_cpu bootstrap). */
void dc_sysmon_set_process_scan_enabled(bool enabled);

/* Refresh the process table if enabled and at least 2s have passed since the
 * last scan. No-op (cheap) if disabled. Safe to call every tick. */
void dc_sysmon_poll_processes(void);

/* Copy up to `max` entries (already sorted by cpu_percent descending) from
 * the most recent scan into `out`. Returns the number copied. The full
 * process count seen in that scan (before the DC_SYSMON_PROC_MAX cap) is
 * available via dc_sysmon_process_total(). */
int dc_sysmon_processes(dc_sysmon_proc *out, int max);

/* Total number of processes seen in the most recent scan (for a "N procs"
 * label), independent of the DC_SYSMON_PROC_MAX cap on dc_sysmon_processes(). */
int dc_sysmon_process_total(void);

#endif /* DC_SERVICES_SYSMON_H */
