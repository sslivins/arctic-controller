/*
 * Arctic Heat Pump Controller
 * Location Manager (implementation).
 */
#include "location_manager.h"
#include "iana_tz.h"
#include "time_manager.h"

#include <string.h>
#include <nvs.h>
#include <esp_log.h>

static const char* TAG = "location_mgr";

#define NVS_NAMESPACE   "loc_cfg"
#define NVS_KEY_LAT     "lat"
#define NVS_KEY_LON     "lon"
#define NVS_KEY_NAME    "name"
#define NVS_KEY_IANA    "iana"
#define NVS_KEY_VALID   "valid"
#define NVS_KEY_TZAUTO  "tz_auto"

// Default location: Sun Peaks, BC (the primary deployment site). Ensures weather
// and automatic timezone work out-of-the-box before the user picks a location.
#define DEFAULT_LAT   50.8762
#define DEFAULT_LON  -119.91075
#define DEFAULT_NAME "Sun Peaks, British Columbia, CA"
#define DEFAULT_IANA "America/Vancouver"

static location_t s_loc = {
    .valid = false,
    .latitude = DEFAULT_LAT,
    .longitude = DEFAULT_LON,
    .name = DEFAULT_NAME,
    .iana_tz = DEFAULT_IANA,
};
static bool s_tz_auto = true;

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
static void load_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No stored location; using default (%s)", DEFAULT_NAME);
        return;
    }

    uint8_t tz_auto = 1;
    if (nvs_get_u8(nvs, NVS_KEY_TZAUTO, &tz_auto) == ESP_OK) {
        s_tz_auto = (tz_auto != 0);
    }

    uint8_t valid = 0;
    if (nvs_get_u8(nvs, NVS_KEY_VALID, &valid) == ESP_OK && valid) {
        // NVS has no double type; latitude/longitude are stored as blobs.
        size_t sz = sizeof(double);
        double lat = 0, lon = 0;
        bool ok = (nvs_get_blob(nvs, NVS_KEY_LAT, &lat, &sz) == ESP_OK);
        sz = sizeof(double);
        ok = ok && (nvs_get_blob(nvs, NVS_KEY_LON, &lon, &sz) == ESP_OK);
        if (ok) {
            s_loc.valid = true;
            s_loc.latitude = lat;
            s_loc.longitude = lon;
            size_t len = sizeof(s_loc.name);
            if (nvs_get_str(nvs, NVS_KEY_NAME, s_loc.name, &len) != ESP_OK) {
                s_loc.name[0] = '\0';
            }
            len = sizeof(s_loc.iana_tz);
            if (nvs_get_str(nvs, NVS_KEY_IANA, s_loc.iana_tz, &len) != ESP_OK) {
                s_loc.iana_tz[0] = '\0';
            }
            ESP_LOGI(TAG, "Loaded location: %s (%.4f, %.4f) tz=%s",
                     s_loc.name, s_loc.latitude, s_loc.longitude, s_loc.iana_tz);
        }
    }
    nvs_close(nvs);
}

static void save_to_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return;
    }
    nvs_set_u8(nvs, NVS_KEY_TZAUTO, s_tz_auto ? 1 : 0);
    nvs_set_u8(nvs, NVS_KEY_VALID, s_loc.valid ? 1 : 0);
    nvs_set_blob(nvs, NVS_KEY_LAT, &s_loc.latitude, sizeof(double));
    nvs_set_blob(nvs, NVS_KEY_LON, &s_loc.longitude, sizeof(double));
    nvs_set_str(nvs, NVS_KEY_NAME, s_loc.name);
    nvs_set_str(nvs, NVS_KEY_IANA, s_loc.iana_tz);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// ---------------------------------------------------------------------------
// Timezone derivation
// ---------------------------------------------------------------------------
static void apply_auto_timezone(void)
{
    if (!s_tz_auto) {
        return;
    }
    const char* posix = iana_to_posix(s_loc.iana_tz);
    if (posix) {
        ESP_LOGI(TAG, "Auto timezone: %s -> %s", s_loc.iana_tz, posix);
        time_mgr_set_timezone(posix);
    } else {
        ESP_LOGW(TAG, "No POSIX mapping for IANA zone '%s'; leaving timezone unchanged",
                 s_loc.iana_tz);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void location_mgr_init(void)
{
    load_from_nvs();
    apply_auto_timezone();
}

const location_t* location_mgr_get(void)
{
    return &s_loc;
}

void location_mgr_set(double lat, double lon, const char* name, const char* iana_tz)
{
    s_loc.valid = true;
    s_loc.latitude = lat;
    s_loc.longitude = lon;
    if (name) {
        strncpy(s_loc.name, name, sizeof(s_loc.name) - 1);
        s_loc.name[sizeof(s_loc.name) - 1] = '\0';
    }
    if (iana_tz) {
        strncpy(s_loc.iana_tz, iana_tz, sizeof(s_loc.iana_tz) - 1);
        s_loc.iana_tz[sizeof(s_loc.iana_tz) - 1] = '\0';
    } else {
        s_loc.iana_tz[0] = '\0';
    }
    ESP_LOGI(TAG, "Location set: %s (%.4f, %.4f) tz=%s",
             s_loc.name, s_loc.latitude, s_loc.longitude, s_loc.iana_tz);
    save_to_nvs();
    apply_auto_timezone();
}

bool location_mgr_get_tz_auto(void)
{
    return s_tz_auto;
}

void location_mgr_set_tz_auto(bool automatic)
{
    s_tz_auto = automatic;
    save_to_nvs();
    if (automatic) {
        apply_auto_timezone();
    }
}

const char* location_mgr_derived_posix(void)
{
    if (s_loc.iana_tz[0] == '\0') {
        return NULL;
    }
    return iana_to_posix(s_loc.iana_tz);
}
