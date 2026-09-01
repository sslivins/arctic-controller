/*
 * Arctic Heat Pump Controller
 * Open-Meteo geocoding client (implementation).
 */
#include "sdkconfig.h"
#include "geocoding.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>

static const char* TAG = "geocoding";

#define GEO_HOST "https://geocoding-api.open-meteo.com/v1/search"
#define GEO_TIMEOUT_MS 12000

#ifdef CONFIG_TEST_ENDPOINTS
// Test-only canned response. When set, geocoding_search() parses this instead
// of performing a network request, making the location-search UI deterministic
// in CI. See geocoding_set_mock_json().
static char* s_mock_json = NULL;

void geocoding_set_mock_json(const char* json)
{
    if (s_mock_json) {
        free(s_mock_json);
        s_mock_json = NULL;
    }
    if (json) {
        s_mock_json = strdup(json);
    }
}

void geocoding_clear_mock(void)
{
    if (s_mock_json) {
        free(s_mock_json);
        s_mock_json = NULL;
    }
}
#endif

// ---------------------------------------------------------------------------
// Response accumulation (single in-flight request; geocoding runs on one
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
            s_resp_cap = 4096;
            s_resp_buf = (char*)malloc(s_resp_cap);
            s_resp_len = 0;
        }
        if (s_resp_buf && s_resp_len + evt->data_len >= s_resp_cap) {
            s_resp_cap = s_resp_len + evt->data_len + 2048;
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
// Helpers
// ---------------------------------------------------------------------------
static void copy_str_field(const cJSON* obj, const char* key, char* dst, size_t dst_len)
{
    dst[0] = '\0';
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
}

// Percent-encode a query string into dst. Unreserved chars pass through; spaces
// and everything else are %-encoded. dst_len must be >= 3*strlen(query)+1 worst case.
static void url_encode(const char* src, char* dst, size_t dst_len)
{
    static const char* hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 4 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[(c >> 4) & 0xF];
            dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = '\0';
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
int geocoding_parse(const char* json, geo_result_t* out, int max_results)
{
    if (!json || !out || max_results <= 0) {
        return -1;
    }
    cJSON* root = cJSON_Parse(json);
    if (root == NULL) {
        return -1;
    }
    int written = 0;
    const cJSON* results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (results && cJSON_IsArray(results)) {
        const cJSON* item = NULL;
        cJSON_ArrayForEach(item, results) {
            if (written >= max_results) {
                break;
            }
            geo_result_t* r = &out[written];
            memset(r, 0, sizeof(*r));
            copy_str_field(item, "name",         r->name,         sizeof(r->name));
            copy_str_field(item, "admin1",       r->admin1,       sizeof(r->admin1));
            copy_str_field(item, "country",      r->country,      sizeof(r->country));
            copy_str_field(item, "country_code", r->country_code, sizeof(r->country_code));
            copy_str_field(item, "timezone",     r->timezone,     sizeof(r->timezone));

            const cJSON* lat = cJSON_GetObjectItemCaseSensitive(item, "latitude");
            const cJSON* lon = cJSON_GetObjectItemCaseSensitive(item, "longitude");
            if (lat && cJSON_IsNumber(lat)) r->latitude = lat->valuedouble;
            if (lon && cJSON_IsNumber(lon)) r->longitude = lon->valuedouble;

            // A result without a usable name is not selectable; skip it.
            if (r->name[0] != '\0') {
                written++;
            }
        }
    }
    cJSON_Delete(root);
    return written;
}

int geocoding_search(const char* query, geo_result_t* out, int max_results)
{
    if (!query || query[0] == '\0' || !out || max_results <= 0) {
        return -1;
    }

#ifdef CONFIG_TEST_ENDPOINTS
    if (s_mock_json) {
        // A body flagged with "__error__" simulates a network/HTTP failure.
        if (strstr(s_mock_json, "__error__") != NULL) {
            ESP_LOGW(TAG, "Geocoding MOCK error for '%s'", query);
            return -1;
        }
        ESP_LOGI(TAG, "Geocoding MOCK active for '%s'", query);
        return geocoding_parse(s_mock_json, out, max_results);
    }
#endif

    char encoded[192];
    url_encode(query, encoded, sizeof(encoded));

    char url[320];
    snprintf(url, sizeof(url),
             GEO_HOST "?name=%s&count=%d&language=en&format=json",
             encoded, max_results);

    resp_reset();

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.timeout_ms = GEO_TIMEOUT_MS;
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
        ESP_LOGE(TAG, "Geocoding request failed: %s (HTTP %d)", esp_err_to_name(err), status);
        resp_reset();
        return -1;
    }

    ESP_LOGI(TAG, "Geocoding '%s' -> %d bytes", query, (int)s_resp_len);
    int n = geocoding_parse(s_resp_buf, out, max_results);
    resp_reset();
    if (n < 0) {
        ESP_LOGE(TAG, "Failed to parse geocoding response");
    }
    return n;
}

void geocoding_format_label(const geo_result_t* r, char* buf, size_t buf_len)
{
    if (!r || !buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    size_t o = 0;
    o += snprintf(buf + o, buf_len - o, "%s", r->name);
    if (r->admin1[0] != '\0' && o < buf_len) {
        o += snprintf(buf + o, buf_len - o, ", %s", r->admin1);
    }
    // Prefer the short country code when present, else the full country name.
    if (r->country_code[0] != '\0' && o < buf_len) {
        snprintf(buf + o, buf_len - o, ", %s", r->country_code);
    } else if (r->country[0] != '\0' && o < buf_len) {
        snprintf(buf + o, buf_len - o, ", %s", r->country);
    }
}
