/* log.h — leveled logging to stderr.
 *
 * The single process-wide logging sink. Not thread-safe by design: DankC runs
 * on one event-loop thread. Call dc_log_init() once at startup.
 */
#ifndef DC_CORE_LOG_H
#define DC_CORE_LOG_H

typedef enum {
    DC_LOG_ERROR = 0,
    DC_LOG_WARN,
    DC_LOG_INFO,
    DC_LOG_DEBUG,
} dc_log_level;

void dc_log_init(dc_log_level level);
void dc_log(dc_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define dc_error(...) dc_log(DC_LOG_ERROR, __VA_ARGS__)
#define dc_warn(...) dc_log(DC_LOG_WARN, __VA_ARGS__)
#define dc_info(...) dc_log(DC_LOG_INFO, __VA_ARGS__)
#define dc_debug(...) dc_log(DC_LOG_DEBUG, __VA_ARGS__)

#endif /* DC_CORE_LOG_H */
