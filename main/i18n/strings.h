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
    STR_SETTINGS_DEMO_MODE,
    STR_SETTINGS_TEMPERATURE,
    STR_SETTINGS_FACTORY_RESET,
    
    // ========================================================================
    // WiFi Panel
    // ========================================================================
    STR_WIFI_CONNECTED,
    STR_WIFI_DISCONNECTED,
    STR_WIFI_DISCONNECT,
    STR_WIFI_CONNECT,
    STR_WIFI_CONNECTING,
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
    STR_NOTIFY_TITLE,
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
    STR_TIME_24H_FORMAT,
    STR_TIME_DISPLAY_FORMAT,
    STR_TIME_FORMAT_INFO,
    STR_TIME_SYNCED,
    STR_TIME_NOT_SYNCED,
    STR_SETTINGS_TIME,

    // Location + auto-timezone
    STR_LOCATION_TITLE,
    STR_LOCATION_CHANGE,
    STR_LOCATION_NONE,
    STR_LOCATION_SEARCH_HINT,
    STR_LOCATION_SEARCHING,
    STR_LOCATION_SEARCH_FAILED,
    STR_LOCATION_NO_MATCHES,
    STR_LOCATION_SELECT,
    STR_TIME_TZ_AUTOMATIC,
    STR_TIME_TZ_UNMAPPED,
    
    // ========================================================================
    // Display Panel
    // ========================================================================
    STR_SETTINGS_DISPLAY,
    STR_DISPLAY_TITLE,
    STR_DISPLAY_BRIGHTNESS,
    STR_DISPLAY_BRIGHTNESS_LOW,
    STR_DISPLAY_BRIGHTNESS_HIGH,
    STR_DISPLAY_IDLE_TITLE,
    STR_DISPLAY_DIM_AFTER,
    STR_DISPLAY_OFF_AFTER_DIM,
    STR_DISPLAY_IDLE_DESCRIPTION,
    STR_DISPLAY_NEVER,
    STR_DISPLAY_MINUTE,
    STR_DISPLAY_MINUTES,

    // ========================================================================
    // Home Assistant Pairing Panel
    // ========================================================================
    STR_SETTINGS_HOME_ASSISTANT,
    STR_HA_TITLE,
    STR_HA_PAIRED,
    STR_HA_NOT_PAIRED,
    STR_HA_PAIRING_DESCRIPTION,
    STR_HA_PAIRING_ACTIVE,
    STR_HA_START_PAIRING,
    STR_HA_REPAIR,
    STR_HA_PAIRED_DESCRIPTION,
    STR_HA_CANCEL_PAIRING,
    STR_HA_PAIRING_CODE,
    STR_HA_FINGERPRINT,
    STR_HA_EXPIRES_IN,
    STR_HA_REVOKE,
    STR_HA_REVOKE_ACTION,
    STR_HA_REVOKE_CONFIRM,
    STR_HA_REVOKE_DESCRIPTION,
    STR_HA_PAIRING_FAILED,
    
    // ========================================================================
    // Security Panel (Home-Assistant-independent device securing)
    // ========================================================================
    STR_SETTINGS_SECURITY,
    STR_SECURITY_TITLE,
    STR_SECURITY_SECURED,
    STR_SECURITY_NOT_SECURED,
    STR_SECURITY_NOT_SECURED_DESC,
    STR_SECURITY_SECURED_DESC,
    STR_SECURITY_SHOW_CODE,
    STR_SECURITY_HIDE_CODE,
    STR_SECURITY_CODE_LABEL,
    STR_SECURITY_CODE_ACTIVE,
    STR_SECURITY_EXPIRES_IN,
    STR_SECURITY_FAILED,

    // ========================================================================
    // Web Interface (QR code + URL to the browser dashboard)
    // ========================================================================
    STR_SETTINGS_WEB,
    STR_WEB_TITLE,
    STR_WEB_SCAN_HINT,
    STR_WEB_URL_LABEL,
    STR_WEB_URL_UNAVAILABLE,
    
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
    STR_HP_POWER_UNAVAILABLE,
    STR_HP_HOLD_POWER_OFF,
    STR_HP_RESERVED_1,  // unused, kept for enum stability
    STR_HP_RESERVED_2,  // unused, kept for enum stability
    STR_HP_DEMO_MODE_ENABLED,
    STR_HP_NOT_CONNECTED,
    STR_HP_BTN_TEMPS,
    STR_HP_BTN_SYSTEM,
    STR_HP_BTN_ADVANCED,
    STR_HP_BTN_STATUS,
    STR_NAV_HOME,
    
    // Heat Pump - Mode Names
    STR_HP_MODE_COOLING,
    STR_HP_MODE_HEATING,
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
    STR_HISTORY_OPEN,
    STR_HISTORY_TITLE,
    STR_HISTORY_PREVIOUS,
    STR_HISTORY_NEXT,
    STR_HISTORY_LATEST,
    STR_HISTORY_BACK,
    STR_HISTORY_INLET,
    STR_HISTORY_OUTLET,
    STR_HISTORY_SETPOINT,
    STR_HISTORY_LOADING,
    STR_HISTORY_NO_DATA,
    STR_HISTORY_WAITING_FOR_TIME,
    STR_HISTORY_STORAGE_ERROR,
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
    STR_HP_STATUS,
    STR_HP_SYSTEM_SECTION,
    STR_HP_DEMO_SYSTEM,
    STR_HP_FREQUENCY,
    STR_HP_FAN_SPEED,
    STR_HP_ELECTRICAL,
    STR_HP_AC_VOLTAGE,
    STR_HP_AC_CURRENT,
    STR_HP_DC_VOLTAGE,
    STR_HP_EXPANSION_VALVES,
    STR_HP_PRIMARY_EEV,
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
    STR_HP_SYSTEM_OK,
    STR_HP_ACTIVE_ERRORS,
    STR_HP_CLEAR_HISTORY,
    STR_HP_ERROR_HISTORY,
    
    // ========================================================================
    // Heat Pump - Control Screen
    // ========================================================================
    STR_HP_ADVANCED,
    STR_HP_DEMO_ADVANCED,
    STR_HP_EDIT_PARAMETER,
    STR_HP_RANGE_FMT,
    STR_HP_KRATIO_REDUCE,
    STR_HP_KRATIO_NONE,
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
    // Heat Pump - Dashboard Labels (main screen redesign)
    // ========================================================================
    STR_HP_COMP_RUNNING,
    STR_HP_COMP_IDLE,
    STR_HP_STANDBY_DEMAND,
    STR_HP_DELTA_ABOVE_FMT,
    STR_HP_DELTA_BELOW_FMT,
    STR_HP_DELTA_AT,
    STR_HP_LABEL_INLET,
    STR_HP_LABEL_OUTLET,
    STR_HP_LABEL_OUTDOOR,
    STR_HP_LABEL_COIL,
    STR_HP_STATE_FAULT,
    STR_HP_ENERGY,
    STR_HP_LABEL_COP,
    STR_HP_LABEL_POWER,
    STR_HP_LABEL_FAN,
    STR_HP_LABEL_DISCHARGE,
    STR_HP_LABEL_SUCTION,
    STR_HP_LABEL_EEV,
    STR_HP_LABEL_HI_PRESS,
    STR_HP_LABEL_LO_PRESS,
    STR_HP_LABEL_POWER_IN,
    STR_HP_LABEL_HEAT_OUT,
    STR_HP_LABEL_COOLING_OUT,

    // ========================================================================
    // Event Log
    // ========================================================================
    STR_EVENT_LOG,
    STR_EVENT_SYSTEM_START,
    STR_EVENT_POWER_ON,
    STR_EVENT_POWER_OFF,
    STR_EVENT_MODE_CHANGED,
    STR_EVENT_SETPOINT_CHANGED,
    STR_EVENT_COMPRESSOR_ON,
    STR_EVENT_COMPRESSOR_OFF,
    STR_EVENT_FAN_ON,
    STR_EVENT_FAN_OFF,
    STR_EVENT_PUMP_ON,
    STR_EVENT_PUMP_OFF,
    STR_EVENT_AUX_HEATER_ON,
    STR_EVENT_AUX_HEATER_OFF,
    STR_EVENT_DEFROST_START,
    STR_EVENT_DEFROST_END,
    STR_EVENT_ERROR_APPEARED,
    STR_EVENT_ERROR_CLEARED,
    STR_EVENT_CONNECTED,
    STR_EVENT_DISCONNECTED,
    STR_EVENT_BROWNOUT_RESET,
    STR_EVENT_APPLICATION_CRASH,
    STR_EVENT_WATCHDOG_RESET,
    STR_EVENT_WATCHDOG_INTERRUPT,
    STR_EVENT_WATCHDOG_TASK,
    STR_EVENT_WATCHDOG_OTHER,
    STR_EVENT_CLEAR,
    STR_EVENT_NO_EVENTS,
    STR_EVENT_SHOW_OLDER,
    STR_EVENT_TODAY,
    STR_EVENT_YESTERDAY,
    STR_EVENT_SINCE_RESTART,
    STR_EVENT_SEARCH,
    STR_EVENT_FILTERS,
    STR_EVENT_PROBLEMS,
    STR_EVENT_EQUIPMENT,
    STR_EVENT_CHANGES,
    STR_EVENT_SYSTEM,
    STR_EVENT_RESULTS_FMT,
    STR_EVENT_NO_MATCHES,
    STR_EVENT_NEW_MATCHES,
    STR_EVENT_TIME_ALL,
    STR_EVENT_TIME_TODAY,
    STR_EVENT_TIME_24_HOURS,
    STR_EVENT_TIME_7_DAYS,
    STR_EVENT_TIME_SINCE_RESTART,
    STR_EVENT_RESET_FILTERS,
    STR_EVENT_APPLY_SEARCH,
    STR_EVENT_APPLY_FILTERS,
    STR_EVENT_CLEAR_CONFIRM_TITLE,
    STR_EVENT_CLEAR_CONFIRM_TEXT,
    STR_EVENT_MONTH_JAN,
    STR_EVENT_MONTH_FEB,
    STR_EVENT_MONTH_MAR,
    STR_EVENT_MONTH_APR,
    STR_EVENT_MONTH_MAY,
    STR_EVENT_MONTH_JUN,
    STR_EVENT_MONTH_JUL,
    STR_EVENT_MONTH_AUG,
    STR_EVENT_MONTH_SEP,
    STR_EVENT_MONTH_OCT,
    STR_EVENT_MONTH_NOV,
    STR_EVENT_MONTH_DEC,

    // ========================================================================
    // Reboot confirmation
    // ========================================================================
    STR_DEMO_MODE_CHANGED,
    STR_RESTART_REQUIRED,
    STR_RESTART,
    STR_FACTORY_RESET_TITLE,
    STR_FACTORY_RESET_DESCRIPTION,
    STR_FACTORY_RESET_WARNING,
    STR_FACTORY_RESET_CONFIRM,
    STR_FACTORY_RESET_ERASING,
    STR_FACTORY_RESET_START_FAILED,

    // ========================================================================
    // Must be last - used for array sizing
    // ========================================================================
    STR_COUNT
    
} string_id_t;

#ifdef __cplusplus
}
#endif
