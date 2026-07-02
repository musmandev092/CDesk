/* weather.h — current-conditions weather via Open-Meteo (no API key needed).
 *
 * Mirrors DMS's WeatherService: a fixed lat/lon, a 15-minute refresh, and a
 * WMO weather-code -> Material Symbols icon mapping for the bar widget. See
 * docs/12-BAR-SPEC.md §4 ("weather").
 */
#ifndef DC_SERVICES_WEATHER_H
#define DC_SERVICES_WEATHER_H

#include <stdbool.h>

/* One day of the 7-day forecast (open-meteo `daily` block; docs/13-POPOUTS-
 * SPEC.md sec.5 Weather tab). Temperatures share the current unit. */
typedef struct dc_weather_daily {
    int weather_code;
    int temp_max;
    int temp_min;
    char sunrise[8]; /* "HH:MM" local time, or "" */
    char sunset[8];  /* "HH:MM" local time, or "" */
} dc_weather_daily;

typedef struct dc_weather_state {
    bool valid;        /* a successful fetch has populated the fields below */
    int temp_c;         /* rounded current temperature, in the unit requested at
                          * dc_weather_init() time (Celsius unless `fahrenheit`
                          * was true — the field keeps the `_c` name for parity
                          * with DMS's WMO-derived state, but holds Fahrenheit
                          * when Fahrenheit was requested) */
    int weather_code;   /* raw WMO weather code (open-meteo `weather_code`) */
    bool is_day;

    /* Extended current conditions for the dashboard Weather tab + Overview
     * card (docs/13-POPOUTS-SPEC.md sec.5). `have_current_extra` is false on
     * older/limited responses. `feels_like` shares temp_c's unit. */
    bool have_current_extra;
    int feels_like;    /* apparent_temperature */
    int humidity;      /* relative_humidity_2m, % */
    int wind_kmh;      /* wind_speed_10m, km/h */
    int pressure_hpa;  /* surface_pressure, hPa */
    int precip_prob;   /* precipitation_probability, % */

    /* 7-day daily forecast, [0] = today. `daily_count` is 0 when absent. */
    dc_weather_daily daily[7];
    int daily_count;
} dc_weather_state;

/* Configure the fixed location + unit and arm the first fetch. Does not block
 * or touch the network itself; dc_weather_get() drives the state machine.
 * Call once at startup. */
void dc_weather_init(double lat, double lon, bool fahrenheit);

/* Non-blocking: drains any in-flight fetch (zero-timeout poll of the curl
 * pipe), kicks off a new fetch if the refresh interval has elapsed, and
 * copies the last-known-good reading into `out`. Safe to call every render
 * frame — never forks more than one fetch at a time and never blocks.
 * Returns true if `out` holds a valid (ever-successful) reading. */
bool dc_weather_get(dc_weather_state *out);

/* Map a WMO weather code + day/night flag to a Material Symbols icon name
 * (e.g. "clear_day", "rainy"). Returned pointer is a static string literal —
 * never NULL, never owned by the caller. */
const char *dc_weather_icon_name(int wmo_code, bool is_day);

/* Human-readable condition text for a WMO code (e.g. "Clear Sky", "Rain").
 * Static string literal, never NULL. (docs/13-POPOUTS-SPEC.md sec.5.) */
const char *dc_weather_condition_name(int wmo_code);

#endif /* DC_SERVICES_WEATHER_H */
