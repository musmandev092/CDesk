#include "services/sysmon.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"

#define DC_SYSMON_POLL_INTERVAL_SEC 3
#define DC_SYSMON_PROC_POLL_INTERVAL_SEC 2
/* Upper bound on how many PIDs a single scan tracks for CPU-delta purposes.
 * Comfortably above any desktop's live process count (docs/13-POPOUTS-
 * SPEC.md Processes popout reference: ~230); only the top
 * DC_SYSMON_PROC_MAX by CPU are exposed via dc_sysmon_processes(), but the
 * *delta* cache needs to cover the whole population so a process that's
 * cold this poll but hot next poll still gets a correct percentage. */
#define DC_SYSMON_SCAN_MAX 1024

typedef unsigned long long dc_jiffies;

typedef struct {
    int pid;
    dc_jiffies ticks; /* utime + stime, in clock ticks */
} dc_sysmon_pid_ticks;

static struct {
    bool polled_once;
    struct timespec last_poll;

    /* /proc/stat aggregate, previous + current sample. */
    bool have_prev_cpu;
    dc_jiffies prev_total;
    dc_jiffies prev_idle;
    int cpu_percent;

    int mem_percent;
    unsigned long mem_total_kb;
    unsigned long mem_used_kb;
    unsigned long swap_total_kb;
    unsigned long swap_used_kb;

    /* Per-process scan state (docs/13-POPOUTS-SPEC.md Processes popout). */
    bool proc_scan_enabled;
    bool proc_polled_once;
    struct timespec last_proc_poll;

    bool have_prev_ticks;
    struct timespec prev_scan_time;
    dc_sysmon_pid_ticks prev_ticks[DC_SYSMON_SCAN_MAX];
    int prev_ticks_count;

    dc_sysmon_proc top[DC_SYSMON_PROC_MAX];
    int top_count;
    int scan_total;

    int temp_c; /* -1 until a sensor is read */
} g_sysmon;

static long secs_since(const struct timespec *from, const struct timespec *now)
{
    return now->tv_sec - from->tv_sec;
}

/* Reads the aggregate "cpu " line of /proc/stat and folds it into a
 * (total, idle) pair. Returns false if the line couldn't be read/parsed. */
static bool read_proc_stat(dc_jiffies *total, dc_jiffies *idle)
{
    FILE *file = fopen("/proc/stat", "r");
    if (!file)
        return false;

    char line[256];
    bool ok = fgets(line, sizeof(line), file) != NULL;
    fclose(file);
    if (!ok)
        return false;

    dc_jiffies user = 0, nice = 0, system = 0, idle_t = 0, iowait = 0, irq = 0, softirq = 0,
               steal = 0;
    /* Older kernels may omit trailing fields (steal, guest, ...); sscanf
     * leaves unmatched fields at their zero-initialised default. */
    int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &system,
                    &idle_t, &iowait, &irq, &softirq, &steal);
    if (n < 4)
        return false;

    *idle = idle_t + iowait;
    *total = user + nice + system + idle_t + iowait + irq + softirq + steal;
    return true;
}

static void update_cpu_percent(void)
{
    dc_jiffies total, idle;
    if (!read_proc_stat(&total, &idle))
        return;

    if (g_sysmon.have_prev_cpu) {
        dc_jiffies dt = total - g_sysmon.prev_total;
        dc_jiffies di = idle - g_sysmon.prev_idle;
        if (dt > 0) {
            dc_jiffies busy = dt > di ? dt - di : 0;
            g_sysmon.cpu_percent = (int)((busy * 100 + dt / 2) / dt);
        }
    }

    g_sysmon.prev_total = total;
    g_sysmon.prev_idle = idle;
    g_sysmon.have_prev_cpu = true;
}

