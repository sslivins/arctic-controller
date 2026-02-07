/*
 * Arctic Heat Pump Controller
 * Localization String IDs
 * 
 * Add new string IDs here, then add translations in i18n.cpp
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief String identifiers for localization
 * 
 * Naming convention:
 * - STR_<CONTEXT>_<DESCRIPTION>
 * - Keep related strings grouped together
 */
typedef enum {
    // ========================================================================
    // General / Common
    // ========================================================================
    STR_OK,
    STR_CANCEL,
    STR_ERROR,
    STR_CLOSE,
    STR_SAVE,
    STR_BACK,
    STR_LOADING,
    STR_PLEASE_WAIT,
    
    // ========================================================================
    // Settings Screen
    // ========================================================================
    STR_SETTINGS,
    STR_SETTINGS_WIFI,
    STR_SETTINGS_UPDATE,
    STR_SETTINGS_LANGUAGE,
    
    // ========================================================================
    // WiFi Panel
    // ========================================================================
    STR_WIFI_CONNECTED,
    STR_WIFI_DISCONNECTED,
    STR_WIFI_DISCONNECT,
    STR_WIFI_CONNECT,
    STR_WIFI_SCANNING,
    STR_WIFI_AVAILABLE_NETWORKS,
    STR_WIFI_NO_NETWORKS,
    STR_WIFI_ENTER_PASSWORD,
    STR_WIFI_PASSWORD,
    STR_WIFI_NETWORK,
    STR_WIFI_IP_ADDRESS,
    STR_WIFI_SIGNAL_EXCELLENT,
    STR_WIFI_SIGNAL_GOOD,
    STR_WIFI_SIGNAL_FAIR,
    STR_WIFI_SIGNAL_WEAK,
    STR_WIFI_FAILED_INIT,
    STR_WIFI_FAILED_SCAN,
    STR_WIFI_FAILED_CONNECT,
    
    // ========================================================================
    // Firmware Update Panel
    // ========================================================================
    STR_FW_TITLE,
    STR_FW_CURRENT,
    STR_FW_LATEST,
    STR_FW_CHECKING,
    STR_FW_UP_TO_DATE,
    STR_FW_UPDATE_AVAILABLE,
    STR_FW_DOWNLOADING,
    STR_FW_VERIFYING,
    STR_FW_INSTALL_UPDATE,
    STR_FW_REBOOTING,
    STR_FW_UPDATE_COMPLETE,
    STR_FW_UPDATE_FAILED,
    STR_FW_CHECK_FAILED,
    
    // ========================================================================
    // Language Panel
    // ========================================================================
    STR_LANG_TITLE,
    STR_LANG_ENGLISH,
    STR_LANG_FRENCH,
    STR_LANG_SPANISH,
    STR_LANG_SELECT,
    STR_LANG_CURRENT,
    STR_LANG_RESTART_REQUIRED,
    
    // ========================================================================
    // Status Bar / Notifications
    // ========================================================================
    STR_NOTIFY_UPDATE_AVAILABLE,
    STR_NOTIFY_WIFI_UNSTABLE,
    STR_NOTIFY_LOW_BATTERY,
    
    // ========================================================================
    // Time Panel
    // ========================================================================
    STR_TIME_TITLE,
    STR_TIME_DATE,
    STR_TIME_TIMEZONE,
    STR_TIME_FORMAT_12H,
    STR_TIME_FORMAT_24H,
    STR_TIME_DISPLAY_FORMAT,
    STR_TIME_FORMAT_INFO,
    STR_TIME_SYNCED,
    STR_TIME_NOT_SYNCED,
    STR_SETTINGS_TIME,
    
    // ========================================================================
    // Display Panel
    // ========================================================================
    STR_SETTINGS_DISPLAY,
    STR_DISPLAY_TITLE,
    STR_DISPLAY_BRIGHTNESS,
    STR_DISPLAY_BRIGHTNESS_LOW,
    STR_DISPLAY_BRIGHTNESS_HIGH,
    
    // ========================================================================
    // Must be last - used for array sizing
    // ========================================================================
    STR_COUNT
    
} string_id_t;

#ifdef __cplusplus
}
#endif
