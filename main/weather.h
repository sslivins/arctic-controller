/*
 * Arctic Heat Pump Controller
 * Open-Meteo current-weather client + status-bar weather service.
 *
 * Fetches the current outside temperature and a WMO weather code for the
 * device's configured location via the free, no-key Open-Meteo forecast API:
 *   https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..
 *          &current=temperature_2m,weather_code&temperature_unit=celsius
 *
 * Temperature is always fetched and cached in Celsius; the status bar converts
 * to the user-selected unit at render time, so toggling °C/°F updates the
 * display instantly without a refetch.
 *
 * The blocking HTTPS request MUST NOT run on the LVGL/UI task; the service
 * spawns a dedicated worker task and marshals the result back with
 * lv_async_call().
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool  valid;         // true once a fetch has succeeded
    float temp_c;        // current temperature, degrees Celsius
    int   weather_code;  // WMO weather interpretation code
} weather_data_t;

/**
 * @brief Fetch the current weather for a location (blocking network call).
 * @param lat,lon  Coordinates.
 * @param out      Caller-provided struct to fill (valid=true on success).
 * @return 0 on success, -1 on network/HTTP/parse error.
 */
int weather_fetch(double lat, double lon, weather_data_t* out);

/**
 * @brief Parse an Open-Meteo forecast JSON body into a weather_data_t.
 *        Pure function (no network); exposed for host-side unit testing.
 * @return 0 on success, -1 if the JSON is invalid or missing fields.
 */
int weather_parse(const char* json, weather_data_t* out);

/**
 * @brief UTF-8 icon glyph (from the weather_icons_32 font) for a WMO code.
 *        Never NULL — falls back to a neutral cloud for unknown codes.
 */
const char* weather_code_icon(int weather_code);

/**
 * @brief Short human-readable description for a WMO code (e.g. "Snow").
 *        Never NULL. Used for logs / accessibility.
 */
const char* weather_code_desc(int weather_code);

// ---------------------------------------------------------------------------
// Status-bar weather service (runs on the UI/LVGL task + a worker task).
// ---------------------------------------------------------------------------

/**
 * @brief Start the periodic weather-refresh timer.
 *        Call once at startup AFTER the status bar and location manager exist.
 */
void weather_service_init(void);

/**
 * @brief Trigger a weather refresh now (non-blocking).
 *
 * Spawns a worker task that fetches the current weather for the device's
 * location and updates the status bar. No-op when WiFi is down, no location is
 * set, or a refresh is already in flight. Safe to call from the UI task.
 */
void weather_service_refresh(void);

#ifdef CONFIG_TEST_ENDPOINTS
/**
 * @brief Test-only: install a canned Open-Meteo forecast body so weather_fetch()
 *        returns deterministic data without touching the network.
 * @param json  An Open-Meteo-shaped body, e.g.
 *              {"current":{"temperature_2m":-5.0,"weather_code":71}}. A body
 *              containing "__error__" makes weather_fetch() return -1. Pass NULL
 *              to clear. Compiled only into test-enabled builds.
 */
void weather_set_mock_json(const char* json);
void weather_clear_mock(void);
#endif

#ifdef __cplusplus
}
#endif
