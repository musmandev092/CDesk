#include "services/weather.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"
#include "cJSON.h"

/* Open-Meteo needs no API key; current-conditions-only keeps the response
 * (and therefore the pipe buffer) tiny. */
#define DC_WEATHER_REFRESH_SEC (15 * 60)
#define DC_WEATHER_RETRY_SEC (2 * 60)
#define DC_WEATHER_FETCH_TIMEOUT_SEC 10 /* guards against a hung curl */
#define DC_WEATHER_BUF_CAP 16384         /* daily + hourly blocks + current extras */
#define DC_WEATHER_HOURLY_REQUEST 36     /* ask for a few hours of margin past 24 (see
                                           * start_fetch(): open-meteo's forecast_hours
                                           * already starts at "now", but the extra
                                           * margin keeps the defensive time-array scan
                                           * below correct even if it doesn't). */

static struct {
    bool configured;
    double lat;
    double lon;
    bool fahrenheit;

    dc_weather_state cache;
    bool have_cache;

    /* In-flight fetch, if any. */
    bool fetch_active;
    pid_t pid;
    int fd;
    char buf[DC_WEATHER_BUF_CAP];
    size_t len;
    struct timespec fetch_started;

    struct timespec next_attempt; /* when the next fetch may start */
} g_weather;

static long secs_since(const struct timespec *from)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - from->tv_sec);
}

static void arm_next_attempt(int seconds_from_now)
{
    clock_gettime(CLOCK_MONOTONIC, &g_weather.next_attempt);
    g_weather.next_attempt.tv_sec += seconds_from_now;
}

static bool attempt_due(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec != g_weather.next_attempt.tv_sec)
        return now.tv_sec > g_weather.next_attempt.tv_sec;
    return now.tv_nsec >= g_weather.next_attempt.tv_nsec;
}

void dc_weather_init(double lat, double lon, bool fahrenheit)
{
    memset(&g_weather, 0, sizeof(g_weather));
    g_weather.lat = lat;
    g_weather.lon = lon;
    g_weather.fahrenheit = fahrenheit;
    g_weather.fd = -1;
    g_weather.configured = true;
    arm_next_attempt(0); /* fetch on the first dc_weather_get() call */
}

