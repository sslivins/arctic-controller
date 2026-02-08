/*
 * Arctic Heat Pump Controller
 * Main entry point for ESP32-P4 Tab5
 */
#include <stdio.h>
#include <bsp/m5stack_tab5.h>
#include <lvgl.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <mooncake_log.h>
#include "startup_anim.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "status_bar.h"
#include "ota_manager.h"
#include "settings_screen.h"
#include "settings/settings_time_panel.h"
#include "settings/settings_display_panel.h"
#include "i18n/i18n.h"
#include "auth_manager.h"
#include "modbus/modbus_manager.h"
#include "modbus/arctic_heatpump.h"
#include "heatpump_screen.h"

static const char* TAG = "main";

// Main screen reference (for returning from WiFi screen)
static lv_obj_t* main_screen = NULL;

// Forward declarations
void create_ui(void);
static void on_startup_complete(void);
static void on_status_bar_wifi_click(void);
static void on_status_bar_settings_click(void);
static void on_status_bar_notify_item_click(status_bar_notify_type_t type);
static void on_wifi_connect(const char* ssid, const char* password);
static void on_wifi_scan(void);
static void on_wifi_disconnect(void);
static void on_settings_close(void);
static void try_auto_connect(void);
static void wifi_init_task(void* param);
static void on_update_check_complete(bool update_available, const char* new_version);

// Flag to track when to show main UI
static bool show_main_ui = false;

// Flag to track WiFi init completion (set by background task)
static volatile bool wifi_init_complete = false;

// Periodic update check timer
static lv_timer_t* update_check_timer = NULL;
#define UPDATE_CHECK_INTERVAL_MS (60 * 1000)  // 1 minute for testing (change to 60*60*1000 for hourly)

// WiFi stability tracking
#define WIFI_UNSTABLE_THRESHOLD 3       // Number of disconnects to trigger "unstable" notification
#define WIFI_UNSTABLE_WINDOW_MS 300000  // 5 minute window to count disconnects
static uint32_t wifi_disconnect_times[WIFI_UNSTABLE_THRESHOLD] = {0};
static int wifi_disconnect_index = 0;

