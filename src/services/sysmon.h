/* sysmon.h — CPU + RAM usage from /proc, for the bar's cpuUsage/memUsage
 * widgets (docs/12-BAR-SPEC.md §4: "3s poll only while visible").
 *
 * No threads, no per-poll heap allocation: two small stack buffers are read
 * and parsed synchronously. dc_sysmon_poll() rate-limits itself internally so
 * callers can invoke it every frame without adding I/O.
 */
#ifndef DC_SERVICES_SYSMON_H
#define DC_SERVICES_SYSMON_H

/* Refresh the cached CPU/RAM percentages if at least 3s have passed since the
 * last refresh. Cheap to call unconditionally (e.g. once per bar redraw). */
void dc_sysmon_poll(void);

/* Aggregate CPU utilisation (0-100) as a delta between the two most recent
 * dc_sysmon_poll() samples. 0 until a second sample exists. */
int dc_sysmon_cpu_percent(void);

/* (MemTotal - MemAvailable) / MemTotal * 100, from the most recent poll. */
int dc_sysmon_mem_percent(void);

#endif /* DC_SERVICES_SYSMON_H */
