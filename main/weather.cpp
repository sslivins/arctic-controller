/*
 * Arctic Heat Pump Controller
 * Open-Meteo current-weather client + status-bar weather service (impl).
 */
#include "sdkconfig.h"
#include "weather.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include <lvgl.h>
#include "status_bar.h"
#include "location_manager.h"
#include "wifi_manager.h"

static const char* TAG = "weather";

#define WX_HOST "https://api.open-meteo.com/v1/forecast"
#define WX_TIMEOUT_MS 12000
#define WX_REFRESH_INTERVAL_MS (15 * 60 * 1000)  // 15 minutes

// UTF-8 glyphs from the weather_icons_32 font (FontAwesome subset).
#define WX_ICON_SUN            "\xEF\x86\x85"  // U+F185 sun
#define WX_ICON_CLOUD_SUN      "\xEF\x9B\x84"  // U+F6C4 cloud-sun
#define WX_ICON_CLOUD          "\xEF\x83\x82"  // U+F0C2 cloud
#define WX_ICON_FOG            "\xEF\x9D\x9F"  // U+F75F smog
#define WX_ICON_RAIN           "\xEF\x9C\xBD"  // U+F73D cloud-rain
#define WX_ICON_HEAVY_RAIN     "\xEF\x9D\x80"  // U+F740 cloud-showers-heavy
#define WX_ICON_SNOW           "\xEF\x8B\x9C"  // U+F2DC snowflake
#define WX_ICON_THUNDER        "\xEF\x83\xA7"  // U+F0E7 bolt

#ifdef CONFIG_TEST_ENDPOINTS
// Test-only canned response. When set, weather_fetch() parses this instead of
// performing a network request, making the weather display deterministic in CI.
static char* s_mock_json = NULL;

void weather_set_mock_json(const char* json)
{
    if (s_mock_json) {
        free(s_mock_json);
        s_mock_json = NULL;
    }
    if (json) {
        s_mock_json = strdup(json);
    }
}

void weather_clear_mock(void)
{
    if (s_mock_json) {
        free(s_mock_json);
        s_mock_json = NULL;
    }
}
#endif

// ---------------------------------------------------------------------------
// Response accumulation (single in-flight request; weather runs on one
// dedicated worker task at a time).
// ---------------------------------------------------------------------------
static char*  s_resp_buf = NULL;
static size_t s_resp_len = 0;
static size_t s_resp_cap = 0;

static void resp_reset(void)
{
    if (s_resp_buf) {
        free(s_resp_buf);
        s_resp_buf = NULL;
    }
    s_resp_len = 0;
    s_resp_cap = 0;
}

