/*
 * Arctic Heat Pump Controller
 * OTA Update Manager Implementation
 */
#include "ota_manager.h"
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_app_format.h>
#include <esp_system.h>
#include <esp_crt_bundle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "ota_manager";

// OTA task stack size
#define OTA_TASK_STACK_SIZE 8192

// Current OTA status
static ota_status_t ota_status = {
    .state = OTA_STATE_IDLE,
    .progress_percent = 0,
    .bytes_downloaded = 0,
    .total_bytes = 0,
    .error_msg = "",
    .current_version = "",
    .new_version = ""
};

// URL for current update
static char update_url[256] = "";

// Mutex for status access
static SemaphoreHandle_t status_mutex = NULL;

// Forward declarations
static void ota_task(void* pvParameter);

bool ota_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager...");
    
    // Create mutex
    status_mutex = xSemaphoreCreateMutex();
    if (status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }
    
    // Get current app version
    const esp_app_desc_t* app_desc = esp_app_get_description();
    if (app_desc) {
        strncpy(ota_status.current_version, app_desc->version, sizeof(ota_status.current_version) - 1);
        ESP_LOGI(TAG, "Current firmware version: %s", ota_status.current_version);
    }
    
    // Log partition info
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running from partition: %s @ 0x%lx", running->label, running->address);
    }
    
    // Check if this is first boot after OTA
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "First boot after OTA - firmware pending verification");
        }
    }
    
    return true;
}

bool ota_mgr_start_update(const char* url)
{
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid URL");
        return false;
    }
    
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    
    if (ota_status.state == OTA_STATE_DOWNLOADING || 
        ota_status.state == OTA_STATE_VERIFYING) {
        ESP_LOGW(TAG, "OTA already in progress");
        xSemaphoreGive(status_mutex);
        return false;
    }
    
    // Store URL and reset status
    strncpy(update_url, url, sizeof(update_url) - 1);
    update_url[sizeof(update_url) - 1] = '\0';
    
    ota_status.state = OTA_STATE_DOWNLOADING;
    ota_status.progress_percent = 0;
    ota_status.bytes_downloaded = 0;
    ota_status.total_bytes = 0;
    ota_status.error_msg[0] = '\0';
    ota_status.new_version[0] = '\0';
    
    xSemaphoreGive(status_mutex);
    
    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    
    // Create OTA task
    // Lower priority than LVGL (5) to avoid display glitches during flash writes
    BaseType_t ret = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE, NULL, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Failed to start OTA task", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        return false;
    }
    
    return true;
}

ota_status_t ota_mgr_get_status(void)
{
    ota_status_t status;
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    memcpy(&status, &ota_status, sizeof(ota_status_t));
    xSemaphoreGive(status_mutex);
    return status;
}

bool ota_mgr_is_busy(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    bool busy = (ota_status.state == OTA_STATE_DOWNLOADING || 
                 ota_status.state == OTA_STATE_VERIFYING);
    xSemaphoreGive(status_mutex);
    return busy;
}

void ota_mgr_reboot(void)
{
    ESP_LOGI(TAG, "Rebooting to apply OTA update...");
    vTaskDelay(pdMS_TO_TICKS(500));  // Allow log to flush
    esp_restart();
}

void ota_mgr_mark_valid(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Marking current firmware as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

void ota_mgr_get_partition_info(char* label, uint32_t* address, uint32_t* size)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        if (label) {
            strncpy(label, running->label, 16);
        }
        if (address) {
            *address = running->address;
        }
        if (size) {
            *size = running->size;
        }
    }
}

