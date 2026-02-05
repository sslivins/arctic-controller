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
    BaseType_t ret = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE, NULL, 5, NULL);
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
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                xSemaphoreTake(status_mutex, portMAX_DELAY);
                ota_status.total_bytes = atoi(evt->header_value);
                xSemaphoreGive(status_mutex);
                ESP_LOGI(TAG, "Firmware size: %d bytes", (int)ota_status.total_bytes);
            }
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
    
    esp_err_t err;
    
    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = update_url;
    config.event_handler = http_event_handler;
    config.timeout_ms = 30000;
    config.keep_alive_enable = true;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    
    // Check if HTTPS
    if (strncmp(update_url, "https://", 8) == 0) {
        // For HTTPS, you'd need to set cert_pem or skip verification
        // For development, we can skip verification
        config.skip_cert_common_name_check = true;
        ESP_LOGW(TAG, "HTTPS: Skipping certificate verification (development mode)");
    }
    
    // Perform OTA
    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &config;
    
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
            ota_status.progress_percent = (image_len * 100) / ota_status.total_bytes;
        }
        xSemaphoreGive(status_mutex);
        
        // Log progress every 10%
        static int last_logged = -1;
        if (ota_status.progress_percent / 10 != last_logged) {
            last_logged = ota_status.progress_percent / 10;
            ESP_LOGI(TAG, "OTA progress: %d%% (%d bytes)", 
                     ota_status.progress_percent, image_len);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
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
