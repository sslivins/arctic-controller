/*
 * Arctic Heat Pump Controller
 * Open-Meteo geocoding client.
 *
 * Resolves a free-text place query (e.g. "sun peaks") to candidate locations via
 * the free, no-key Open-Meteo geocoding API:
 *   https://geocoding-api.open-meteo.com/v1/search?name=<q>&count=<n>
 *
 * The blocking HTTPS request MUST NOT run on the LVGL/UI task; call
 * geocoding_search() from a worker task and marshal results back to the UI.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   name[64];         // e.g. "Sun Peaks"
    char   admin1[64];       // e.g. "British Columbia" (may be empty)
    char   country[48];      // e.g. "Canada" (may be empty)
    char   country_code[4];  // e.g. "CA" (may be empty)
    char   timezone[48];     // IANA zone, e.g. "America/Vancouver" (may be empty)
    double latitude;
    double longitude;
} geo_result_t;

/**
 * @brief Search Open-Meteo geocoding for a place name (blocking network call).
 * @param query     Free-text place name (URL-encoded internally).
 * @param out       Caller-provided array to fill.
 * @param max_results Capacity of @p out.
 * @return Number of results written (0..max_results), or -1 on network/HTTP error.
 */
int geocoding_search(const char* query, geo_result_t* out, int max_results);

/**
 * @brief Parse an Open-Meteo geocoding JSON response body into results.
 *        Pure function (no network); exposed for host-side unit testing.
 * @return Number of results written (0..max_results), or -1 if JSON is invalid.
 */
int geocoding_parse(const char* json, geo_result_t* out, int max_results);

/**
 * @brief Build a human-readable label: "Sun Peaks, British Columbia, CA".
 *        Omits empty components.
 */
void geocoding_format_label(const geo_result_t* r, char* buf, size_t buf_len);

#ifdef CONFIG_TEST_ENDPOINTS
/**
 * @brief Test-only: install a canned Open-Meteo response so geocoding_search()
 *        returns deterministic results without touching the network.
 * @param json  An Open-Meteo-shaped body, e.g. {"results":[{...}]}. A body
 *              containing "__error__" makes geocoding_search() return -1
 *              (simulated network/HTTP failure). Pass NULL to clear.
 *        Compiled only into test-enabled builds.
 */
void geocoding_set_mock_json(const char* json);
void geocoding_clear_mock(void);
#endif

#ifdef __cplusplus
}
#endif
