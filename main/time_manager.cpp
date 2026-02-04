/*
 * Arctic Heat Pump Controller
 * Time Manager Implementation - NTP time synchronization
 */
#include "time_manager.h"
#include <string.h>
#include <sys/time.h>
#include <esp_log.h>
#include <esp_sntp.h>

static const char* TAG = "time_mgr";

// Default NTP servers
#define NTP_SERVER_PRIMARY   "pool.ntp.org"
#define NTP_SERVER_SECONDARY "time.google.com"
#define NTP_SERVER_TERTIARY  "time.cloudflare.com"

// Default timezone (UTC)
#define DEFAULT_TIMEZONE "UTC0"

// Internal state
static struct {
    bool initialized;
    bool sntp_configured;
    bool synced;
    char timezone[64];
} time_state = {
    .initialized = false,
    .sntp_configured = false,
    .synced = false,
    .timezone = DEFAULT_TIMEZONE,
};

// Callback when time is synchronized
static void time_sync_notification_cb(struct timeval* tv)
{
    ESP_LOGI(TAG, "NTP time synchronized!");
    time_state.synced = true;
    
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

void time_mgr_init(void)
{
    if (time_state.initialized) {
        ESP_LOGW(TAG, "Time manager already initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Initializing time manager...");
    
    // Set timezone
    setenv("TZ", time_state.timezone, 1);
    tzset();
    
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
    if (!time_mgr_get_local_time(&tm_info)) {
        return false;
    }
    
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
    
    // Check if time is valid (after year 2020)
    if (now < 1577836800) {  // Jan 1, 2020
        return false;
    }
    
    localtime_r(&now, tm_info);
    return true;
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
    
    ESP_LOGI(TAG, "Timezone set to: %s", time_state.timezone);
}