static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_resp_buf == NULL) {
            s_resp_cap = 2048;
            s_resp_buf = (char*)malloc(s_resp_cap);
            s_resp_len = 0;
        }
        if (s_resp_buf && s_resp_len + evt->data_len >= s_resp_cap) {
            s_resp_cap = s_resp_len + evt->data_len + 1024;
            s_resp_buf = (char*)realloc(s_resp_buf, s_resp_cap);
        }
        if (s_resp_buf) {
            memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
            s_resp_len += evt->data_len;
            s_resp_buf[s_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// WMO weather-code mapping
// (https://open-meteo.com/en/docs — "Weather variable documentation").
// ---------------------------------------------------------------------------
const char* weather_code_icon(int code)
{
    switch (code) {
        case 0:                 return WX_ICON_SUN;         // Clear sky
        case 1: case 2:         return WX_ICON_CLOUD_SUN;   // Mainly/partly clear
        case 3:                 return WX_ICON_CLOUD;       // Overcast
        case 45: case 48:       return WX_ICON_FOG;         // Fog
        case 51: case 53: case 55:                          // Drizzle
        case 56: case 57:                                   // Freezing drizzle
        case 61: case 63:       return WX_ICON_RAIN;        // Rain (slight/moderate)
        case 65: case 66: case 67:                          // Heavy / freezing rain
                                return WX_ICON_HEAVY_RAIN;
        case 71: case 73: case 75: case 77:                 // Snow fall / grains
                                return WX_ICON_SNOW;
        case 80: case 81:       return WX_ICON_RAIN;        // Rain showers
        case 82:                return WX_ICON_HEAVY_RAIN;  // Violent rain showers
        case 85: case 86:       return WX_ICON_SNOW;        // Snow showers
        case 95: case 96: case 99:                          // Thunderstorm
                                return WX_ICON_THUNDER;
        default:                return WX_ICON_CLOUD;
    }
}

const char* weather_code_desc(int code)
{
    switch (code) {
        case 0:                 return "Clear";
        case 1:                 return "Mainly clear";
        case 2:                 return "Partly cloudy";
        case 3:                 return "Overcast";
        case 45: case 48:       return "Fog";
        case 51: case 53: case 55: return "Drizzle";
        case 56: case 57:       return "Freezing drizzle";
        case 61: case 63:       return "Rain";
        case 65:                return "Heavy rain";
        case 66: case 67:       return "Freezing rain";
        case 71: case 73: case 75: return "Snow";
        case 77:                return "Snow grains";
        case 80: case 81:       return "Rain showers";
        case 82:                return "Heavy showers";
        case 85: case 86:       return "Snow showers";
        case 95:                return "Thunderstorm";
        case 96: case 99:       return "Thunderstorm";
        default:                return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Parse + fetch
// ---------------------------------------------------------------------------
int weather_parse(const char* json, weather_data_t* out)
{
    if (!json || !out) {
        return -1;
    }
    cJSON* root = cJSON_Parse(json);
    if (root == NULL) {
        return -1;
    }
    int rc = -1;
    const cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (current && cJSON_IsObject(current)) {
        const cJSON* temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
        const cJSON* wcode = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
        if (temp && cJSON_IsNumber(temp)) {
            out->temp_c = (float)temp->valuedouble;
            out->weather_code = (wcode && cJSON_IsNumber(wcode)) ? wcode->valueint : 0;
            out->valid = true;
            rc = 0;
        }
    }
    cJSON_Delete(root);
    return rc;
}

int weather_fetch(double lat, double lon, weather_data_t* out)
{
    if (!out) {
        return -1;
    }

#ifdef CONFIG_TEST_ENDPOINTS
    if (s_mock_json) {
        if (strstr(s_mock_json, "__error__") != NULL) {
            ESP_LOGW(TAG, "Weather MOCK error");
            return -1;
        }
        ESP_LOGI(TAG, "Weather MOCK active");
        return weather_parse(s_mock_json, out);
    }
#endif

    char url[256];
    // Always request Celsius; the UI converts to the user's unit at render time.
    snprintf(url, sizeof(url),
             WX_HOST "?latitude=%.5f&longitude=%.5f"
                     "&current=temperature_2m,weather_code&temperature_unit=celsius",
             lat, lon);

    resp_reset();

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.timeout_ms = WX_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        resp_reset();
        return -1;
    }
    esp_http_client_set_header(client, "User-Agent", "arctic-controller");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || s_resp_buf == NULL) {
        ESP_LOGE(TAG, "Weather request failed: %s (HTTP %d)", esp_err_to_name(err), status);
        resp_reset();
        return -1;
    }

    ESP_LOGI(TAG, "Weather -> %d bytes", (int)s_resp_len);
    int rc = weather_parse(s_resp_buf, out);
    resp_reset();
    if (rc < 0) {
        ESP_LOGE(TAG, "Failed to parse weather response");
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Status-bar weather service
//
// A single, long-lived worker task performs the blocking HTTPS fetch. It is
// created ONCE at init with its stack in PSRAM and then parked on a task
// notification. Two reasons for this design:
//
//  1. Persistent (not per-fetch): creating a fresh task per refresh means
//     allocating and freeing a ~12 KB FreeRTOS stack on every fetch, adding
//     needless churn.
//  2. PSRAM stack (MALLOC_CAP_SPIRAM): FreeRTOS task stacks default to
//     internal RAM, which on this board is scarce and shared with the on-
//     device HTTPS servers' per-session TLS bookkeeping and httpd task
//     stacks. A resident 12 KB internal-RAM weather stack was enough to (a)
//     fragment the internal heap so late-run TLS handshakes failed with
//     SSL_ALLOC_FAILED (-0x7780), and (b) starve the 4th httpd server of a
//     task stack on a post-reboot restart (ESP_ERR_HTTPD_TASK), taking the
//     whole API server down. Placing the stack in the 27 MB PSRAM removes all
//     internal-RAM pressure. The worker only does networking + heap work
//     (never flash/NVS writes), so running on a PSRAM stack is safe.
// ---------------------------------------------------------------------------
static volatile bool s_refresh_busy = false;
static weather_data_t s_worker_result;   // written by worker, read on UI task
static TaskHandle_t   s_worker_task = NULL;
static double         s_pending_lat = 0.0;
static double         s_pending_lon = 0.0;

// Runs on the LVGL/UI task via lv_async_call: push the fetched weather to the
// status bar (or leave it unchanged on failure).
static void apply_result_cb(void* arg)
{
    (void)arg;
    if (s_worker_result.valid) {
        status_bar_set_weather(true, s_worker_result.temp_c, s_worker_result.weather_code);
    }
    s_refresh_busy = false;
}

// Long-lived worker: park on a notification, fetch when signalled, marshal the
// result back to the LVGL task, then park again. Never touch LVGL here.
static void weather_worker(void* arg)
{
    (void)arg;
    for (;;) {
        // Block until weather_service_refresh() signals a fetch is wanted.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        double lat = s_pending_lat;
        double lon = s_pending_lon;

        weather_data_t data = {};
        if (weather_fetch(lat, lon, &data) == 0) {
            s_worker_result = data;
        } else {
            s_worker_result.valid = false;
        }
        lv_async_call(apply_result_cb, NULL);
    }
}

void weather_service_refresh(void)
{
    if (s_refresh_busy) {
        return;  // a fetch is already in flight
    }
    if (s_worker_task == NULL) {
        return;  // service not initialised yet
    }
    if (wifi_mgr_get_state() != WIFI_MGR_STATE_CONNECTED) {
        return;  // no network
    }
    const location_t* loc = location_mgr_get();
    if (!loc || !loc->valid) {
        return;  // no location to query
    }

    s_pending_lat = loc->latitude;
    s_pending_lon = loc->longitude;

    s_refresh_busy = true;
    xTaskNotifyGive(s_worker_task);
}

static void refresh_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    weather_service_refresh();
}

void weather_service_init(void)
{
    // Create the persistent worker ONCE, with its stack in PSRAM so it never
    // competes with the HTTPS servers for internal RAM. Priority 4 keeps it
    // below the LVGL render task and the HTTPS server (both priority 5) so a
    // background weather fetch never preempts the UI or the web server.
    if (s_worker_task == NULL) {
        if (xTaskCreateWithCaps(weather_worker, "weather", 12288, NULL, 4,
                                &s_worker_task,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGW(TAG, "Failed to create weather worker task");
            s_worker_task = NULL;
        }
    }

    // Periodic refresh; the first fetch is also triggered on WiFi connect.
    lv_timer_create(refresh_timer_cb, WX_REFRESH_INTERVAL_MS, NULL);
    ESP_LOGI(TAG, "Weather service started (refresh every %d min)",
             WX_REFRESH_INTERVAL_MS / 60000);
}
