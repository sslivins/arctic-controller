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
    // Heat Pump - Main Screen
    // ========================================================================
    STR_HP_COMMUNICATION_ERROR,
    STR_HP_DISCONNECTED,
    STR_HP_STANDBY,
    STR_HP_TANK_TEMPERATURE,
    STR_HP_SETPOINT,
    STR_HP_MODE,
    STR_HP_POWER_ON,
    STR_HP_POWER_OFF,
    STR_HP_DEMO_MODE_ENABLED,
    STR_HP_NOT_CONNECTED,
    STR_HP_BTN_TEMPS,
    STR_HP_BTN_SYSTEM,
    STR_HP_BTN_ADVANCED,
    
    // Heat Pump - Mode Names
    STR_HP_MODE_COOLING,
    STR_HP_MODE_FLOOR_HEAT,
    STR_HP_MODE_FAN_HEAT,
    STR_HP_MODE_HOT_WATER,
    STR_HP_MODE_AUTO,
    STR_HP_MODE_DEFROST,
    STR_HP_MODE_UNKNOWN,
    
    // Heat Pump - Mode Dropdown Options
    STR_HP_OPT_COOLING,
    STR_HP_OPT_FLOOR_HEATING,
    STR_HP_OPT_FAN_COIL_HEATING,
    STR_HP_OPT_HOT_WATER,
    STR_HP_OPT_AUTO,
    
    // Heat Pump - Component Labels
    STR_HP_COMPRESSOR,
    STR_HP_FAN,
    STR_HP_PUMP,
    STR_HP_AUX_HEAT,
    
    // Heat Pump - Fan Speeds
    STR_HP_FAN_LOW,
    STR_HP_FAN_MED,
    STR_HP_FAN_HIGH,
    
    // Heat Pump - Setpoint Names
    STR_HP_COOLING,
    STR_HP_HEATING,
    STR_HP_HOT_WATER,
    
    // ========================================================================
    // Heat Pump - Temperatures Screen
    // ========================================================================
    STR_HP_TEMPERATURES,
    STR_HP_DEMO_TEMPERATURES,
    STR_HP_WATER_TANK,
    STR_HP_WATER_OUTLET,
    STR_HP_WATER_INLET,
    STR_HP_OUTDOOR_AMBIENT,
    STR_HP_DISCHARGE,
    STR_HP_SUCTION,
    STR_HP_OUTDOOR_COIL,
    STR_HP_INDOOR_COIL,
    STR_HP_IPM_MODULE,
    
    // ========================================================================
    // Heat Pump - System Screen
    // ========================================================================
    STR_HP_SYSTEM_READINGS,
    STR_HP_DEMO_SYSTEM,
    STR_HP_FREQUENCY,
    STR_HP_FAN_SPEED,
    STR_HP_ELECTRICAL,
    STR_HP_AC_VOLTAGE,
    STR_HP_AC_CURRENT,
    STR_HP_DC_VOLTAGE,
    STR_HP_DC_CURRENT,
    STR_HP_PRESSURES,
    STR_HP_HIGH_PRESSURE,
    STR_HP_LOW_PRESSURE,
    STR_HP_EXPANSION_VALVES,
    STR_HP_PRIMARY_EEV,
    STR_HP_SECONDARY_EEV,
    STR_HP_SETPOINTS,
    
    // ========================================================================
    // Heat Pump - Errors Screen
    // ========================================================================
    STR_HP_ERROR_STATUS,
    STR_HP_DEMO_ERRORS,
    STR_HP_RESOLUTION,
    STR_HP_CONTACT_DEALER,
    STR_HP_STARTED,
    STR_HP_ACTIVE_FOR,
    STR_HP_DURATION,
    STR_HP_JUST_DETECTED,
    STR_HP_DISCONNECTED_MSG,
    STR_HP_NO_ERRORS,
    
    // ========================================================================
    // Heat Pump - Advanced/Params Screen
    // ========================================================================
    STR_HP_ADVANCED,
    STR_HP_DEMO_ADVANCED,
    STR_HP_EDIT_PARAMETER,
    STR_HP_RANGE_FMT,
    STR_HP_COOLING_SETPOINT,
    STR_HP_HEATING_SETPOINT,
    STR_HP_HOT_WATER_SETPOINT,
    STR_HP_CANNOT_SAVE,
    STR_HP_CANNOT_SAVE_SETPOINT,
    STR_HP_CAT_EEV,
    STR_HP_CAT_DEFROST,
    STR_HP_CAT_PROTECTION,
    STR_HP_CAT_AUTO_MODE,
    STR_HP_CAT_PUMP_VALVE,

    // Heat Pump - P-parameter names
    STR_HP_PARAM_EEV_OPENING,
    STR_HP_PARAM_EEV_MODE,
    STR_HP_PARAM_TARGET_SUPERHEAT,
    STR_HP_PARAM_DEFROST_CYCLE,
    STR_HP_PARAM_DEFROST_ENTER_TEMP,
    STR_HP_PARAM_DEFROST_EXTEND_TEMP,
    STR_HP_PARAM_DEFROST_TEMP_DIFF,
    STR_HP_PARAM_DEFROST_EXTEND_TIME,
    STR_HP_PARAM_MAX_DEFROST_TIME,
    STR_HP_PARAM_DEFROST_EXIT_TEMP,
    STR_HP_PARAM_LOW_AMBIENT_PROTECT,
    STR_HP_PARAM_FREQ_REDUCTION,
    STR_HP_PARAM_COOLING_LOW_AMBIENT,
    STR_HP_PARAM_MAX_SETTING_TEMP,
    STR_HP_PARAM_COOLING_AUTO_TEMP,
    STR_HP_PARAM_HEATING_AUTO_TEMP,
    STR_HP_PARAM_MODE_SWITCH_DELAY,
    STR_HP_PARAM_STERILIZE_TIME,
    STR_HP_PARAM_WATER_RETURN_TEMP,
    STR_HP_PARAM_WATER_RETURN_TIME,
    STR_HP_PARAM_3WAY_VALVE_TIME,
    STR_HP_PARAM_PUMP_MODE,
    STR_HP_PARAM_PUMP_INTERVAL,
    STR_HP_PARAM_PUMP_LOW_AMBIENT,
    STR_HP_PARAM_WATERWAY_CLEAN,
    
    // ========================================================================
    // Must be last - used for array sizing
    // ========================================================================
    STR_COUNT
    
} string_id_t;

#ifdef __cplusplus
}
#endif
