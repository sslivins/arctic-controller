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
#include "wifi_screen.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "time_screen.h"
#include "status_bar.h"
#include "ota_manager.h"
#include "settings_screen.h"

static const char* TAG = "main";

// Main screen reference (for returning from WiFi screen)
static lv_obj_t* main_screen = NULL;

// Forward declarations
void create_ui(void);
static void on_startup_complete(void);
static void show_wifi_screen(void);
static void show_time_screen(void);
static void on_status_bar_wifi_click(void);
static void on_status_bar_time_click(void);
static void on_status_bar_settings_click(void);
static void on_status_bar_notify_item_click(status_bar_notify_type_t type);
static void on_wifi_connect(const char* ssid, const char* password);
static void on_wifi_scan(void);
static void on_wifi_disconnect(void);
static void on_wifi_close(void);
static void on_time_close(void);
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
        mclog::tagError(TAG, "Failed to initialize NVS: %d", ret);
    }

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
    bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = true,
        }
    };
    display_cfg.lvgl_port_cfg.task_priority = 5;
    
    lv_display_t* display = bsp_display_start_with_config(&display_cfg);
    if (display == NULL) {
        mclog::tagError(TAG, "Failed to initialize display!");
        return;
    }

    // Set rotation and turn on backlight
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
    bsp_display_backlight_on();

    mclog::tagInfo(TAG, "Display initialized: %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);

    // Start the startup animation
    bsp_display_lock(0);
    startup_anim_init(on_startup_complete);
    bsp_display_unlock();

    mclog::tagInfo(TAG, "Startup animation started");

    // Initialize time manager (NTP will start when WiFi connects)
    time_mgr_init();

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
        .on_time_click = on_status_bar_time_click,
        .on_settings_click = on_status_bar_settings_click,
        .on_notify_item_click = on_status_bar_notify_item_click,
    };
    status_bar_create(&bar_config);
    
    // Create title label (adjusted for larger status bar - 80px)
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Arctic Heat Pump");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00d4ff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 100);

    // Create subtitle
    lv_obj_t* subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Controller v0.1");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 150);

    // Create a container for the main content
    lv_obj_t* container = lv_obj_create(scr);
    lv_obj_set_size(container, 600, 380);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x16213e), LV_PART_MAIN);
    lv_obj_set_style_border_color(container, lv_color_hex(0x0f3460), LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(container, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 30, LV_PART_MAIN);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Temperature display (placeholder)
    lv_obj_t* temp_label = lv_label_create(container);
    lv_label_set_text(temp_label, "-- °C");
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(temp_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(temp_label, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* temp_desc = lv_label_create(container);
    lv_label_set_text(temp_desc, "Current Temperature");
    lv_obj_set_style_text_font(temp_desc, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(temp_desc, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(temp_desc, LV_ALIGN_TOP_MID, 0, 110);

    // Status label
    lv_obj_t* status = lv_label_create(container);
    lv_label_set_text(status, "Status: Ready");
    lv_obj_set_style_text_font(status, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(status, lv_color_hex(0x00d4ff), LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 50);

    // Footer
    lv_obj_t* footer = lv_label_create(scr);
    lv_label_set_text(footer, "M5Stack Tab5 • ESP32-P4 • LVGL 9.2");
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(footer, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -20);
    
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
    
    // Convert to wifi_screen format
    wifi_network_info_t* networks = new wifi_network_info_t[count];
    for (uint16_t i = 0; i < count; i++) {
        strncpy(networks[i].ssid, ap_list[i].ssid, sizeof(networks[i].ssid) - 1);
        networks[i].rssi = ap_list[i].rssi;
        networks[i].authmode = ap_list[i].authmode;
    }
    
    // Update UI (must be done with LVGL lock)
    bsp_display_lock(0);
    wifi_screen_set_scanning(false);
    wifi_screen_update_networks(networks, count);
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
            wifi_screen_set_connection_status(true, ssid, ip);
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
            wifi_screen_set_connection_status(false, NULL, NULL);
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
            wifi_screen_show_error("Connection failed.\nPlease check password and try again.");
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
    mclog::tagInfo(TAG, "Status bar WiFi clicked");
    
    // WiFi manager is initialized at startup, but check just in case
    if (!wifi_mgr_is_initialized()) {
        mclog::tagWarn(TAG, "WiFi not initialized, initializing now...");
        if (!wifi_mgr_init()) {
            mclog::tagError(TAG, "Failed to initialize WiFi!");
        }
    }
    
    bsp_display_lock(0);
    show_wifi_screen();
    bsp_display_unlock();
}

static void show_wifi_screen(void)
{
    wifi_screen_config_t config = {
        .on_connect = on_wifi_connect,
        .on_scan = on_wifi_scan,
        .on_disconnect = on_wifi_disconnect,
        .on_close = on_wifi_close,
    };
    wifi_screen_create(&config);
    
    // Update connection status if already connected
    if (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) {
        char ip[16] = {};
        wifi_mgr_get_ip_addr(ip, sizeof(ip));
        wifi_screen_set_connection_status(true, wifi_mgr_get_connected_ssid(), ip);
    }
}

static void on_wifi_connect(const char* ssid, const char* password)
{
    mclog::tagInfo(TAG, "WiFi connect requested: SSID='%s'", ssid);
    
    if (!wifi_mgr_is_initialized()) {
        wifi_screen_show_error("WiFi not initialized.\nPlease try again.");
        return;
    }
    
    // Save credentials to be persisted on successful connection
    strncpy(pending_ssid, ssid, sizeof(pending_ssid) - 1);
    strncpy(pending_password, password ? password : "", sizeof(pending_password) - 1);
    
    if (!wifi_mgr_connect(ssid, password, on_wifi_state_changed)) {
        wifi_screen_show_error("Failed to start connection.\nPlease try again.");
    }
}

static void on_wifi_scan(void)
{
    mclog::tagInfo(TAG, "WiFi scan requested");
    
    if (!wifi_mgr_is_initialized()) {
        mclog::tagInfo(TAG, "Initializing WiFi manager for scan...");
        if (!wifi_mgr_init()) {
            wifi_screen_set_scanning(false);
            wifi_screen_show_error("Failed to initialize WiFi.\nCheck ESP32-C6 module.");
            return;
        }
    }
    
    wifi_screen_set_scanning(true);
    
    if (!wifi_mgr_start_scan(on_scan_done)) {
        wifi_screen_set_scanning(false);
        wifi_screen_show_error("Failed to start scan.\nPlease try again.");
    }
}

static void on_wifi_disconnect(void)
{
    mclog::tagInfo(TAG, "WiFi disconnect requested");
    
    if (wifi_mgr_is_initialized()) {
        wifi_mgr_disconnect();
    }
}

static void on_wifi_close(void)
{
    mclog::tagInfo(TAG, "WiFi screen closed");
    
    // Return to main screen
    if (main_screen) {
        lv_scr_load(main_screen);
    }
}

// Time screen callbacks
static void on_status_bar_time_click(void)
{
    mclog::tagInfo(TAG, "Status bar time clicked");
    
    bsp_display_lock(0);
    show_time_screen();
    bsp_display_unlock();
}

static void show_time_screen(void)
{
    time_screen_config_t config = {
        .on_close = on_time_close,
    };
    time_screen_create(&config);
}

static void on_time_close(void)
{
    mclog::tagInfo(TAG, "Time screen closed");
    
    bsp_display_lock(0);
    time_screen_delete();
    
    // Return to main screen
    if (main_screen) {
        lv_scr_load(main_screen);
    }
    
    // Force status bar to update with new format
    status_bar_update_time();
    bsp_display_unlock();
}

// Settings screen callbacks
static void on_status_bar_settings_click(void)
{
    mclog::tagInfo(TAG, "Status bar settings clicked");
    
    bsp_display_lock(0);
    settings_screen_config_t config = {
        .on_close = on_settings_close,
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
            // Open settings screen (which shows firmware updates)
            on_status_bar_settings_click();
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
        mclog::tagInfo(TAG, "Firmware update available: %s", new_version);
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
            mclog::tagInfo(TAG, "[BG] Auto-connecting to: %s", ssid);
            
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