static void update_mem_percent(void)
{
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file)
        return;

    unsigned long total_kb = 0, avail_kb = 0, swap_total_kb = 0, swap_free_kb = 0;
    bool have_total = false, have_avail = false, have_swap_total = false, have_swap_free = false;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (!have_total && sscanf(line, "MemTotal: %lu kB", &total_kb) == 1) {
            have_total = true;
            continue;
        }
        if (!have_avail && sscanf(line, "MemAvailable: %lu kB", &avail_kb) == 1) {
            have_avail = true;
            continue;
        }
        if (!have_swap_total && sscanf(line, "SwapTotal: %lu kB", &swap_total_kb) == 1) {
            have_swap_total = true;
            continue;
        }
        if (!have_swap_free && sscanf(line, "SwapFree: %lu kB", &swap_free_kb) == 1) {
            have_swap_free = true;
            continue;
        }
    }
    fclose(file);

    if (!have_total || total_kb == 0) {
        dc_warn("sysmon: /proc/meminfo missing MemTotal");
        return;
    }
    if (!have_avail) /* very old kernels lack MemAvailable: treat as fully used */
        avail_kb = 0;

    unsigned long used_kb = total_kb > avail_kb ? total_kb - avail_kb : 0;
    g_sysmon.mem_percent = (int)((used_kb * 100 + total_kb / 2) / total_kb);
    g_sysmon.mem_total_kb = total_kb;
    g_sysmon.mem_used_kb = used_kb;
    g_sysmon.swap_total_kb = have_swap_total ? swap_total_kb : 0;
    g_sysmon.swap_used_kb =
        (have_swap_total && have_swap_free && swap_total_kb > swap_free_kb)
            ? swap_total_kb - swap_free_kb
            : 0;
}

/* Read one millidegree-Celsius integer from `path` into `*out_c` (whole °C).
 * Returns true on success. */
static bool read_millideg(const char *path, int *out_c)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return false;
    long milli = 0;
    bool ok = fscanf(file, "%ld", &milli) == 1;
    fclose(file);
    if (!ok || milli <= 0)
        return false;
    *out_c = (int)((milli + 500) / 1000);
    return true;
}

/* CPU package temperature. Prefer a /sys/class/thermal zone whose `type`
 * mentions the CPU package (x86_pkg_temp / cpu / coretemp / k10temp), else the
 * first readable zone, else a hwmon tempN_input. -1 if nothing is readable.
 * No allocation, mirrors the other /sys readers here. */
static void update_temp_c(void)
{
    int best = -1;
    int fallback = -1;

    DIR *dir = opendir("/sys/class/thermal");
    if (dir) {
        struct dirent *ent;
        char path[320], type[64];
        while ((ent = readdir(dir))) {
            if (strncmp(ent->d_name, "thermal_zone", 12) != 0)
                continue;
            int val;
            snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", ent->d_name);
            if (!read_millideg(path, &val))
                continue;
            if (fallback < 0)
                fallback = val;

            snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", ent->d_name);
            FILE *tf = fopen(path, "r");
            if (tf) {
                if (fgets(type, sizeof(type), tf)) {
                    if (strstr(type, "x86_pkg_temp") || strstr(type, "coretemp") ||
                        strstr(type, "k10temp") || strstr(type, "cpu") || strstr(type, "CPU")) {
                        best = val;
                    }
                }
                fclose(tf);
            }
            if (best >= 0)
                break;
        }
        closedir(dir);
    }

    int result = best >= 0 ? best : fallback;
    if (result < 0) {
        /* hwmon fallback: first hwmonN/temp1_input we can read. */
        DIR *hd = opendir("/sys/class/hwmon");
        if (hd) {
            struct dirent *ent;
            char path[320];
            while ((ent = readdir(hd))) {
                if (ent->d_name[0] == '.')
                    continue;
                int val;
                snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input", ent->d_name);
                if (read_millideg(path, &val)) {
                    result = val;
                    break;
                }
            }
            closedir(hd);
        }
    }

    g_sysmon.temp_c = result;
}

void dc_sysmon_poll(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (g_sysmon.polled_once &&
        secs_since(&g_sysmon.last_poll, &now) < DC_SYSMON_POLL_INTERVAL_SEC)
        return;

    update_cpu_percent();
    update_mem_percent();
    update_temp_c();

    g_sysmon.last_poll = now;
    g_sysmon.polled_once = true;
}

int dc_sysmon_cpu_percent(void)
{
    return g_sysmon.cpu_percent;
}

int dc_sysmon_mem_percent(void)
{
    return g_sysmon.mem_percent;
}

unsigned long dc_sysmon_mem_total_kb(void)
{
    return g_sysmon.mem_total_kb;
}

unsigned long dc_sysmon_mem_used_kb(void)
{
    return g_sysmon.mem_used_kb;
}

unsigned long dc_sysmon_swap_total_kb(void)
{
    return g_sysmon.swap_total_kb;
}

unsigned long dc_sysmon_swap_used_kb(void)
{
    return g_sysmon.swap_used_kb;
}

/* --- per-process scan (Processes popout) --------------------------------- */

