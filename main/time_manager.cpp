/*
 * Arctic Heat Pump Controller
 * Time Manager Implementation - NTP time synchronization
 */
#include "time_manager.h"
#include "event_log.h"
#include <string.h>
#include <sys/time.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char* TAG = "time_mgr";

// NVS namespace and keys
#define NVS_NAMESPACE "time_cfg"
#define NVS_KEY_TZ    "timezone"
#define NVS_KEY_24H   "format_24h"

// Default NTP servers
#define NTP_SERVER_PRIMARY   "pool.ntp.org"
#define NTP_SERVER_SECONDARY "time.google.com"
#define NTP_SERVER_TERTIARY  "time.cloudflare.com"

// Default timezone - US Eastern (EST5EDT with DST rules)
// Format: STDoffsetDST,start,end
// EST5EDT = Eastern Standard Time, 5 hours behind UTC, Eastern Daylight Time
// M3.2.0 = DST starts March, 2nd week, Sunday
// M11.1.0 = DST ends November, 1st week, Sunday
#define DEFAULT_TIMEZONE "EST5EDT,M3.2.0,M11.1.0"

// Internal state
static struct {
    bool initialized;
    bool sntp_configured;
    bool synced;
    bool format_24h;
    char timezone[64];
} time_state = {
    .initialized = false,
    .sntp_configured = false,
    .synced = false,
    .format_24h = true,  // Default to 24-hour format
    .timezone = DEFAULT_TIMEZONE,
};

// Callback when time is synchronized
static void time_sync_notification_cb(struct timeval* tv)
{
    ESP_LOGI(TAG, "NTP time synchronized!");
    time_state.synced = true;
    event_log_time_synced(tv);
    
    // Log the current time
    char time_str[64];
    if (time_mgr_get_time_str(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S")) {
        ESP_LOGI(TAG, "Current time: %s", time_str);
    }
}

// Configure SNTP (called lazily when network is ready)
static void configure_sntp(void)
{
    if (time_state.sntp_configured) {
        return;
    }
    
    ESP_LOGI(TAG, "Configuring SNTP...");
    
    // Configure SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_PRIMARY);
    esp_sntp_setservername(1, NTP_SERVER_SECONDARY);
    esp_sntp_setservername(2, NTP_SERVER_TERTIARY);
    
    // Set sync notification callback
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    
    // Set sync interval (default is 1 hour)
    esp_sntp_set_sync_interval(3600000);  // 1 hour in ms
    
    time_state.sntp_configured = true;
}

// Load timezone from NVS
static void load_timezone_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    ESP_LOGI(TAG, "Opening NVS namespace '%s' for timezone: err=%d", NVS_NAMESPACE, err);
    if (err == ESP_OK) {
        size_t len = sizeof(time_state.timezone);
        err = nvs_get_str(nvs, NVS_KEY_TZ, time_state.timezone, &len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded timezone from NVS: '%s' (len=%d)", time_state.timezone, len);
        } else {
            ESP_LOGW(TAG, "nvs_get_str failed (err=%d), using default: %s", err, DEFAULT_TIMEZONE);
            strncpy(time_state.timezone, DEFAULT_TIMEZONE, sizeof(time_state.timezone) - 1);
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "NVS open failed (err=%d), using default timezone: %s", err, DEFAULT_TIMEZONE);
        strncpy(time_state.timezone, DEFAULT_TIMEZONE, sizeof(time_state.timezone) - 1);
    }
}

// Save timezone to NVS
static void save_timezone_to_nvs(const char* tz_str)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    ESP_LOGI(TAG, "Saving timezone to NVS: '%s', open err=%d", tz_str, err);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_TZ, tz_str);
        ESP_LOGI(TAG, "nvs_set_str result: %d", err);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
            ESP_LOGI(TAG, "nvs_commit result: %d", err);
        }
        nvs_close(nvs);
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for writing!");
    }
}