extern "C" void app_main(void)
{
    mclog::tagInfo(TAG, "Arctic Heat Pump Controller Starting...");

    // Initialize NVS (required for storing settings like timezone, WiFi credentials, etc.)
    mclog::tagInfo(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        mclog::tagWarn(TAG, "NVS partition needs to be erased, erasing...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        mclog::tagError(TAG, "Failed to initialize NVS: {}", ret);
    }

    // Initialize i18n (localization) system
    i18n_init();

    // Initialize I2C (required for IO expander and other peripherals)
    mclog::tagInfo(TAG, "Initializing I2C...");
    bsp_i2c_init();

    // Initialize IO expander (required for display control)
    mclog::tagInfo(TAG, "Initializing IO expander...");
    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
    bsp_io_expander_pi4ioe_init(i2c_bus_handle);

    // Reset touch panel
    bsp_reset_tp();

    // Initialize Tab5 BSP (display, touch, etc.)
    // NOTE: sw_rotate is disabled because it causes visible tearing during scrolling.
    // The PPA-accelerated rotation process isn't synchronized with VSync, resulting in
    // horizontal tear lines on animated content. Using native portrait mode (720x1280)
    // eliminates tearing. MADCTL hardware rotation was tested but ST7123 DSI panel
    // doesn't support it. If landscape is needed, re-enable sw_rotate and accept tearing.
    bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = false,  // Disabled - causes tearing during scroll (see note above)
        }
    };
    display_cfg.lvgl_port_cfg.task_priority = 5;
    
    lv_display_t* display = bsp_display_start_with_config(&display_cfg);
    if (display == NULL) {
        mclog::tagError(TAG, "Failed to initialize display!");
        return;
    }

    // Portrait mode - rotation has no effect with sw_rotate disabled
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
    bsp_display_backlight_on();
    
    // Initialize display brightness from saved settings
    display_panel_init_brightness();

    mclog::tagInfo(TAG, "Display initialized: {}x{}", BSP_LCD_H_RES, BSP_LCD_V_RES);

    // Start the startup animation
    bsp_display_lock(0);
    startup_anim_init(on_startup_complete);
    bsp_display_unlock();

    mclog::tagInfo(TAG, "Startup animation started");

    // Initialize time manager (NTP will start when WiFi connects)
    time_mgr_init();

    // Initialize Modbus and Arctic heat pump communication
    esp_err_t modbus_ret = modbus::init();
    if (modbus_ret == ESP_OK) {
        arctic::init();
        arctic::startPolling();
        mclog::tagInfo(TAG, "Modbus initialized, heat pump polling started");
    } else {
        mclog::tagError(TAG, "Failed to initialize Modbus: {}", (int)modbus_ret);
    }

    // Initialize authentication manager
    auth_mgr_init();

    // Initialize OTA manager
    ota_mgr_init();
    
    // Mark firmware as valid (prevents rollback after successful boot)
    ota_mgr_mark_valid();

    // Start WiFi initialization in background task (runs parallel to animation)
    xTaskCreate(wifi_init_task, "wifi_init", 4096, NULL, 5, NULL);

    // Main loop
    while (1) {
        // Update startup animation if running
        if (startup_anim_is_running()) {
            bsp_display_lock(0);
            startup_anim_update();
            bsp_display_unlock();
        }
        
        // Check if we should show the main UI after animation completes
        if (show_main_ui) {
            bsp_display_lock(0);
            create_ui();
            bsp_display_unlock();
            mclog::tagInfo(TAG, "UI Created");
            show_main_ui = false;  // Only create once
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void on_startup_complete(void)
{
    mclog::tagInfo(TAG, "Startup animation complete");
    show_main_ui = true;
}

void create_ui(void)
{
    // Get the active screen and save reference
    lv_obj_t* scr = lv_scr_act();
    main_screen = scr;
    
    // Set background color
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    
    // Create status bar at the top (time + WiFi icon)
    status_bar_config_t bar_config = {
        .parent = scr,
        .on_wifi_click = on_status_bar_wifi_click,
        .on_time_click = NULL,  // Time click disabled - use Settings > Time instead
        .on_settings_click = on_status_bar_settings_click,
        .on_notify_item_click = on_status_bar_notify_item_click,
    };
    status_bar_create(&bar_config);
    
    // Create heat pump status display (main content area)
    // Status bar is 80px, leave some margin
    heatpump_screen_create(scr, 90);
    
    // Footer
    lv_obj_t* footer = lv_label_create(scr);
    lv_label_set_text(footer, "M5Stack Tab5 • ESP32-P4");
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(footer, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);
    
    // WiFi init happens in background task, just update status bar when ready
    if (wifi_init_complete && wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) {
        status_bar_set_wifi_state(true, wifi_mgr_get_connected_ssid());
    }
}

// ============================================================================
// WiFi Screen Integration
// ============================================================================

// Pending credentials to save on successful connection
static char pending_ssid[33] = {};
static char pending_password[65] = {};

// Callback when scan completes (runs in sys_evt task - use ESP_LOGI to avoid stack overflow)
static void on_scan_done(const wifi_mgr_ap_info_t* ap_list, uint16_t count)
{
    ESP_LOGI(TAG, "Scan complete, found %d networks", count);
    
    // Convert to settings_screen format
    settings_wifi_network_t* networks = new settings_wifi_network_t[count];
    for (uint16_t i = 0; i < count; i++) {
        strncpy(networks[i].ssid, ap_list[i].ssid, sizeof(networks[i].ssid) - 1);
        networks[i].rssi = ap_list[i].rssi;
        networks[i].authmode = ap_list[i].authmode;
    }
    
    // Update UI (must be done with LVGL lock)
    bsp_display_lock(0);
    settings_screen_set_scanning(false);
    settings_screen_update_networks(networks, count);
    bsp_display_unlock();
    
    delete[] networks;
}

// Callback when connection state changes (runs in sys_evt task - use ESP_LOGI)
static void on_wifi_state_changed(wifi_mgr_state_t state, const char* ssid)
{
    bsp_display_lock(0);
    
    switch (state) {
        case WIFI_MGR_STATE_CONNECTING:
            ESP_LOGI(TAG, "WiFi connecting...");
            status_bar_set_wifi_connecting(true);
            break;
            
        case WIFI_MGR_STATE_CONNECTED: {
            ESP_LOGI(TAG, "WiFi connected to '%s'", ssid ? ssid : "?");
            char ip[16] = {};
            wifi_mgr_get_ip_addr(ip, sizeof(ip));
            settings_screen_set_wifi_status(true, ssid, ip);
            status_bar_set_wifi_state(true, ssid);
            // Save credentials on successful connection
            if (pending_ssid[0] != '\0') {
                wifi_mgr_save_credentials(pending_ssid, pending_password);
                pending_ssid[0] = '\0';
                pending_password[0] = '\0';
            }
            // Start periodic firmware update checks (if not already running)
            if (!update_check_timer) {
                // Check immediately on first connect
                settings_screen_check_for_updates_async(on_update_check_complete);
                // Then check periodically
                update_check_timer = lv_timer_create([](lv_timer_t* t) {
                    (void)t;
                    if (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) {
                        ESP_LOGI("main", "Periodic firmware update check...");
                        settings_screen_check_for_updates_async(on_update_check_complete);
                    }
                }, UPDATE_CHECK_INTERVAL_MS, NULL);
            }
            // Clear WiFi unstable notification since we're connected now
            // (but keep tracking - it will reappear if we keep disconnecting)
            break;
        }
            
        case WIFI_MGR_STATE_DISCONNECTED: {
            ESP_LOGI(TAG, "WiFi disconnected");
            settings_screen_set_wifi_status(false, NULL, NULL);
            status_bar_set_wifi_state(false, NULL);
            
            // Track disconnect for instability detection
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);  // Current time in ms
            wifi_disconnect_times[wifi_disconnect_index] = now;
            wifi_disconnect_index = (wifi_disconnect_index + 1) % WIFI_UNSTABLE_THRESHOLD;
            
            // Count disconnects within the time window
            int recent_disconnects = 0;
            for (int i = 0; i < WIFI_UNSTABLE_THRESHOLD; i++) {
                if (wifi_disconnect_times[i] > 0 && 
                    (now - wifi_disconnect_times[i]) < WIFI_UNSTABLE_WINDOW_MS) {
                    recent_disconnects++;
                }
            }
            
            // Show notification if WiFi is unstable
            if (recent_disconnects >= WIFI_UNSTABLE_THRESHOLD) {
                ESP_LOGW(TAG, "WiFi unstable: %d disconnects in last %d seconds", 
                         recent_disconnects, WIFI_UNSTABLE_WINDOW_MS / 1000);
                bsp_display_lock(0);
                status_bar_add_notification(STATUS_BAR_NOTIFY_WIFI_UNSTABLE, 
                    "WiFi unstable - frequent disconnects");
                bsp_display_unlock();
            }
            break;
        }
            
        case WIFI_MGR_STATE_ERROR:
            ESP_LOGE(TAG, "WiFi error");
            settings_screen_show_error("Connection failed.\nPlease check password and try again.");
            status_bar_set_wifi_state(false, NULL);
            // Clear pending credentials on error
            pending_ssid[0] = '\0';
            pending_password[0] = '\0';
            break;
            
        default:
            break;
    }
    
    bsp_display_unlock();
}

static void on_status_bar_wifi_click(void)
{
    mclog::tagInfo(TAG, "Status bar WiFi clicked - opening settings");
    
    // WiFi manager is initialized at startup, but check just in case
    if (!wifi_mgr_is_initialized()) {
        mclog::tagWarn(TAG, "WiFi not initialized, initializing now...");
        if (!wifi_mgr_init()) {
            mclog::tagError(TAG, "Failed to initialize WiFi!");
        }
    }
    
    // Open settings screen (WiFi is now integrated there)
    on_status_bar_settings_click();
}

static void on_wifi_connect(const char* ssid, const char* password)
{
    mclog::tagInfo(TAG, "WiFi connect requested: SSID='{}'", ssid);
    
    if (!wifi_mgr_is_initialized()) {
        settings_screen_show_error("WiFi not initialized.\nPlease try again.");
        return;
    }
    
    // Save credentials to be persisted on successful connection
    strncpy(pending_ssid, ssid, sizeof(pending_ssid) - 1);
    strncpy(pending_password, password ? password : "", sizeof(pending_password) - 1);
    
    if (!wifi_mgr_connect(ssid, password, on_wifi_state_changed)) {
        settings_screen_show_error("Failed to start connection.\nPlease try again.");
    }
}

static void on_wifi_scan(void)
{
    mclog::tagInfo(TAG, "WiFi scan requested");
    
    if (!wifi_mgr_is_initialized()) {
        mclog::tagInfo(TAG, "Initializing WiFi manager for scan...");
        if (!wifi_mgr_init()) {
            settings_screen_set_scanning(false);
            settings_screen_show_error("Failed to initialize WiFi.\nCheck ESP32-C6 module.");
            return;
        }
    }
    
    settings_screen_set_scanning(true);
    
    if (!wifi_mgr_start_scan(on_scan_done)) {
        settings_screen_set_scanning(false);
        settings_screen_show_error("Failed to start scan.\nPlease try again.");
    }
}

static void on_wifi_disconnect(void)
{
    mclog::tagInfo(TAG, "WiFi disconnect requested");
    
    if (wifi_mgr_is_initialized()) {
        wifi_mgr_disconnect();
    }
}

// Settings screen callbacks
static void on_status_bar_settings_click(void)
{
    mclog::tagInfo(TAG, "Status bar settings clicked");
    
    // Ensure WiFi is initialized
    if (!wifi_mgr_is_initialized()) {
        mclog::tagWarn(TAG, "WiFi not initialized, initializing now...");
        wifi_mgr_init();
    }
    
    bsp_display_lock(0);
    settings_screen_config_t config = {
        .on_close = on_settings_close,
        .on_wifi_connect = on_wifi_connect,
        .on_wifi_scan = on_wifi_scan,
        .on_wifi_disconnect = on_wifi_disconnect,
    };
    settings_screen_create(&config);
    bsp_display_unlock();
}

static void on_settings_close(void)
{
    mclog::tagInfo(TAG, "Settings screen closed");
    
    bsp_display_lock(0);
    settings_screen_close();
    
    // Return to main screen
    if (main_screen) {
        lv_scr_load(main_screen);
    }
    bsp_display_unlock();
}

// Notification item clicked - handle based on type
static void on_status_bar_notify_item_click(status_bar_notify_type_t type)
{
    mclog::tagInfo(TAG, "Notification item clicked: type={}", (int)type);
    
    // Clear the notification that was clicked
    bsp_display_lock(0);
    status_bar_clear_notification(type);
    bsp_display_unlock();
    
    // Handle specific actions based on type
    switch (type) {
        case STATUS_BAR_NOTIFY_FIRMWARE_UPDATE:
            // Open settings screen and go directly to firmware panel
            on_status_bar_settings_click();
            bsp_display_lock(0);
            settings_screen_show_firmware_panel();
            bsp_display_unlock();
            break;
        
        case STATUS_BAR_NOTIFY_WIFI_UNSTABLE:
            // Open WiFi screen - user might want to switch networks
            on_status_bar_wifi_click();
            // Reset the disconnect tracking since user acknowledged
            for (int i = 0; i < WIFI_UNSTABLE_THRESHOLD; i++) {
                wifi_disconnect_times[i] = 0;
            }
            break;
        
        case STATUS_BAR_NOTIFY_LOW_BATTERY:
            // Just clear (already done above) - no additional action
            break;
        
        default:
            mclog::tagWarn(TAG, "Unknown notification type: {}", (int)type);
            break;
    }
}

// Callback when background update check completes
static void on_update_check_complete(bool update_available, const char* new_version)
{
    if (update_available) {
        mclog::tagInfo(TAG, "Firmware update available: {}", new_version ? new_version : "?");
        char msg[64];
        snprintf(msg, sizeof(msg), "Firmware v%s available", new_version ? new_version : "?");
        bsp_display_lock(0);
        status_bar_add_notification(STATUS_BAR_NOTIFY_FIRMWARE_UPDATE, msg);
        bsp_display_unlock();
    } else {
        mclog::tagInfo(TAG, "Firmware is up to date");
        // Clear any existing firmware notification
        bsp_display_lock(0);
        status_bar_clear_notification(STATUS_BAR_NOTIFY_FIRMWARE_UPDATE);
        bsp_display_unlock();
    }
}

// Background task to initialize WiFi during startup animation
static void wifi_init_task(void* param)
{
    (void)param;
    
    mclog::tagInfo(TAG, "[BG] Starting WiFi initialization...");
    
    // Initialize WiFi manager (this takes ~4 seconds)
    if (!wifi_mgr_init()) {
        mclog::tagError(TAG, "[BG] Failed to initialize WiFi manager");
        wifi_init_complete = true;
        vTaskDelete(NULL);
        return;
    }
    
    mclog::tagInfo(TAG, "[BG] WiFi manager initialized");
    
    // Check if we have saved credentials to auto-connect
    if (wifi_mgr_has_saved_credentials()) {
        char ssid[33];
        char password[65];
        
        if (wifi_mgr_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
            mclog::tagInfo(TAG, "[BG] Auto-connecting to: {}", ssid);
            
            // Store as pending for state callback
            strncpy(pending_ssid, ssid, sizeof(pending_ssid) - 1);
            strncpy(pending_password, password, sizeof(pending_password) - 1);
            
            // Connect (callback will update UI when connected)
            wifi_mgr_connect(ssid, password, on_wifi_state_changed);
        }
    } else {
        mclog::tagInfo(TAG, "[BG] No saved credentials - WiFi ready for manual config");
    }
    
    wifi_init_complete = true;
    mclog::tagInfo(TAG, "[BG] WiFi init task complete");
    
    vTaskDelete(NULL);
}

// Legacy function - now just checks if we need to init (shouldn't be called)
static void try_auto_connect(void)
{
    // WiFi init now happens in background task during startup animation
    // This function is kept for backwards compatibility
    if (!wifi_mgr_is_initialized()) {
        mclog::tagWarn(TAG, "WiFi not initialized - init task may have failed");
    }
}