// HTTP event handler for progress tracking
static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP connected");
            break;
        case HTTP_EVENT_HEADER_SENT:
            break;
        case HTTP_EVENT_ON_HEADER:
            // Note: Don't parse Content-Length here - with partial_http_download enabled,
            // headers show chunk sizes (e.g., 8192) not actual firmware size.
            // We get the correct total from esp_https_ota_get_image_size() instead.
            ESP_LOGD(TAG, "Header: %s = %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP finished");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP disconnected");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void ota_task(void* pvParameter)
{
    ESP_LOGI(TAG, "OTA task started");
    
    // Reduce verbosity of certificate bundle logging
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    
    esp_err_t err;
    
    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = update_url;
    config.event_handler = http_event_handler;
    config.timeout_ms = 60000;  // 60 second timeout for large files
    config.keep_alive_enable = true;
    config.buffer_size = 8192;  // Larger buffer for HTTPS TLS handshake
    config.buffer_size_tx = 4096;  // Larger TX buffer for GitHub
    
    // Check if HTTPS
    if (strncmp(update_url, "https://", 8) == 0) {
        // Use ESP's built-in certificate bundle for HTTPS verification
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.skip_cert_common_name_check = false;
        ESP_LOGI(TAG, "HTTPS: Using certificate bundle for verification");
    }
    
    // GitHub uses redirects for release downloads (to objects.githubusercontent.com)
    config.disable_auto_redirect = false;
    config.max_redirection_count = 10;
    
    // Perform OTA with partial download (handles redirects/chunked better)
    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &config;
    ota_config.partial_http_download = true;
    ota_config.max_http_request_size = 64 * 1024;  // 64KB chunks for faster download
    
    esp_https_ota_handle_t https_ota_handle = NULL;
    err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        snprintf(ota_status.error_msg, sizeof(ota_status.error_msg), 
                 "OTA begin failed: %s", esp_err_to_name(err));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    
    // Get new app info
    esp_app_desc_t new_app_info;
    err = esp_https_ota_get_img_desc(https_ota_handle, &new_app_info);
    if (err == ESP_OK) {
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        strncpy(ota_status.new_version, new_app_info.version, sizeof(ota_status.new_version) - 1);
        xSemaphoreGive(status_mutex);
        ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
    }
    
    // Get total image size from OTA handle
    // Note: GitHub uses chunked transfer encoding, so this may return 0
    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    ESP_LOGI(TAG, "Image size from server: %d bytes (0 means unknown/chunked)", image_size);
    
    // If image size unknown, use a reasonable estimate based on current firmware size
    // Current firmware is ~1.5MB, so estimate 1.6MB for updates
    int estimated_size = (image_size > 0) ? image_size : (1600 * 1024);
    
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.total_bytes = estimated_size;
    xSemaphoreGive(status_mutex);
    
    if (image_size <= 0) {
        ESP_LOGW(TAG, "Using estimated size: %d bytes (server didn't provide Content-Length)", estimated_size);
    }
    
    // Download and write firmware
    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        // Update progress
        int image_len = esp_https_ota_get_image_len_read(https_ota_handle);
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.bytes_downloaded = image_len;
        if (ota_status.total_bytes > 0) {
            int progress = (image_len * 100) / ota_status.total_bytes;
            // Cap at 99% until download is complete (in case estimate was too small)
            ota_status.progress_percent = (progress > 99) ? 99 : progress;
        }
        xSemaphoreGive(status_mutex);
        
        // Log progress every 10%
        static int last_logged = -1;
        if (ota_status.progress_percent / 10 != last_logged) {
            last_logged = ota_status.progress_percent / 10;
            ESP_LOGI(TAG, "OTA progress: %d%% (%d bytes)", 
                     ota_status.progress_percent, image_len);
        }
        
        // Yield to let display task run (prevents screen flashing)
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        snprintf(ota_status.error_msg, sizeof(ota_status.error_msg), 
                 "Download failed: %s", esp_err_to_name(err));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    
    // Verify and finish
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.state = OTA_STATE_VERIFYING;
    ota_status.progress_percent = 100;
    xSemaphoreGive(status_mutex);
    
    ESP_LOGI(TAG, "Download complete, verifying...");
    
    if (!esp_https_ota_is_complete_data_received(https_ota_handle)) {
        ESP_LOGE(TAG, "Complete data was not received");
        esp_https_ota_abort(https_ota_handle);
        
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Incomplete data received", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    
    err = esp_https_ota_finish(https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            strncpy(ota_status.error_msg, "Image validation failed", sizeof(ota_status.error_msg));
        } else {
            snprintf(ota_status.error_msg, sizeof(ota_status.error_msg), 
                     "OTA finish failed: %s", esp_err_to_name(err));
        }
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    
    // Success!
    ESP_LOGI(TAG, "OTA update successful! Ready to reboot.");
    
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.state = OTA_STATE_READY_TO_REBOOT;
    xSemaphoreGive(status_mutex);
    
    vTaskDelete(NULL);
}