void dc_sysmon_set_process_scan_enabled(bool enabled)
{
    if (enabled == g_sysmon.proc_scan_enabled)
        return;
    g_sysmon.proc_scan_enabled = enabled;
    if (enabled) {
        /* Force the next dc_sysmon_poll_processes() call to scan immediately
         * rather than waiting out the 2s rate limit, and drop the stale
         * previous-tick cache so CPU-delta math starts from a clean
         * baseline (a long-disabled period would otherwise read as a huge
         * elapsed time with huge tick deltas). */
        g_sysmon.proc_polled_once = false;
        g_sysmon.have_prev_ticks = false;
        g_sysmon.prev_ticks_count = 0;
    }
}

/* Read one /proc/<pid>/stat line's utime+stime (clock ticks). The comm field
 * is parenthesized and may itself contain spaces or parens, so this finds
 * the *last* ')' before parsing the fixed-position fields that follow (the
 * standard robust approach — ps(1)/htop use the same trick). Returns false
 * if the file is unreadable or malformed. */
static bool read_proc_pid_ticks(int pid, dc_jiffies *out_ticks)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[1024];
    bool ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok)
        return false;

    char *p = strrchr(line, ')');
    if (!p)
        return false;
    p++;

    /* Fields after the comm's closing paren, per proc(5): state ppid pgrp
     * session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime.
     * Walked by hand (rather than sscanf's "%*lu" skip-fields) since gcc's
     * format checker flags assignment-suppression combined with a length
     * modifier as suspicious even though it's valid C99/POSIX -- this side-
     * steps the warning and is no less readable. */
    for (int i = 0; i < 11; i++) {
        while (*p == ' ')
            p++;
        while (*p && *p != ' ')
            p++;
    }
    while (*p == ' ')
        p++;
    char *end = NULL;
    unsigned long long utime = strtoull(p, &end, 10);
    if (end == p)
        return false;
    p = end;
    while (*p == ' ')
        p++;
    unsigned long long stime = strtoull(p, &end, 10);
    if (end == p)
        return false;

    *out_ticks = (dc_jiffies)(utime + stime);
    return true;
}

/* comm (task name) from /proc/<pid>/comm — a single line, already truncated
 * by the kernel to whatever TASK_COMM_LEN the running kernel uses, newline-
 * terminated. Simpler and more robust than re-deriving it from stat's
 * parenthesized field. */
static void read_proc_pid_comm(int pid, char *out, size_t outsz)
{
    out[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(out, (int)outsz, f)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
    }
    fclose(f);
}

/* uid + VmRSS from /proc/<pid>/status. Both live in the same file, so one
 * open covers both rather than a second /proc round-trip. */
static bool read_proc_pid_status(int pid, uid_t *out_uid, unsigned long *out_rss_kb)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    bool have_uid = false, have_rss = false;
    unsigned long uid = 0;
    char line[256];
    while ((!have_uid || !have_rss) && fgets(line, sizeof(line), f)) {
        if (!have_uid && sscanf(line, "Uid: %lu", &uid) == 1) {
            have_uid = true;
            continue;
        }
        if (!have_rss && sscanf(line, "VmRSS: %lu kB", out_rss_kb) == 1) {
            have_rss = true;
            continue;
        }
    }
    fclose(f);
    if (!have_rss)
        *out_rss_kb = 0;
    *out_uid = (uid_t)uid;
    return true; /* a process with no VmRSS line (e.g. a zombie) is still valid, just 0 kB */
}

static dc_jiffies find_prev_ticks(int pid)
{
    for (int i = 0; i < g_sysmon.prev_ticks_count; i++)
        if (g_sysmon.prev_ticks[i].pid == pid)
            return g_sysmon.prev_ticks[i].ticks;
    return 0;
}

static int cmp_proc_cpu_desc(const void *a, const void *b)
{
    const dc_sysmon_proc *pa = a, *pb = b;
    if (pa->cpu_percent > pb->cpu_percent)
        return -1;
    if (pa->cpu_percent < pb->cpu_percent)
        return 1;
    return pa->pid - pb->pid;
}

