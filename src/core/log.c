#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static dc_log_level g_level = DC_LOG_INFO;

static const char *level_tag(dc_log_level level)
{
    switch (level) {
    case DC_LOG_ERROR:
        return "ERROR";
    case DC_LOG_WARN:
        return "WARN ";
    case DC_LOG_INFO:
        return "INFO ";
    case DC_LOG_DEBUG:
        return "DEBUG";
    default:
        return "?????";
    }
}

void dc_log_init(dc_log_level level)
{
    g_level = level;
}

void dc_log(dc_log_level level, const char *fmt, ...)
{
    if (level > g_level)
        return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char stamp[16];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

    fprintf(stderr, "%s.%03ld [%s] dankc: ", stamp, ts.tv_nsec / 1000000L, level_tag(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    fflush(stderr); /* so logs survive a crash / redirected stderr */
}