void time_mgr_init(void)
{
    if (time_state.initialized) {
        ESP_LOGW(TAG, "Time manager already initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Initializing time manager...");
    
    // Load saved timezone from NVS (or use default)
    load_timezone_from_nvs();
    
    // Load 24h format preference from NVS
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t format_val = 1;  // Default to 24h
        if (nvs_get_u8(nvs, NVS_KEY_24H, &format_val) == ESP_OK) {
            time_state.format_24h = (format_val != 0);
        }
        nvs_close(nvs);
    }
    
    // Set timezone
    setenv("TZ", time_state.timezone, 1);
    tzset();
    
    ESP_LOGI(TAG, "Timezone: %s, Format: %s", time_state.timezone, 
             time_state.format_24h ? "24-hour" : "12-hour");
    
    // Note: SNTP configuration is deferred until start_sync() is called
    // because it requires the TCP/IP stack to be initialized first
    
    time_state.initialized = true;
    ESP_LOGI(TAG, "Time manager initialized (NTP will start when WiFi connects)");
}

void time_mgr_start_sync(void)
{
    if (!time_state.initialized) {
        ESP_LOGW(TAG, "Time manager not initialized");
        return;
    }
    
    // Configure SNTP if not done yet (requires TCP/IP stack to be ready)
    configure_sntp();
    
    // Check if SNTP is already running
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "NTP already running, restarting...");
        esp_sntp_stop();
    }
    
    ESP_LOGI(TAG, "Starting NTP synchronization...");
    ESP_LOGI(TAG, "  Primary:   %s", NTP_SERVER_PRIMARY);
    ESP_LOGI(TAG, "  Secondary: %s", NTP_SERVER_SECONDARY);
    ESP_LOGI(TAG, "  Tertiary:  %s", NTP_SERVER_TERTIARY);
    
    esp_sntp_init();
}

void time_mgr_stop_sync(void)
{
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "Stopping NTP synchronization");
        esp_sntp_stop();
    }
}

bool time_mgr_is_synced(void)
{
    return time_state.synced;
}

bool time_mgr_get_time_str(char* buf, size_t buf_len, const char* format)
{
    if (!buf || buf_len == 0 || !format) {
        return false;
    }
    
    struct tm tm_info;
    time_t now;
    time(&now);
    localtime_r(&now, &tm_info);
    
    if (strftime(buf, buf_len, format, &tm_info) == 0) {
        return false;
    }
    
    return true;
}

bool time_mgr_get_local_time(struct tm* tm_info)
{
    if (!tm_info) {
        return false;
    }
    
    time_t now;
    time(&now);
    
    // Always return the time, even if not synced
    // The caller can check time_mgr_is_synced() if they need to know if it's accurate
    localtime_r(&now, tm_info);
    
    // Return true if time appears valid (after year 2020), false otherwise
    // But we still populate tm_info either way
    return (now >= 1577836800);  // Jan 1, 2020
}

void time_mgr_set_timezone(const char* tz_str)
{
    if (!tz_str) {
        return;
    }
    
    strncpy(time_state.timezone, tz_str, sizeof(time_state.timezone) - 1);
    time_state.timezone[sizeof(time_state.timezone) - 1] = '\0';
    
    setenv("TZ", time_state.timezone, 1);
    tzset();
    
    // Save to NVS for persistence
    save_timezone_to_nvs(tz_str);
    
    ESP_LOGI(TAG, "Timezone set to: %s", time_state.timezone);
}

const char* time_mgr_get_timezone(void)
{
    return time_state.timezone;
}

void time_mgr_force_sync(void)
{
    ESP_LOGI(TAG, "Forcing NTP sync...");
    time_mgr_stop_sync();
    time_mgr_start_sync();
}

void time_mgr_set_24h_format(bool use_24h)
{
    time_state.format_24h = use_24h;
    
    // Save to NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_24H, use_24h ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    
    ESP_LOGI(TAG, "Time format set to %s", use_24h ? "24-hour" : "12-hour");
}

bool time_mgr_get_24h_format(void)
{
    return time_state.format_24h;
}