void dc_sysmon_poll_processes(void)
{
    if (!g_sysmon.proc_scan_enabled)
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (g_sysmon.proc_polled_once &&
        secs_since(&g_sysmon.last_proc_poll, &now) < DC_SYSMON_PROC_POLL_INTERVAL_SEC)
        return;

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0)
        clk_tck = 100;

    double elapsed = g_sysmon.have_prev_ticks
                         ? (double)(now.tv_sec - g_sysmon.prev_scan_time.tv_sec) +
                               (double)(now.tv_nsec - g_sysmon.prev_scan_time.tv_nsec) / 1e9
                         : 0.0;

    DIR *dir = opendir("/proc");
    if (!dir) {
        dc_warn("sysmon: opendir(/proc) failed");
        return;
    }

    /* Scanned this poll, matched against g_sysmon.prev_ticks for the CPU
     * delta, then becomes next poll's prev_ticks. Static (not stack): ~1024 *
     * sizeof(dc_sysmon_proc) is too large for a comfortable stack frame. */
    static dc_sysmon_proc scan[DC_SYSMON_SCAN_MAX];
    static dc_sysmon_pid_ticks scan_ticks[DC_SYSMON_SCAN_MAX];
    int scan_count = 0;
    int total = 0;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!isdigit((unsigned char)ent->d_name[0]))
            continue;
        int pid = atoi(ent->d_name);
        if (pid <= 0)
            continue;
        total++;

        dc_jiffies ticks;
        if (!read_proc_pid_ticks(pid, &ticks))
            continue; /* process exited between readdir() and open(); skip */

        uid_t uid;
        unsigned long rss_kb;
        if (!read_proc_pid_status(pid, &uid, &rss_kb))
            continue;

        char comm[DC_SYSMON_COMM_MAX];
        read_proc_pid_comm(pid, comm, sizeof(comm));

        float cpu_percent = 0.0f;
        if (g_sysmon.have_prev_ticks && elapsed > 0.05) {
            dc_jiffies prev = find_prev_ticks(pid);
            if (prev > 0 && ticks >= prev) {
                double delta_ticks = (double)(ticks - prev);
                cpu_percent = (float)((delta_ticks / (double)clk_tck) / elapsed * 100.0);
            }
        }

        if (scan_count < DC_SYSMON_SCAN_MAX) {
            dc_sysmon_proc *p = &scan[scan_count];
            p->pid = pid;
            p->uid = uid;
            snprintf(p->comm, sizeof(p->comm), "%s", comm[0] ? comm : "?");
            p->cpu_percent = cpu_percent;
            p->mem_kb = rss_kb;
            scan_ticks[scan_count].pid = pid;
            scan_ticks[scan_count].ticks = ticks;
            scan_count++;
        }
    }
    closedir(dir);

    qsort(scan, (size_t)scan_count, sizeof(scan[0]), cmp_proc_cpu_desc);

    g_sysmon.top_count = scan_count < DC_SYSMON_PROC_MAX ? scan_count : DC_SYSMON_PROC_MAX;
    memcpy(g_sysmon.top, scan, (size_t)g_sysmon.top_count * sizeof(scan[0]));
    g_sysmon.scan_total = total;

    memcpy(g_sysmon.prev_ticks, scan_ticks, (size_t)scan_count * sizeof(scan_ticks[0]));
    g_sysmon.prev_ticks_count = scan_count;
    g_sysmon.prev_scan_time = now;
    g_sysmon.have_prev_ticks = true;

    g_sysmon.last_proc_poll = now;
    g_sysmon.proc_polled_once = true;
}

int dc_sysmon_processes(dc_sysmon_proc *out, int max)
{
    int n = g_sysmon.top_count < max ? g_sysmon.top_count : max;
    if (n > 0)
        memcpy(out, g_sysmon.top, (size_t)n * sizeof(out[0]));
    return n;
}

int dc_sysmon_process_total(void)
{
    return g_sysmon.scan_total;
}

int dc_sysmon_temp_c(void)
{
    return g_sysmon.polled_once ? g_sysmon.temp_c : -1;
}

#ifdef DC_SERVICE_TEST
/* Standalone smoke test: `cc -DDC_SERVICE_TEST -Isrc src/services/sysmon.c
 * src/core/log.c -o /tmp/stest` then run it — two samples 3s apart. */
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    dc_log_init(DC_LOG_DEBUG);
    dc_sysmon_poll();
    printf("sample 1: cpu=%d%% mem=%d%%\n", dc_sysmon_cpu_percent(), dc_sysmon_mem_percent());
    sleep(3);
    dc_sysmon_poll();
    printf("sample 2: cpu=%d%% mem=%d%%\n", dc_sysmon_cpu_percent(), dc_sysmon_mem_percent());
    return 0;
}
#endif
