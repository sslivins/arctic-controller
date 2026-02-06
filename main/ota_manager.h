/*
 * Arctic Heat Pump Controller
 * OTA Update Manager
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// OTA update states
typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_READY_TO_REBOOT,
    OTA_STATE_FAILED
} ota_state_t;

// OTA progress info
typedef struct {
    ota_state_t state;
    int progress_percent;       // 0-100
    size_t bytes_downloaded;
    size_t total_bytes;
    char error_msg[128];
    char current_version[32];
    char new_version[32];
} ota_status_t;

/**
 * @brief Initialize OTA manager
 * @return true on success
 */
bool ota_mgr_init(void);

/**
 * @brief Check if a URL is allowed for OTA updates
 * @param url URL to check
 * @return true if URL is allowed
 */
bool ota_mgr_is_url_allowed(const char* url);

/**
 * @brief Start OTA update from URL
 * @param url URL to firmware binary (http or https)
 * @return true if update started, false if already in progress or error
 */
bool ota_mgr_start_update(const char* url);

/**
 * @brief Get current OTA status
 * @return Current OTA status
 */
ota_status_t ota_mgr_get_status(void);

/**
 * @brief Check if OTA update is in progress
 * @return true if update in progress
 */
bool ota_mgr_is_busy(void);

/**
 * @brief Reboot to apply pending update
 * Should only be called when state is OTA_STATE_READY_TO_REBOOT
 */
void ota_mgr_reboot(void);

/**
 * @brief Mark current firmware as valid (rollback protection)
 * Call this after successful boot to prevent rollback
 */
void ota_mgr_mark_valid(void);

/**
 * @brief Get running partition info
 * @param label Buffer to store partition label (min 16 bytes)
 * @param address Pointer to store partition address (can be NULL)
 * @param size Pointer to store partition size (can be NULL)
 */
void ota_mgr_get_partition_info(char* label, uint32_t* address, uint32_t* size);

#ifdef __cplusplus
}
#endif
