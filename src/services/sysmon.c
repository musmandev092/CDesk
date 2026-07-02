#include "services/sysmon.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/log.h"

#define DC_SYSMON_POLL_INTERVAL_SEC 3

typedef unsigned long long dc_jiffies;

static struct {
    bool polled_once;
    struct timespec last_poll;

    /* /proc/stat aggregate, previous + current sample. */
    bool have_prev_cpu;
    dc_jiffies prev_total;
    dc_jiffies prev_idle;
    int cpu_percent;

    int mem_percent;
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

    unsigned long total_kb = 0, avail_kb = 0;
    bool have_total = false, have_avail = false;
    char line[256];
    while ((have_total == false || have_avail == false) && fgets(line, sizeof(line), file)) {
        if (!have_total && sscanf(line, "MemTotal: %lu kB", &total_kb) == 1) {
            have_total = true;
            continue;
        }
        if (!have_avail && sscanf(line, "MemAvailable: %lu kB", &avail_kb) == 1) {
            have_avail = true;
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