/* Parse a completed response into g_weather.cache. Returns true on success. */
static bool parse_response(void)
{
    cJSON *root = cJSON_ParseWithLength(g_weather.buf, g_weather.len);
    if (!root)
        return false;

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    bool ok = false;
    if (cJSON_IsObject(current)) {
        const cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
        const cJSON *is_day = cJSON_GetObjectItemCaseSensitive(current, "is_day");
        if (cJSON_IsNumber(temp) && cJSON_IsNumber(code)) {
            dc_weather_state st = {0};
            st.temp_c = (int)lround(temp->valuedouble);
            st.weather_code = code->valueint;
            st.is_day = cJSON_IsNumber(is_day) ? is_day->valueint != 0 : true;
            st.valid = true;

            /* Extended current conditions (docs/13-POPOUTS-SPEC.md sec.5). Any
             * subset may be absent on a limited response; only mark
             * have_current_extra once at least the "feels like" landed. */
            const cJSON *feels = cJSON_GetObjectItemCaseSensitive(current, "apparent_temperature");
            const cJSON *hum = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
            const cJSON *wind = cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m");
            const cJSON *press = cJSON_GetObjectItemCaseSensitive(current, "surface_pressure");
            const cJSON *precip =
                cJSON_GetObjectItemCaseSensitive(current, "precipitation_probability");
            if (cJSON_IsNumber(feels)) {
                st.feels_like = (int)lround(feels->valuedouble);
                st.have_current_extra = true;
            } else {
                st.feels_like = st.temp_c;
            }
            if (cJSON_IsNumber(hum))
                st.humidity = (int)lround(hum->valuedouble);
            if (cJSON_IsNumber(wind))
                st.wind_kmh = (int)lround(wind->valuedouble);
            if (cJSON_IsNumber(press))
                st.pressure_hpa = (int)lround(press->valuedouble);
            if (cJSON_IsNumber(precip))
                st.precip_prob = (int)lround(precip->valuedouble);

            /* 7-day daily forecast: parallel arrays keyed by index. */
            const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
            if (cJSON_IsObject(daily)) {
                const cJSON *dcode = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
                const cJSON *dmax = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
                const cJSON *dmin = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
                const cJSON *sunrise = cJSON_GetObjectItemCaseSensitive(daily, "sunrise");
                const cJSON *sunset = cJSON_GetObjectItemCaseSensitive(daily, "sunset");
                int n = cJSON_IsArray(dmax) ? cJSON_GetArraySize(dmax) : 0;
                if (n > 7)
                    n = 7;
                for (int i = 0; i < n; i++) {
                    dc_weather_daily *d = &st.daily[i];
                    const cJSON *c = cJSON_GetArrayItem(dcode, i);
                    const cJSON *mx = cJSON_GetArrayItem(dmax, i);
                    const cJSON *mn = cJSON_GetArrayItem(dmin, i);
                    const cJSON *sr = cJSON_GetArrayItem(sunrise, i);
                    const cJSON *ss = cJSON_GetArrayItem(sunset, i);
                    d->weather_code = cJSON_IsNumber(c) ? c->valueint : 0;
                    d->temp_max = cJSON_IsNumber(mx) ? (int)lround(mx->valuedouble) : 0;
                    d->temp_min = cJSON_IsNumber(mn) ? (int)lround(mn->valuedouble) : 0;
                    /* ISO "2026-07-02T05:29" -> take the "HH:MM" after 'T'. */
                    if (cJSON_IsString(sr) && sr->valuestring) {
                        const char *t = strchr(sr->valuestring, 'T');
                        if (t)
                            snprintf(d->sunrise, sizeof(d->sunrise), "%.5s", t + 1);
                    }
                    if (cJSON_IsString(ss) && ss->valuestring) {
                        const char *t = strchr(ss->valuestring, 'T');
                        if (t)
                            snprintf(d->sunset, sizeof(d->sunset), "%.5s", t + 1);
                    }
                    st.daily_count = i + 1;
                }
            }

            /* Next-24h hourly forecast: parallel arrays keyed by index, same
             * shape as `daily` above. open-meteo's `forecast_hours` request
             * parameter already starts the array at the current hour, but we
             * defensively scan for the first entry whose timestamp is >= now
             * in case that ever isn't true (DST edges, clock skew, ...). */
            const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
            if (cJSON_IsObject(hourly)) {
                const cJSON *htime = cJSON_GetObjectItemCaseSensitive(hourly, "time");
                const cJSON *hcode = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
                const cJSON *htemp = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
                int total = cJSON_IsArray(htime) ? cJSON_GetArraySize(htime) : 0;
                if (cJSON_IsArray(hcode) && cJSON_IsArray(htemp) && total > 0) {
                    time_t now = time(NULL);
                    struct tm tmnow;
                    localtime_r(&now, &tmnow);
                    char now_key[64]; /* generous: keeps -Wformat-truncation quiet */
                    snprintf(now_key, sizeof(now_key), "%04d-%02d-%02dT%02d:00",
                             tmnow.tm_year + 1900, tmnow.tm_mon + 1, tmnow.tm_mday, tmnow.tm_hour);

                    int start = 0;
                    for (int i = 0; i < total; i++) {
                        const cJSON *ti = cJSON_GetArrayItem(htime, i);
                        if (cJSON_IsString(ti) && ti->valuestring &&
                            strcmp(ti->valuestring, now_key) >= 0) {
                            start = i;
                            break;
                        }
                    }

                    int n = 0;
                    for (int i = start; i < total && n < 24; i++, n++) {
                        dc_weather_hourly *hh = &st.hourly[n];
                        const cJSON *c = cJSON_GetArrayItem(hcode, i);
                        const cJSON *tt = cJSON_GetArrayItem(htemp, i);
                        const cJSON *ti = cJSON_GetArrayItem(htime, i);
                        hh->weather_code = cJSON_IsNumber(c) ? c->valueint : 0;
                        hh->temp_c = cJSON_IsNumber(tt) ? (int)lround(tt->valuedouble) : 0;
                        hh->hour = 0;
                        if (cJSON_IsString(ti) && ti->valuestring) {
                            const char *t = strchr(ti->valuestring, 'T');
                            if (t && isdigit((unsigned char)t[1]) && isdigit((unsigned char)t[2]))
                                hh->hour = (t[1] - '0') * 10 + (t[2] - '0');
                        }
                    }
                    st.hourly_count = n;
                }
            }

            g_weather.cache = st;
            g_weather.have_cache = true;
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static void finish_fetch(bool eof_reached)
{
    close(g_weather.fd);
    g_weather.fd = -1;
    g_weather.fetch_active = false;
    /* SIGCHLD is SIG_IGN process-wide (see main.c), so the kernel reaps curl
     * for us; do not waitpid() here (it would just fail ECHILD). */

    bool ok = false;
    if (eof_reached && g_weather.len > 0)
        ok = parse_response();

    g_weather.len = 0;
    if (ok) {
        arm_next_attempt(DC_WEATHER_REFRESH_SEC);
    } else {
        dc_warn("weather: fetch failed, retrying in %ds", DC_WEATHER_RETRY_SEC);
        arm_next_attempt(DC_WEATHER_RETRY_SEC); /* keep last-known-good cache */
    }
}

static void abort_fetch(void)
{
    if (g_weather.pid > 0)
        kill(g_weather.pid, SIGKILL);
    close(g_weather.fd);
    g_weather.fd = -1;
    g_weather.len = 0;
    g_weather.fetch_active = false;
    dc_warn("weather: fetch timed out, retrying in %ds", DC_WEATHER_RETRY_SEC);
    arm_next_attempt(DC_WEATHER_RETRY_SEC);
}

/* Drain whatever is currently readable, never blocking. */
static void drain_fetch(void)
{
    if (secs_since(&g_weather.fetch_started) > DC_WEATHER_FETCH_TIMEOUT_SEC) {
        abort_fetch();
        return;
    }

    struct pollfd pfd = {.fd = g_weather.fd, .events = POLLIN};
    if (poll(&pfd, 1, 0) <= 0)
        return; /* nothing ready yet: try again next call */

    for (;;) {
        if (g_weather.len + 1 >= sizeof(g_weather.buf)) {
            dc_warn("weather: response too large, dropping");
            finish_fetch(false);
            return;
        }
        ssize_t n = read(g_weather.fd, g_weather.buf + g_weather.len,
                          sizeof(g_weather.buf) - g_weather.len - 1);
        if (n > 0) {
            g_weather.len += (size_t)n;
            continue;
        }
        if (n == 0) { /* EOF: curl exited */
            g_weather.buf[g_weather.len] = '\0';
            finish_fetch(true);
            return;
        }
        return; /* EAGAIN/EWOULDBLOCK: wait for the next call */
    }
}

static void start_fetch(void)
{
    char url[640];
    snprintf(url, sizeof(url),
              "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
              "&current=temperature_2m,weather_code,is_day,apparent_temperature,"
              "relative_humidity_2m,wind_speed_10m,surface_pressure,precipitation_probability"
              "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
              "&hourly=temperature_2m,weather_code&forecast_hours=%d"
              "&forecast_days=7&timezone=auto&temperature_unit=%s",
              g_weather.lat, g_weather.lon, DC_WEATHER_HOURLY_REQUEST,
              g_weather.fahrenheit ? "fahrenheit" : "celsius");

    int fds[2];
    if (pipe(fds) < 0) {
        dc_warn("weather: pipe() failed");
        arm_next_attempt(DC_WEATHER_RETRY_SEC);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        dc_warn("weather: fork() failed");
        close(fds[0]);
        close(fds[1]);
        arm_next_attempt(DC_WEATHER_RETRY_SEC);
        return;
    }

    if (pid == 0) { /* child: curl -> write end of the pipe */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        execlp("curl", "curl", "-s", "--connect-timeout", "3", "--max-time", "6", url,
               (char *)NULL);
        _exit(127);
    }

    /* Parent: read end, non-blocking so dc_weather_get() never stalls. */
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);

    g_weather.fd = fds[0];
    g_weather.pid = pid;
    g_weather.len = 0;
    g_weather.fetch_active = true;
    clock_gettime(CLOCK_MONOTONIC, &g_weather.fetch_started);
}

bool dc_weather_get(dc_weather_state *out)
{
    if (!g_weather.configured) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    if (g_weather.fetch_active)
        drain_fetch();
    else if (attempt_due())
        start_fetch();

    *out = g_weather.cache;
    return g_weather.have_cache;
}

/* DMS's WeatherService.qml weatherIcons/nightWeatherIcons tables, collapsed
 * onto the icon set used by DankC's bar (see docs/12-BAR-SPEC.md §4). */
const char *dc_weather_icon_name(int wmo_code, bool is_day)
{
    switch (wmo_code) {
    case 0:
    case 1:
        return is_day ? "clear_day" : "clear_night";
    case 2:
        return is_day ? "partly_cloudy_day" : "partly_cloudy_night";
    case 3:
        return "cloud";
    case 45:
    case 48:
        return "foggy";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
    case 80:
    case 81:
    case 82:
        return "rainy";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return "weather_snowy";
    case 95:
    case 96:
    case 99:
        return "thunderstorm";
    default:
        return "cloud";
    }
}

/* WMO code -> short condition text, matching DMS WeatherService's descriptions
 * (docs/13-POPOUTS-SPEC.md sec.5). */
const char *dc_weather_condition_name(int wmo_code)
{
    switch (wmo_code) {
    case 0:
        return "Clear Sky";
    case 1:
        return "Mainly Clear";
    case 2:
        return "Partly Cloudy";
    case 3:
        return "Overcast";
    case 45:
        return "Fog";
    case 48:
        return "Rime Fog";
    case 51:
        return "Light Drizzle";
    case 53:
        return "Drizzle";
    case 55:
        return "Heavy Drizzle";
    case 56:
    case 57:
        return "Freezing Drizzle";
    case 61:
        return "Light Rain";
    case 63:
        return "Rain";
    case 65:
        return "Heavy Rain";
    case 66:
    case 67:
        return "Freezing Rain";
    case 71:
        return "Light Snow";
    case 73:
        return "Snow";
    case 75:
        return "Heavy Snow";
    case 77:
        return "Snow Grains";
    case 80:
        return "Light Showers";
    case 81:
        return "Showers";
    case 82:
        return "Heavy Showers";
    case 85:
    case 86:
        return "Snow Showers";
    case 95:
        return "Thunderstorm";
    case 96:
    case 99:
        return "Thunderstorm";
    default:
        return "Unknown";
    }
}

#ifdef DC_SERVICE_TEST
/* Standalone smoke test: `cc -DDC_SERVICE_TEST -Isrc -Ithird_party/cjson
 * src/services/weather.c src/core/log.c third_party/cjson/cJSON.c -lm -o /tmp/wtest`
 * then run it — makes a real network call. */
int main(void)
{
    signal(SIGCHLD, SIG_IGN);
    dc_log_init(DC_LOG_DEBUG);
    dc_weather_init(40.7128, -74.0060, false);

    dc_weather_state st = {0};
    for (int i = 0; i < 20; i++) {
        bool valid = dc_weather_get(&st);
        printf("[%2ds] valid=%d temp=%d code=%d is_day=%d icon=%s\n", i, valid, st.temp_c,
               st.weather_code, st.is_day, dc_weather_icon_name(st.weather_code, st.is_day));
        if (valid)
            break;
        sleep(1);
    }
    return st.valid ? 0 : 1;
}
#endif
