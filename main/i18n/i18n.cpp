/*
 * Arctic Heat Pump Controller
 * Internationalization (i18n) Implementation
 */
#include "i18n.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "i18n";

// NVS storage
#define NVS_NAMESPACE "settings"
#define NVS_KEY_LANGUAGE "language"

// Current language
static language_t s_current_language = LANG_ENGLISH;

// ============================================================================
// String Tables
// ============================================================================

// English strings (default/fallback)
static const char* strings_en[STR_COUNT] = {
    // General / Common
    [STR_OK] = "OK",
    [STR_CANCEL] = "Cancel",
    [STR_ERROR] = "Error",
    [STR_CLOSE] = "Close",
    [STR_SAVE] = "Save",
    [STR_BACK] = "Back",
    [STR_LOADING] = "Loading...",
    [STR_PLEASE_WAIT] = "Please wait...",
    
    // Settings Screen
    [STR_SETTINGS] = "Settings",
    [STR_SETTINGS_WIFI] = "WiFi",
    [STR_SETTINGS_UPDATE] = "Update",
    [STR_SETTINGS_LANGUAGE] = "Language",
    [STR_SETTINGS_DEMO_MODE] = "Demo Mode",
    [STR_SETTINGS_TEMPERATURE] = "Temperature",
    [STR_SETTINGS_FACTORY_RESET] = "Factory Reset",
    
    // WiFi Panel
    [STR_WIFI_CONNECTED] = "Connected",
    [STR_WIFI_DISCONNECTED] = "Disconnected",
    [STR_WIFI_DISCONNECT] = "Disconnect",
    [STR_WIFI_CONNECT] = "Connect",
    [STR_WIFI_CONNECTING] = "Connecting",
    [STR_WIFI_SCANNING] = "Scanning for networks...",
    [STR_WIFI_AVAILABLE_NETWORKS] = "Available Networks",
    [STR_WIFI_NO_NETWORKS] = "No networks found",
    [STR_WIFI_ENTER_PASSWORD] = "Enter Password",
    [STR_WIFI_PASSWORD] = "Password",
    [STR_WIFI_NETWORK] = "Network",
    [STR_WIFI_IP_ADDRESS] = "IP",
    [STR_WIFI_SIGNAL_EXCELLENT] = "Signal: Excellent",
    [STR_WIFI_SIGNAL_GOOD] = "Signal: Good",
    [STR_WIFI_SIGNAL_FAIR] = "Signal: Fair",
    [STR_WIFI_SIGNAL_WEAK] = "Signal: Weak",
    [STR_WIFI_FAILED_INIT] = "Failed to initialize WiFi.\nCheck ESP32-C6 module.",
    [STR_WIFI_FAILED_SCAN] = "Failed to start scan.\nPlease try again.",
    [STR_WIFI_FAILED_CONNECT] = "Failed to connect.\nCheck password and try again.",
    
    // Firmware Update Panel
    [STR_FW_TITLE] = "Firmware Update",
    [STR_FW_CURRENT] = "Current",
    [STR_FW_LATEST] = "Latest",
    [STR_FW_CHECKING] = "Checking for updates...",
    [STR_FW_UP_TO_DATE] = "You're up to date!",
    [STR_FW_UPDATE_AVAILABLE] = "Update available!",
    [STR_FW_DOWNLOADING] = "Downloading update...",
    [STR_FW_VERIFYING] = "Verifying firmware...",
    [STR_FW_INSTALL_UPDATE] = "Install Update",
    [STR_FW_REBOOTING] = "Rebooting in 3 seconds...",
    [STR_FW_UPDATE_COMPLETE] = "Update complete! Rebooting...",
    [STR_FW_UPDATE_FAILED] = "Update failed",
    [STR_FW_CHECK_FAILED] = "Update check failed",
    
    // Language Panel
    [STR_LANG_TITLE] = "Language",
    [STR_LANG_ENGLISH] = "English",
    [STR_LANG_FRENCH] = "French",
    [STR_LANG_SPANISH] = "Spanish",
    [STR_LANG_SELECT] = "Select Language",
    [STR_LANG_CURRENT] = "Current",
    [STR_LANG_RESTART_REQUIRED] = "Restart may be required for full effect",
    
    // Status Bar / Notifications
    [STR_NOTIFY_UPDATE_AVAILABLE] = "Update available",
    [STR_NOTIFY_WIFI_UNSTABLE] = "WiFi connection unstable",
    [STR_NOTIFY_LOW_BATTERY] = "Low battery",
    
    // Time Panel
    [STR_TIME_TITLE] = "Date & Time",
    [STR_TIME_DATE] = "Date",
    [STR_TIME_TIMEZONE] = "Time Zone",
    [STR_TIME_FORMAT_12H] = "12h",
    [STR_TIME_FORMAT_24H] = "24h",
    [STR_TIME_DISPLAY_FORMAT] = "Display Format",
    [STR_TIME_FORMAT_INFO] = "Choose how time is\ndisplayed in the UI",
    [STR_TIME_SYNCED] = "Synced",
    [STR_TIME_NOT_SYNCED] = "Not synced",
    [STR_SETTINGS_TIME] = "Time",
    
    // Display Panel
    [STR_SETTINGS_DISPLAY] = "Display",
    [STR_DISPLAY_TITLE] = "Display Settings",
    [STR_DISPLAY_BRIGHTNESS] = "Brightness",
    [STR_DISPLAY_BRIGHTNESS_LOW] = "Low",
    [STR_DISPLAY_BRIGHTNESS_HIGH] = "High",
    
    // Heat Pump - Main Screen
    [STR_HP_COMMUNICATION_ERROR] = "Communication Error",
    [STR_HP_DISCONNECTED] = "DISCONNECTED",
    [STR_HP_STANDBY] = "STANDBY",
    [STR_HP_TANK_TEMPERATURE] = "Tank Temperature",
    [STR_HP_SETPOINT] = "Setpoint",
    [STR_HP_MODE] = "Mode",
    [STR_HP_POWER_ON] = "\xEF\x80\x91 POWERED ON",
    [STR_HP_POWER_OFF] = "\xEF\x80\x91 POWERED OFF",
    [STR_HP_POWER_UNAVAILABLE] = "UNAVAILABLE",
    [STR_HP_HOLD_POWER_OFF] = "\xEF\x80\x91 Powering off in %d...",
    [STR_HP_RESERVED_1] = "",
    [STR_HP_RESERVED_2] = "",
    [STR_HP_DEMO_MODE_ENABLED] = "Demo Mode Enabled",
    [STR_HP_NOT_CONNECTED] = "Heat pump not connected",
    [STR_HP_BTN_TEMPS] = "\xEF\x81\xA8 Temps",
    [STR_HP_BTN_SYSTEM] = "\xEF\x80\x8B System",
    [STR_HP_BTN_ADVANCED] = "\xEF\x8C\x84 Control",
    [STR_HP_BTN_STATUS] = "\xEF\x80\x8B Status",
    [STR_NAV_HOME] = "Home",
    
    // Heat Pump - Mode Names
    [STR_HP_MODE_COOLING] = "COOLING",
    [STR_HP_MODE_HEATING] = "HEATING",
    [STR_HP_MODE_FLOOR_HEAT] = "FLOOR HEAT",
    [STR_HP_MODE_FAN_HEAT] = "FAN HEAT",
    [STR_HP_MODE_HOT_WATER] = "HOT WATER",
    [STR_HP_MODE_AUTO] = "AUTO",
    [STR_HP_MODE_DEFROST] = "DEFROST",
    [STR_HP_MODE_UNKNOWN] = "UNKNOWN",
    
    // Heat Pump - Mode Dropdown Options
    [STR_HP_OPT_COOLING] = "Cooling",
    [STR_HP_OPT_FLOOR_HEATING] = "Floor Heating",
    [STR_HP_OPT_FAN_COIL_HEATING] = "Fan Coil Heating",
    [STR_HP_OPT_HOT_WATER] = "Hot Water",
    [STR_HP_OPT_AUTO] = "Auto",
    
    // Heat Pump - Component Labels
    [STR_HP_COMPRESSOR] = "Compressor",
    [STR_HP_FAN] = "Fan",
    [STR_HP_PUMP] = "Pump",
    [STR_HP_AUX_HEAT] = "Aux Heat",
    
    // Heat Pump - Fan Speeds
    [STR_HP_FAN_LOW] = "Low",
    [STR_HP_FAN_MED] = "Med",
    [STR_HP_FAN_HIGH] = "High",
    
    // Heat Pump - Setpoint Names
    [STR_HP_COOLING] = "Cooling",
    [STR_HP_HEATING] = "Heating",
    [STR_HP_HOT_WATER] = "Hot Water",
    
    // Heat Pump - Temperatures Screen
    [STR_HP_TEMPERATURES] = "Temperatures",
    [STR_HP_DEMO_TEMPERATURES] = "DEMO MODE - Temperatures",
    [STR_HP_WATER_TANK] = "Water Tank",
    [STR_HP_WATER_OUTLET] = "Water Outlet",
    [STR_HP_WATER_INLET] = "Water Inlet",
    [STR_HP_OUTDOOR_AMBIENT] = "Outdoor Ambient",
    [STR_HP_DISCHARGE] = "Discharge",
    [STR_HP_SUCTION] = "Suction",
    [STR_HP_OUTDOOR_COIL] = "Outdoor Coil",
    [STR_HP_INDOOR_COIL] = "Indoor Coil",
    [STR_HP_IPM_MODULE] = "IPM Module",
    
    // Heat Pump - System Screen
    [STR_HP_SYSTEM_READINGS] = "System Readings",
    [STR_HP_STATUS] = "Status",
    [STR_HP_SYSTEM_SECTION] = "System",
    [STR_HP_DEMO_SYSTEM] = "DEMO MODE - System",
    [STR_HP_FREQUENCY] = "Frequency",
    [STR_HP_FAN_SPEED] = "Fan Speed",
    [STR_HP_ELECTRICAL] = "Electrical",
    [STR_HP_AC_VOLTAGE] = "AC Voltage",
    [STR_HP_AC_CURRENT] = "AC Current",
    [STR_HP_DC_VOLTAGE] = "DC Voltage",
    [STR_HP_DC_CURRENT] = "DC Current",
    [STR_HP_PRESSURES] = "Pressures",
    [STR_HP_HIGH_PRESSURE] = "High Pressure",
    [STR_HP_LOW_PRESSURE] = "Low Pressure",
    [STR_HP_EXPANSION_VALVES] = "Expansion Valves",
    [STR_HP_PRIMARY_EEV] = "Primary EEV",
    [STR_HP_SECONDARY_EEV] = "Secondary EEV",
    [STR_HP_SETPOINTS] = "Setpoints",
    
    // Heat Pump - Errors Screen
    [STR_HP_ERROR_STATUS] = "Error Status",
    [STR_HP_DEMO_ERRORS] = "DEMO MODE - Errors",
    [STR_HP_RESOLUTION] = "\xEF\x80\x93 Resolution:",
    [STR_HP_CONTACT_DEALER] = "Contact the dealer.",
    [STR_HP_STARTED] = "Started:",
    [STR_HP_ACTIVE_FOR] = "Active for",
    [STR_HP_DURATION] = "Duration:",
    [STR_HP_JUST_DETECTED] = "\xEF\x81\xB1 Just detected",
    [STR_HP_DISCONNECTED_MSG] = "\xEF\x81\xB1 Heat Pump Disconnected\n\nError status unavailable.\nCheck Modbus connection.",
    [STR_HP_NO_ERRORS] = "\xEF\x80\x8C No Active Errors\n\nAll systems operating normally.",
    [STR_HP_SYSTEM_OK] = "\xEF\x80\x8C No active errors",
    [STR_HP_ACTIVE_ERRORS] = "Active Errors",
    [STR_HP_CLEAR_HISTORY] = "Clear",
    [STR_HP_ERROR_HISTORY] = "Error History",
    
    // Heat Pump - Control Screen
    [STR_HP_ADVANCED] = "Control",
    [STR_HP_DEMO_ADVANCED] = "DEMO MODE - Control",
    [STR_HP_EDIT_PARAMETER] = "Edit Parameter",
    [STR_HP_RANGE_FMT] = "Range:",
    [STR_HP_KRATIO_REDUCE] = "Lowers running frequency by %d steps per %d Hz",
    [STR_HP_KRATIO_NONE] = "No change to running frequency",
    [STR_HP_COOLING_SETPOINT] = "Cooling Setpoint",
    [STR_HP_HEATING_SETPOINT] = "Heating Setpoint",
    [STR_HP_HOT_WATER_SETPOINT] = "Hot Water Setpoint",
    [STR_HP_CANNOT_SAVE] = "Cannot save setting: Heat pump not connected",
    [STR_HP_CANNOT_SAVE_SETPOINT] = "Cannot save setpoint: Heat pump not connected",
    [STR_HP_CAT_EEV] = "EEV",
    [STR_HP_CAT_DEFROST] = "Defrost",
    [STR_HP_CAT_PROTECTION] = "Protection",
    [STR_HP_CAT_AUTO_MODE] = "Auto Mode",
    [STR_HP_CAT_PUMP_VALVE] = "Pump & Valve",
    // P-parameter names
    [STR_HP_PARAM_EEV_OPENING] = "EEV Opening",
    [STR_HP_PARAM_EEV_MODE] = "EEV Mode",
    [STR_HP_PARAM_TARGET_SUPERHEAT] = "Target Superheat",
    [STR_HP_PARAM_DEFROST_CYCLE] = "Defrost Cycle",
    [STR_HP_PARAM_DEFROST_ENTER_TEMP] = "Defrost Enter Temp",
    [STR_HP_PARAM_DEFROST_EXTEND_TEMP] = "Defrost Extend Temp",
    [STR_HP_PARAM_DEFROST_TEMP_DIFF] = "Defrost Temp Diff",
    [STR_HP_PARAM_DEFROST_EXTEND_TIME] = "Defrost Extend Time",
    [STR_HP_PARAM_MAX_DEFROST_TIME] = "Max Defrost Time",
    [STR_HP_PARAM_DEFROST_EXIT_TEMP] = "Defrost Exit Temp",
    [STR_HP_PARAM_LOW_AMBIENT_PROTECT] = "Low Ambient Protect",
    [STR_HP_PARAM_FREQ_REDUCTION] = "Freq Reduction",
    [STR_HP_PARAM_COOLING_LOW_AMBIENT] = "Cooling Low Ambient",
    [STR_HP_PARAM_MAX_SETTING_TEMP] = "Max Setting Temp",
    [STR_HP_PARAM_COOLING_AUTO_TEMP] = "Cooling Auto Temp",
    [STR_HP_PARAM_HEATING_AUTO_TEMP] = "Heating Auto Temp",
    [STR_HP_PARAM_MODE_SWITCH_DELAY] = "Mode Switch Delay",
    [STR_HP_PARAM_STERILIZE_TIME] = "Sterilize Time",
    [STR_HP_PARAM_WATER_RETURN_TEMP] = "Water Return Temp",
    [STR_HP_PARAM_WATER_RETURN_TIME] = "Water Return Time",
    [STR_HP_PARAM_3WAY_VALVE_TIME] = "3-Way Valve Time",
    [STR_HP_PARAM_PUMP_MODE] = "Pump Mode",
    [STR_HP_PARAM_PUMP_INTERVAL] = "Pump Interval",
    [STR_HP_PARAM_PUMP_LOW_AMBIENT] = "Pump Low Ambient",
    [STR_HP_PARAM_WATERWAY_CLEAN] = "Waterway Clean",

    // Heat Pump - Dashboard Labels
    [STR_HP_COMP_RUNNING] = "RUNNING",
    [STR_HP_COMP_IDLE] = "IDLE",
    [STR_HP_STANDBY_DEMAND] = "Standby \xe2\x80\x94 waiting for demand",
    [STR_HP_DELTA_ABOVE_FMT] = "+%d\xc2\xb0 above set",
    [STR_HP_DELTA_BELOW_FMT] = "%d\xc2\xb0 below set",
    [STR_HP_DELTA_AT] = "At setpoint",
    [STR_HP_LABEL_INLET] = "INLET",
    [STR_HP_LABEL_OUTLET] = "OUTLET",
    [STR_HP_LABEL_OUTDOOR] = "OUTDOOR",
    [STR_HP_LABEL_COIL] = "COIL",
    [STR_HP_STATE_FAULT] = "FAULT",
    [STR_HP_ENERGY] = "Energy",
    [STR_HP_LABEL_COP] = "COP",
    [STR_HP_LABEL_POWER] = "POWER",
    [STR_HP_LABEL_FAN] = "FAN",
    [STR_HP_LABEL_DISCHARGE] = "DISCHARGE",
    [STR_HP_LABEL_SUCTION] = "SUCTION",
    [STR_HP_LABEL_EEV] = "EEV",
    [STR_HP_LABEL_HI_PRESS] = "HIGH PRESS",
    [STR_HP_LABEL_LO_PRESS] = "LOW PRESS",
    [STR_HP_LABEL_POWER_IN] = "POWER IN",
    [STR_HP_LABEL_HEAT_OUT] = "HEAT OUT",
    [STR_HP_LABEL_COOLING_OUT] = "COOLING OUT",

    // Event Log
    [STR_EVENT_LOG] = "Events",
    [STR_EVENT_SYSTEM_START] = "System Start",
    [STR_EVENT_POWER_ON] = "Power ON",
    [STR_EVENT_POWER_OFF] = "Power OFF",
    [STR_EVENT_MODE_CHANGED] = "Mode Changed",
    [STR_EVENT_SETPOINT_CHANGED] = "Setpoint Changed",
    [STR_EVENT_COMPRESSOR_ON] = "Compressor ON",
    [STR_EVENT_COMPRESSOR_OFF] = "Compressor OFF",
    [STR_EVENT_FAN_ON] = "Fan ON",
    [STR_EVENT_FAN_OFF] = "Fan OFF",
    [STR_EVENT_PUMP_ON] = "Water Pump ON",
    [STR_EVENT_PUMP_OFF] = "Water Pump OFF",
    [STR_EVENT_AUX_HEATER_ON] = "Aux Heater ON",
    [STR_EVENT_AUX_HEATER_OFF] = "Aux Heater OFF",
    [STR_EVENT_DEFROST_START] = "Defrost Start",
    [STR_EVENT_DEFROST_END] = "Defrost End",
    [STR_EVENT_ERROR_APPEARED] = "Error Appeared",
    [STR_EVENT_ERROR_CLEARED] = "Error Cleared",
    [STR_EVENT_CONNECTED] = "Connected",
    [STR_EVENT_DISCONNECTED] = "Disconnected",
    [STR_EVENT_BROWNOUT_RESET] = "Brownout Reset",
    [STR_EVENT_APPLICATION_CRASH] = "Application Crash",
    [STR_EVENT_WATCHDOG_RESET] = "Watchdog Reset",
    [STR_EVENT_WATCHDOG_INTERRUPT] = "Interrupt watchdog",
    [STR_EVENT_WATCHDOG_TASK] = "Task watchdog",
    [STR_EVENT_WATCHDOG_OTHER] = "System watchdog",
    [STR_EVENT_CLEAR] = "Clear Events",
    [STR_EVENT_NO_EVENTS] = "No events recorded",
    [STR_EVENT_SHOW_OLDER] = "Show older events",
    [STR_EVENT_TODAY] = "Today",
    [STR_EVENT_YESTERDAY] = "Yesterday",
    [STR_EVENT_SINCE_RESTART] = "Since restart",
    [STR_EVENT_SEARCH] = "Search events...",
    [STR_EVENT_FILTERS] = "Filters",
    [STR_EVENT_PROBLEMS] = "Problems",
    [STR_EVENT_EQUIPMENT] = "Equipment",
    [STR_EVENT_CHANGES] = "Changes",
    [STR_EVENT_SYSTEM] = "System",
    [STR_EVENT_RESULTS_FMT] = "%d of %d events",
    [STR_EVENT_NO_MATCHES] = "No matching events",
    [STR_EVENT_NEW_MATCHES] = "New matching events",
    [STR_EVENT_TIME_ALL] = "All time",
    [STR_EVENT_TIME_TODAY] = "Today",
    [STR_EVENT_TIME_24_HOURS] = "Last 24 hours",
    [STR_EVENT_TIME_7_DAYS] = "Last 7 days",
    [STR_EVENT_TIME_SINCE_RESTART] = "Since restart",
    [STR_EVENT_RESET_FILTERS] = "Reset filters",
    [STR_EVENT_APPLY_SEARCH] = "Search",
    [STR_EVENT_APPLY_FILTERS] = "Apply",
    [STR_EVENT_CLEAR_CONFIRM_TITLE] = "Clear event history?",
    [STR_EVENT_CLEAR_CONFIRM_TEXT] = "This permanently removes all recorded events.",
    [STR_EVENT_MONTH_JAN] = "Jan",
    [STR_EVENT_MONTH_FEB] = "Feb",
    [STR_EVENT_MONTH_MAR] = "Mar",
    [STR_EVENT_MONTH_APR] = "Apr",
    [STR_EVENT_MONTH_MAY] = "May",
    [STR_EVENT_MONTH_JUN] = "Jun",
    [STR_EVENT_MONTH_JUL] = "Jul",
    [STR_EVENT_MONTH_AUG] = "Aug",
    [STR_EVENT_MONTH_SEP] = "Sep",
    [STR_EVENT_MONTH_OCT] = "Oct",
    [STR_EVENT_MONTH_NOV] = "Nov",
    [STR_EVENT_MONTH_DEC] = "Dec",

    // Reboot confirmation
    [STR_DEMO_MODE_CHANGED] = "Demo mode changed.",
    [STR_RESTART_REQUIRED] = "Restart required to take effect.",
    [STR_RESTART] = "Restart",
    [STR_FACTORY_RESET_TITLE] = "Factory reset this controller?",
    [STR_FACTORY_RESET_DESCRIPTION] = "This permanently erases WiFi, settings, certificates, event history, and stored files.",
    [STR_FACTORY_RESET_WARNING] = "This action cannot be undone.",
    [STR_FACTORY_RESET_CONFIRM] = "Erase & Reset",
    [STR_FACTORY_RESET_ERASING] = "Erasing...",
    [STR_FACTORY_RESET_START_FAILED] = "Unable to start reset. Please try again.",
};

// French strings
static const char* strings_fr[STR_COUNT] = {
    // General / Common
    [STR_OK] = "OK",
    [STR_CANCEL] = "Annuler",
    [STR_ERROR] = "Erreur",
    [STR_CLOSE] = "Fermer",
    [STR_SAVE] = "Enregistrer",
    [STR_BACK] = "Retour",
    [STR_LOADING] = "Chargement...",
    [STR_PLEASE_WAIT] = "Veuillez patienter...",
    
    // Settings Screen
    [STR_SETTINGS] = "Paramètres",
    [STR_SETTINGS_WIFI] = "WiFi",
    [STR_SETTINGS_UPDATE] = "Mise à jour",
    [STR_SETTINGS_LANGUAGE] = "Langue",
    [STR_SETTINGS_DEMO_MODE] = "Mode d\xc3\xa9mo",
    [STR_SETTINGS_TEMPERATURE] = "Temp\xc3\xa9rature",
    [STR_SETTINGS_FACTORY_RESET] = "Réinitialisation",
    
    // WiFi Panel
    [STR_WIFI_CONNECTED] = "Connecté",
    [STR_WIFI_DISCONNECTED] = "Déconnecté",
    [STR_WIFI_DISCONNECT] = "Déconnecter",
    [STR_WIFI_CONNECT] = "Connecter",
    [STR_WIFI_CONNECTING] = "Connexion",
    [STR_WIFI_SCANNING] = "Recherche de réseaux...",
    [STR_WIFI_AVAILABLE_NETWORKS] = "Réseaux disponibles",
    [STR_WIFI_NO_NETWORKS] = "Aucun réseau trouvé",
    [STR_WIFI_ENTER_PASSWORD] = "Entrer le mot de passe",
    [STR_WIFI_PASSWORD] = "Mot de passe",
    [STR_WIFI_NETWORK] = "Réseau",
    [STR_WIFI_IP_ADDRESS] = "IP",
    [STR_WIFI_SIGNAL_EXCELLENT] = "Signal : Excellent",
    [STR_WIFI_SIGNAL_GOOD] = "Signal : Bon",
    [STR_WIFI_SIGNAL_FAIR] = "Signal : Moyen",
    [STR_WIFI_SIGNAL_WEAK] = "Signal : Faible",
    [STR_WIFI_FAILED_INIT] = "Échec de l'initialisation WiFi.\nVérifiez le module ESP32-C6.",
    [STR_WIFI_FAILED_SCAN] = "Échec de la recherche.\nVeuillez réessayer.",
    [STR_WIFI_FAILED_CONNECT] = "Échec de connexion.\nVérifiez le mot de passe.",
    
    // Firmware Update Panel
    [STR_FW_TITLE] = "Mise à jour du firmware",
    [STR_FW_CURRENT] = "Actuelle",
    [STR_FW_LATEST] = "Dernière",
    [STR_FW_CHECKING] = "Vérification des mises à jour...",
    [STR_FW_UP_TO_DATE] = "Vous êtes à jour !",
    [STR_FW_UPDATE_AVAILABLE] = "Mise à jour disponible !",
    [STR_FW_DOWNLOADING] = "Téléchargement...",
    [STR_FW_VERIFYING] = "Vérification du firmware...",
    [STR_FW_INSTALL_UPDATE] = "Installer",
    [STR_FW_REBOOTING] = "Redémarrage dans 3 secondes...",
    [STR_FW_UPDATE_COMPLETE] = "Mise à jour terminée ! Redémarrage...",
    [STR_FW_UPDATE_FAILED] = "Échec de la mise à jour",
    [STR_FW_CHECK_FAILED] = "Échec de la vérification",
    
    // Language Panel
    [STR_LANG_TITLE] = "Langue",
    [STR_LANG_ENGLISH] = "Anglais",
    [STR_LANG_FRENCH] = "Français",
    [STR_LANG_SPANISH] = "Espagnol",
    [STR_LANG_SELECT] = "Sélectionner la langue",
    [STR_LANG_CURRENT] = "Actuelle",
    [STR_LANG_RESTART_REQUIRED] = "Un redémarrage peut être nécessaire",
    
    // Status Bar / Notifications
    [STR_NOTIFY_UPDATE_AVAILABLE] = "Mise à jour disponible",
    [STR_NOTIFY_WIFI_UNSTABLE] = "Connexion WiFi instable",
    [STR_NOTIFY_LOW_BATTERY] = "Batterie faible",
    
    // Time Panel
    [STR_TIME_TITLE] = "Date et heure",
    [STR_TIME_DATE] = "Date",
    [STR_TIME_TIMEZONE] = "Fuseau horaire",
    [STR_TIME_FORMAT_12H] = "12h",
    [STR_TIME_FORMAT_24H] = "24h",
    [STR_TIME_DISPLAY_FORMAT] = "Format d'affichage",
    [STR_TIME_FORMAT_INFO] = "Choisir l'affichage\nde l'heure",
    [STR_TIME_SYNCED] = "Synchronisé",
    [STR_TIME_NOT_SYNCED] = "Non synchronisé",
    [STR_SETTINGS_TIME] = "Heure",
    
    // Display Panel
    [STR_SETTINGS_DISPLAY] = "Affichage",
    [STR_DISPLAY_TITLE] = "Paramètres d'affichage",
    [STR_DISPLAY_BRIGHTNESS] = "Luminosité",
    [STR_DISPLAY_BRIGHTNESS_LOW] = "Faible",
    [STR_DISPLAY_BRIGHTNESS_HIGH] = "Élevée",
    
    // Heat Pump - Main Screen
    [STR_HP_COMMUNICATION_ERROR] = "Erreur de communication",
    [STR_HP_DISCONNECTED] = "DÉCONNECTÉ",
    [STR_HP_STANDBY] = "EN VEILLE",
    [STR_HP_TANK_TEMPERATURE] = "Température du ballon",
    [STR_HP_SETPOINT] = "Consigne",
    [STR_HP_MODE] = "Mode",
    [STR_HP_POWER_ON] = "\xEF\x80\x91 EN MARCHE",
    [STR_HP_POWER_OFF] = "\xEF\x80\x91 À L'ARRÊT",
    [STR_HP_POWER_UNAVAILABLE] = "INDISPONIBLE",
    [STR_HP_HOLD_POWER_OFF] = "\xEF\x80\x91 Arrêt dans %d...",
    [STR_HP_RESERVED_1] = "",
    [STR_HP_RESERVED_2] = "",
    [STR_HP_DEMO_MODE_ENABLED] = "Mode démo activé",
    [STR_HP_NOT_CONNECTED] = "Pompe à chaleur non connectée",
    [STR_HP_BTN_TEMPS] = "\xEF\x81\xA8 Temp.",
    [STR_HP_BTN_SYSTEM] = "\xEF\x80\x8B Système",
    [STR_HP_BTN_ADVANCED] = "\xEF\x8C\x84 Contr\xc3\xb4le",
    [STR_HP_BTN_STATUS] = "\xEF\x80\x8B \xc3\x89tat",
    [STR_NAV_HOME] = "Accueil",
    
    // Heat Pump - Mode Names
    [STR_HP_MODE_COOLING] = "REFROIDISSEMENT",
    [STR_HP_MODE_HEATING] = "CHAUFFAGE",
    [STR_HP_MODE_FLOOR_HEAT] = "CHAUFF. SOL",
    [STR_HP_MODE_FAN_HEAT] = "VENTILO-CONV.",
    [STR_HP_MODE_HOT_WATER] = "EAU CHAUDE",
    [STR_HP_MODE_AUTO] = "AUTO",
    [STR_HP_MODE_DEFROST] = "DÉGIVRAGE",
    [STR_HP_MODE_UNKNOWN] = "INCONNU",
    
    // Heat Pump - Mode Dropdown Options
    [STR_HP_OPT_COOLING] = "Refroidissement",
    [STR_HP_OPT_FLOOR_HEATING] = "Chauffage au sol",
    [STR_HP_OPT_FAN_COIL_HEATING] = "Ventilo-convecteur",
    [STR_HP_OPT_HOT_WATER] = "Eau chaude sanitaire",
    [STR_HP_OPT_AUTO] = "Auto",
    
    // Heat Pump - Component Labels
    [STR_HP_COMPRESSOR] = "Compresseur",
    [STR_HP_FAN] = "Ventilateur",
    [STR_HP_PUMP] = "Pompe",
    [STR_HP_AUX_HEAT] = "Chauff. aux.",
    
    // Heat Pump - Fan Speeds
    [STR_HP_FAN_LOW] = "Basse",
    [STR_HP_FAN_MED] = "Moy.",
    [STR_HP_FAN_HIGH] = "Haute",
    
    // Heat Pump - Setpoint Names
    [STR_HP_COOLING] = "Refroidissement",
    [STR_HP_HEATING] = "Chauffage",
    [STR_HP_HOT_WATER] = "Eau chaude",
    
    // Heat Pump - Temperatures Screen
    [STR_HP_TEMPERATURES] = "Températures",
    [STR_HP_DEMO_TEMPERATURES] = "DÉMO - Températures",
    [STR_HP_WATER_TANK] = "Ballon d'eau",
    [STR_HP_WATER_OUTLET] = "Sortie d'eau",
    [STR_HP_WATER_INLET] = "Entrée d'eau",
    [STR_HP_OUTDOOR_AMBIENT] = "Extérieur",
    [STR_HP_DISCHARGE] = "Refoulement",
    [STR_HP_SUCTION] = "Aspiration",
    [STR_HP_OUTDOOR_COIL] = "Échangeur ext.",
    [STR_HP_INDOOR_COIL] = "Échangeur int.",
    [STR_HP_IPM_MODULE] = "Module IPM",
    
    // Heat Pump - System Screen
    [STR_HP_SYSTEM_READINGS] = "Lecture système",
    [STR_HP_STATUS] = "\xc3\x89tat",
    [STR_HP_SYSTEM_SECTION] = "Syst\xc3\xa8me",
    [STR_HP_DEMO_SYSTEM] = "DÉMO - Système",
    [STR_HP_FREQUENCY] = "Fréquence",
    [STR_HP_FAN_SPEED] = "Vitesse ventilateur",
    [STR_HP_ELECTRICAL] = "Électrique",
    [STR_HP_AC_VOLTAGE] = "Tension CA",
    [STR_HP_AC_CURRENT] = "Courant CA",
    [STR_HP_DC_VOLTAGE] = "Tension CC",
    [STR_HP_DC_CURRENT] = "Courant CC",
    [STR_HP_PRESSURES] = "Pressions",
    [STR_HP_HIGH_PRESSURE] = "Haute pression",
    [STR_HP_LOW_PRESSURE] = "Basse pression",
    [STR_HP_EXPANSION_VALVES] = "Détendeurs",
    [STR_HP_PRIMARY_EEV] = "Détendeur primaire",
    [STR_HP_SECONDARY_EEV] = "Détendeur secondaire",
    [STR_HP_SETPOINTS] = "Consignes",
    
    // Heat Pump - Errors Screen
    [STR_HP_ERROR_STATUS] = "État des erreurs",
    [STR_HP_DEMO_ERRORS] = "DÉMO - Erreurs",
    [STR_HP_RESOLUTION] = "\xEF\x80\x93 Résolution :",
    [STR_HP_CONTACT_DEALER] = "Contactez le revendeur.",
    [STR_HP_STARTED] = "Début :",
    [STR_HP_ACTIVE_FOR] = "Active depuis",
    [STR_HP_DURATION] = "Durée :",
    [STR_HP_JUST_DETECTED] = "\xEF\x81\xB1 Détectée à l'instant",
    [STR_HP_DISCONNECTED_MSG] = "\xEF\x81\xB1 PAC déconnectée\n\nÉtat des erreurs indisponible.\nVérifiez la connexion Modbus.",
    [STR_HP_NO_ERRORS] = "\xEF\x80\x8C Aucune erreur\n\nTous les systèmes fonctionnent normalement.",
    [STR_HP_SYSTEM_OK] = "\xEF\x80\x8C Aucune erreur active",
    [STR_HP_ACTIVE_ERRORS] = "Erreurs actives",
    [STR_HP_CLEAR_HISTORY] = "Effacer",
    [STR_HP_ERROR_HISTORY] = "Historique des erreurs",
    
    // Heat Pump - Control Screen
    [STR_HP_ADVANCED] = "Contr\xc3\xb4le",
    [STR_HP_DEMO_ADVANCED] = "D\xc3\x89MO - Contr\xc3\xb4le",
    [STR_HP_EDIT_PARAMETER] = "Modifier le paramètre",
    [STR_HP_RANGE_FMT] = "Plage :",
    [STR_HP_KRATIO_REDUCE] = "Réduit la fréquence de fonctionnement de %d pas par %d Hz",
    [STR_HP_KRATIO_NONE] = "Aucune modification de la fréquence de fonctionnement",
    [STR_HP_COOLING_SETPOINT] = "Consigne refroidissement",
    [STR_HP_HEATING_SETPOINT] = "Consigne chauffage",
    [STR_HP_HOT_WATER_SETPOINT] = "Consigne eau chaude",
    [STR_HP_CANNOT_SAVE] = "Impossible d'enregistrer : PAC non connectée",
    [STR_HP_CANNOT_SAVE_SETPOINT] = "Impossible d'enregistrer la consigne : PAC non connectée",
    [STR_HP_CAT_EEV] = "Détendeur (EEV)",
    [STR_HP_CAT_DEFROST] = "Dégivrage",
    [STR_HP_CAT_PROTECTION] = "Protection",
    [STR_HP_CAT_AUTO_MODE] = "Mode auto",
    [STR_HP_CAT_PUMP_VALVE] = "Pompe & vanne",
    // P-parameter names
    [STR_HP_PARAM_EEV_OPENING] = "Ouverture EEV",
    [STR_HP_PARAM_EEV_MODE] = "Mode EEV",
    [STR_HP_PARAM_TARGET_SUPERHEAT] = "Surchauffe cible",
    [STR_HP_PARAM_DEFROST_CYCLE] = "Cycle dégivrage",
    [STR_HP_PARAM_DEFROST_ENTER_TEMP] = "Temp. entrée dégivrage",
    [STR_HP_PARAM_DEFROST_EXTEND_TEMP] = "Temp. extension dégivrage",
    [STR_HP_PARAM_DEFROST_TEMP_DIFF] = "Diff. temp. dégivrage",
    [STR_HP_PARAM_DEFROST_EXTEND_TIME] = "Durée extension dégivrage",
    [STR_HP_PARAM_MAX_DEFROST_TIME] = "Durée max dégivrage",
    [STR_HP_PARAM_DEFROST_EXIT_TEMP] = "Temp. sortie dégivrage",
    [STR_HP_PARAM_LOW_AMBIENT_PROTECT] = "Protect. basse temp.",
    [STR_HP_PARAM_FREQ_REDUCTION] = "Réduction fréquence",
    [STR_HP_PARAM_COOLING_LOW_AMBIENT] = "Refroid. basse temp.",
    [STR_HP_PARAM_MAX_SETTING_TEMP] = "Temp. réglage max",
    [STR_HP_PARAM_COOLING_AUTO_TEMP] = "Temp. auto refroid.",
    [STR_HP_PARAM_HEATING_AUTO_TEMP] = "Temp. auto chauffage",
    [STR_HP_PARAM_MODE_SWITCH_DELAY] = "Délai changement mode",
    [STR_HP_PARAM_STERILIZE_TIME] = "Durée stérilisation",
    [STR_HP_PARAM_WATER_RETURN_TEMP] = "Temp. retour eau",
    [STR_HP_PARAM_WATER_RETURN_TIME] = "Durée retour eau",
    [STR_HP_PARAM_3WAY_VALVE_TIME] = "Durée vanne 3 voies",
    [STR_HP_PARAM_PUMP_MODE] = "Mode pompe",
    [STR_HP_PARAM_PUMP_INTERVAL] = "Intervalle pompe",
    [STR_HP_PARAM_PUMP_LOW_AMBIENT] = "Pompe basse temp.",
    [STR_HP_PARAM_WATERWAY_CLEAN] = "Nettoyage circuit",

    // Heat Pump - Dashboard Labels
    [STR_HP_COMP_RUNNING] = "EN MARCHE",
    [STR_HP_COMP_IDLE] = "INACTIF",
    [STR_HP_STANDBY_DEMAND] = "Veille \xe2\x80\x94 en attente de demande",
    [STR_HP_DELTA_ABOVE_FMT] = "+%d\xc2\xb0 au-dessus",
    [STR_HP_DELTA_BELOW_FMT] = "%d\xc2\xb0 en dessous",
    [STR_HP_DELTA_AT] = "\xc3\x80 la consigne",
    [STR_HP_LABEL_INLET] = "ENTR" "\xc3\x89" "E",
    [STR_HP_LABEL_OUTLET] = "SORTIE",
    [STR_HP_LABEL_OUTDOOR] = "EXT" "\xc3\x89" "RIEUR",
    [STR_HP_LABEL_COIL] = "\xc3\x89" "CHANGEUR",
    [STR_HP_STATE_FAULT] = "PANNE",
    [STR_HP_ENERGY] = "\xc3\x89nergie",
    [STR_HP_LABEL_COP] = "COP",
    [STR_HP_LABEL_POWER] = "PUISSANCE",
    [STR_HP_LABEL_FAN] = "VENTIL.",
    [STR_HP_LABEL_DISCHARGE] = "REFOUL.",
    [STR_HP_LABEL_SUCTION] = "ASPIR.",
    [STR_HP_LABEL_EEV] = "EEV",
    [STR_HP_LABEL_HI_PRESS] = "HAUTE PR.",
    [STR_HP_LABEL_LO_PRESS] = "BASSE PR.",
    [STR_HP_LABEL_POWER_IN] = "PUISS. IN",
    [STR_HP_LABEL_HEAT_OUT] = "CHALEUR",
    [STR_HP_LABEL_COOLING_OUT] = "FROID",

    // Event Log
    [STR_EVENT_LOG] = "Événements",
    [STR_EVENT_SYSTEM_START] = "Démarrage système",
    [STR_EVENT_POWER_ON] = "Mise en marche",
    [STR_EVENT_POWER_OFF] = "Arrêt",
    [STR_EVENT_MODE_CHANGED] = "Mode changé",
    [STR_EVENT_SETPOINT_CHANGED] = "Consigne modifiée",
    [STR_EVENT_COMPRESSOR_ON] = "Compresseur ON",
    [STR_EVENT_COMPRESSOR_OFF] = "Compresseur OFF",
    [STR_EVENT_FAN_ON] = "Ventilateur ON",
    [STR_EVENT_FAN_OFF] = "Ventilateur OFF",
    [STR_EVENT_PUMP_ON] = "Pompe à eau ON",
    [STR_EVENT_PUMP_OFF] = "Pompe à eau OFF",
    [STR_EVENT_AUX_HEATER_ON] = "Chauff. aux ON",
    [STR_EVENT_AUX_HEATER_OFF] = "Chauff. aux OFF",
    [STR_EVENT_DEFROST_START] = "Début dégivrage",
    [STR_EVENT_DEFROST_END] = "Fin dégivrage",
    [STR_EVENT_ERROR_APPEARED] = "Erreur apparue",
    [STR_EVENT_ERROR_CLEARED] = "Erreur effacée",
    [STR_EVENT_CONNECTED] = "Connecté",
    [STR_EVENT_DISCONNECTED] = "Déconnecté",
    [STR_EVENT_BROWNOUT_RESET] = "Réinit. sous-tension",
    [STR_EVENT_APPLICATION_CRASH] = "Plantage de l'application",
    [STR_EVENT_WATCHDOG_RESET] = "Réinit. par surveillance",
    [STR_EVENT_WATCHDOG_INTERRUPT] = "Surveillance d'interruption",
    [STR_EVENT_WATCHDOG_TASK] = "Surveillance de tâche",
    [STR_EVENT_WATCHDOG_OTHER] = "Surveillance système",
    [STR_EVENT_CLEAR] = "Effacer événements",
    [STR_EVENT_NO_EVENTS] = "Aucun événement",
    [STR_EVENT_SHOW_OLDER] = "Voir événements plus anciens",
    [STR_EVENT_TODAY] = "Aujourd'hui",
    [STR_EVENT_YESTERDAY] = "Hier",
    [STR_EVENT_SINCE_RESTART] = "Depuis le redémarrage",
    [STR_EVENT_SEARCH] = "Rechercher des événements...",
    [STR_EVENT_FILTERS] = "Filtres",
    [STR_EVENT_PROBLEMS] = "Problèmes",
    [STR_EVENT_EQUIPMENT] = "Équipement",
    [STR_EVENT_CHANGES] = "Modifications",
    [STR_EVENT_SYSTEM] = "Système",
    [STR_EVENT_RESULTS_FMT] = "%d événements sur %d",
    [STR_EVENT_NO_MATCHES] = "Aucun événement correspondant",
    [STR_EVENT_NEW_MATCHES] = "Nouveaux événements",
    [STR_EVENT_TIME_ALL] = "Toute la période",
    [STR_EVENT_TIME_TODAY] = "Aujourd'hui",
    [STR_EVENT_TIME_24_HOURS] = "Dernières 24 heures",
    [STR_EVENT_TIME_7_DAYS] = "7 derniers jours",
    [STR_EVENT_TIME_SINCE_RESTART] = "Depuis le redémarrage",
    [STR_EVENT_RESET_FILTERS] = "Réinitialiser les filtres",
    [STR_EVENT_APPLY_SEARCH] = "Rechercher",
    [STR_EVENT_APPLY_FILTERS] = "Appliquer",
    [STR_EVENT_CLEAR_CONFIRM_TITLE] = "Effacer l'historique ?",
    [STR_EVENT_CLEAR_CONFIRM_TEXT] = "Tous les événements enregistrés seront supprimés.",
    [STR_EVENT_MONTH_JAN] = "janv.",
    [STR_EVENT_MONTH_FEB] = "févr.",
    [STR_EVENT_MONTH_MAR] = "mars",
    [STR_EVENT_MONTH_APR] = "avr.",
    [STR_EVENT_MONTH_MAY] = "mai",
    [STR_EVENT_MONTH_JUN] = "juin",
    [STR_EVENT_MONTH_JUL] = "juil.",
    [STR_EVENT_MONTH_AUG] = "août",
    [STR_EVENT_MONTH_SEP] = "sept.",
    [STR_EVENT_MONTH_OCT] = "oct.",
    [STR_EVENT_MONTH_NOV] = "nov.",
    [STR_EVENT_MONTH_DEC] = "déc.",

    // Reboot confirmation
    [STR_DEMO_MODE_CHANGED] = "Mode démo modifié.",
    [STR_RESTART_REQUIRED] = "Redémarrage nécessaire.",
    [STR_RESTART] = "Redémarrer",
    [STR_FACTORY_RESET_TITLE] = "Réinitialiser ce contrôleur ?",
    [STR_FACTORY_RESET_DESCRIPTION] = "Cette action efface définitivement le Wi-Fi, les réglages, les certificats, l'historique des événements et les fichiers stockés.",
    [STR_FACTORY_RESET_WARNING] = "Cette action est irréversible.",
    [STR_FACTORY_RESET_CONFIRM] = "Effacer et réinitialiser",
    [STR_FACTORY_RESET_ERASING] = "Effacement...",
    [STR_FACTORY_RESET_START_FAILED] = "Impossible de démarrer la réinitialisation. Réessayez.",
};

// Spanish strings
static const char* strings_es[STR_COUNT] = {
    // General / Common
    [STR_OK] = "OK",
    [STR_CANCEL] = "Cancelar",
    [STR_ERROR] = "Error",
    [STR_CLOSE] = "Cerrar",
    [STR_SAVE] = "Guardar",
    [STR_BACK] = "Volver",
    [STR_LOADING] = "Cargando...",
    [STR_PLEASE_WAIT] = "Por favor espere...",
    
    // Settings Screen
    [STR_SETTINGS] = "Ajustes",
    [STR_SETTINGS_WIFI] = "WiFi",
    [STR_SETTINGS_UPDATE] = "Actualizar",
    [STR_SETTINGS_LANGUAGE] = "Idioma",
    [STR_SETTINGS_DEMO_MODE] = "Modo demo",
    [STR_SETTINGS_TEMPERATURE] = "Temperatura",
    [STR_SETTINGS_FACTORY_RESET] = "Restablecer fábrica",
    
    // WiFi Panel
    [STR_WIFI_CONNECTED] = "Conectado",
    [STR_WIFI_DISCONNECTED] = "Desconectado",
    [STR_WIFI_DISCONNECT] = "Desconectar",
    [STR_WIFI_CONNECT] = "Conectar",
    [STR_WIFI_CONNECTING] = "Conectando",
    [STR_WIFI_SCANNING] = "Buscando redes...",
    [STR_WIFI_AVAILABLE_NETWORKS] = "Redes disponibles",
    [STR_WIFI_NO_NETWORKS] = "No se encontraron redes",
    [STR_WIFI_ENTER_PASSWORD] = "Introducir contraseña",
    [STR_WIFI_PASSWORD] = "Contraseña",
    [STR_WIFI_NETWORK] = "Red",
    [STR_WIFI_IP_ADDRESS] = "IP",
    [STR_WIFI_SIGNAL_EXCELLENT] = "Señal: Excelente",
    [STR_WIFI_SIGNAL_GOOD] = "Señal: Buena",
    [STR_WIFI_SIGNAL_FAIR] = "Señal: Regular",
    [STR_WIFI_SIGNAL_WEAK] = "Señal: Débil",
    [STR_WIFI_FAILED_INIT] = "Error al inicializar WiFi.\nVerifique el módulo ESP32-C6.",
    [STR_WIFI_FAILED_SCAN] = "Error al buscar redes.\nIntente de nuevo.",
    [STR_WIFI_FAILED_CONNECT] = "Error al conectar.\nVerifique la contraseña.",
    
    // Firmware Update Panel
    [STR_FW_TITLE] = "Actualización de firmware",
    [STR_FW_CURRENT] = "Actual",
    [STR_FW_LATEST] = "Última",
    [STR_FW_CHECKING] = "Buscando actualizaciones...",
    [STR_FW_UP_TO_DATE] = "¡Está actualizado!",
    [STR_FW_UPDATE_AVAILABLE] = "¡Actualización disponible!",
    [STR_FW_DOWNLOADING] = "Descargando...",
    [STR_FW_VERIFYING] = "Verificando firmware...",
    [STR_FW_INSTALL_UPDATE] = "Instalar",
    [STR_FW_REBOOTING] = "Reiniciando en 3 segundos...",
    [STR_FW_UPDATE_COMPLETE] = "¡Actualización completa! Reiniciando...",
    [STR_FW_UPDATE_FAILED] = "Error en la actualización",
    [STR_FW_CHECK_FAILED] = "Error al verificar",
    
    // Language Panel
    [STR_LANG_TITLE] = "Idioma",
    [STR_LANG_ENGLISH] = "Inglés",
    [STR_LANG_FRENCH] = "Francés",
    [STR_LANG_SPANISH] = "Español",
    [STR_LANG_SELECT] = "Seleccionar idioma",
    [STR_LANG_CURRENT] = "Actual",
    [STR_LANG_RESTART_REQUIRED] = "Puede ser necesario reiniciar",
    
    // Status Bar / Notifications
    [STR_NOTIFY_UPDATE_AVAILABLE] = "Actualización disponible",
    [STR_NOTIFY_WIFI_UNSTABLE] = "Conexión WiFi inestable",
    [STR_NOTIFY_LOW_BATTERY] = "Batería baja",
    
    // Time Panel
    [STR_TIME_TITLE] = "Fecha y hora",
    [STR_TIME_DATE] = "Fecha",
    [STR_TIME_TIMEZONE] = "Zona horaria",
    [STR_TIME_FORMAT_12H] = "12h",
    [STR_TIME_FORMAT_24H] = "24h",
    [STR_TIME_DISPLAY_FORMAT] = "Formato de visualización",
    [STR_TIME_FORMAT_INFO] = "Elige cómo se muestra\nla hora en la interfaz",
    [STR_TIME_SYNCED] = "Sincronizado",
    [STR_TIME_NOT_SYNCED] = "No sincronizado",
    [STR_SETTINGS_TIME] = "Hora",
    
    // Display Panel
    [STR_SETTINGS_DISPLAY] = "Pantalla",
    [STR_DISPLAY_TITLE] = "Ajustes de pantalla",
    [STR_DISPLAY_BRIGHTNESS] = "Brillo",
    [STR_DISPLAY_BRIGHTNESS_LOW] = "Bajo",
    [STR_DISPLAY_BRIGHTNESS_HIGH] = "Alto",
    
    // Heat Pump - Main Screen
    [STR_HP_COMMUNICATION_ERROR] = "Error de comunicación",
    [STR_HP_DISCONNECTED] = "DESCONECTADO",
    [STR_HP_STANDBY] = "EN ESPERA",
    [STR_HP_TANK_TEMPERATURE] = "Temperatura del tanque",
    [STR_HP_SETPOINT] = "Consigna",
    [STR_HP_MODE] = "Modo",
    [STR_HP_POWER_ON] = "\xEF\x80\x91 ENCENDIDO",
    [STR_HP_POWER_OFF] = "\xEF\x80\x91 APAGADO",
    [STR_HP_POWER_UNAVAILABLE] = "NO DISPONIBLE",
    [STR_HP_HOLD_POWER_OFF] = "\xEF\x80\x91 Apagando en %d...",
    [STR_HP_RESERVED_1] = "",
    [STR_HP_RESERVED_2] = "",
    [STR_HP_DEMO_MODE_ENABLED] = "Modo demo activado",
    [STR_HP_NOT_CONNECTED] = "Bomba de calor no conectada",
    [STR_HP_BTN_TEMPS] = "\xEF\x81\xA8 Temp.",
    [STR_HP_BTN_SYSTEM] = "\xEF\x80\x8B Sistema",
    [STR_HP_BTN_ADVANCED] = "\xEF\x8C\x84 Control",
    [STR_HP_BTN_STATUS] = "\xEF\x80\x8B Estado",
    [STR_NAV_HOME] = "Inicio",
    
    // Heat Pump - Mode Names
    [STR_HP_MODE_COOLING] = "ENFRIAMIENTO",
    [STR_HP_MODE_HEATING] = "CALEFACCIÓN",
    [STR_HP_MODE_FLOOR_HEAT] = "CALEF. SUELO",
    [STR_HP_MODE_FAN_HEAT] = "FAN COIL",
    [STR_HP_MODE_HOT_WATER] = "AGUA CALIENTE",
    [STR_HP_MODE_AUTO] = "AUTO",
    [STR_HP_MODE_DEFROST] = "DESCONGELACIÓN",
    [STR_HP_MODE_UNKNOWN] = "DESCONOCIDO",
    
    // Heat Pump - Mode Dropdown Options
    [STR_HP_OPT_COOLING] = "Enfriamiento",
    [STR_HP_OPT_FLOOR_HEATING] = "Calefacción por suelo",
    [STR_HP_OPT_FAN_COIL_HEATING] = "Fan coil",
    [STR_HP_OPT_HOT_WATER] = "Agua caliente sanitaria",
    [STR_HP_OPT_AUTO] = "Auto",
    
    // Heat Pump - Component Labels
    [STR_HP_COMPRESSOR] = "Compresor",
    [STR_HP_FAN] = "Ventilador",
    [STR_HP_PUMP] = "Bomba",
    [STR_HP_AUX_HEAT] = "Calef. aux.",
    
    // Heat Pump - Fan Speeds
    [STR_HP_FAN_LOW] = "Baja",
    [STR_HP_FAN_MED] = "Media",
    [STR_HP_FAN_HIGH] = "Alta",
    
    // Heat Pump - Setpoint Names
    [STR_HP_COOLING] = "Enfriamiento",
    [STR_HP_HEATING] = "Calefacción",
    [STR_HP_HOT_WATER] = "Agua caliente",
    
    // Heat Pump - Temperatures Screen
    [STR_HP_TEMPERATURES] = "Temperaturas",
    [STR_HP_DEMO_TEMPERATURES] = "DEMO - Temperaturas",
    [STR_HP_WATER_TANK] = "Tanque de agua",
    [STR_HP_WATER_OUTLET] = "Salida de agua",
    [STR_HP_WATER_INLET] = "Entrada de agua",
    [STR_HP_OUTDOOR_AMBIENT] = "Exterior",
    [STR_HP_DISCHARGE] = "Descarga",
    [STR_HP_SUCTION] = "Succión",
    [STR_HP_OUTDOOR_COIL] = "Intercambiador ext.",
    [STR_HP_INDOOR_COIL] = "Intercambiador int.",
    [STR_HP_IPM_MODULE] = "Módulo IPM",
    
    // Heat Pump - System Screen
    [STR_HP_SYSTEM_READINGS] = "Lecturas del sistema",
    [STR_HP_STATUS] = "Estado",
    [STR_HP_SYSTEM_SECTION] = "Sistema",
    [STR_HP_DEMO_SYSTEM] = "DEMO - Sistema",
    [STR_HP_FREQUENCY] = "Frecuencia",
    [STR_HP_FAN_SPEED] = "Velocidad ventilador",
    [STR_HP_ELECTRICAL] = "Eléctrico",
    [STR_HP_AC_VOLTAGE] = "Tensión CA",
    [STR_HP_AC_CURRENT] = "Corriente CA",
    [STR_HP_DC_VOLTAGE] = "Tensión CC",
    [STR_HP_DC_CURRENT] = "Corriente CC",
    [STR_HP_PRESSURES] = "Presiones",
    [STR_HP_HIGH_PRESSURE] = "Alta presión",
    [STR_HP_LOW_PRESSURE] = "Baja presión",
    [STR_HP_EXPANSION_VALVES] = "Válvulas de expansión",
    [STR_HP_PRIMARY_EEV] = "VEE primaria",
    [STR_HP_SECONDARY_EEV] = "VEE secundaria",
    [STR_HP_SETPOINTS] = "Consignas",
    
    // Heat Pump - Errors Screen
    [STR_HP_ERROR_STATUS] = "Estado de errores",
    [STR_HP_DEMO_ERRORS] = "DEMO - Errores",
    [STR_HP_RESOLUTION] = "\xEF\x80\x93 Resolución:",
    [STR_HP_CONTACT_DEALER] = "Contacte al distribuidor.",
    [STR_HP_STARTED] = "Inicio:",
    [STR_HP_ACTIVE_FOR] = "Activo desde hace",
    [STR_HP_DURATION] = "Duración:",
    [STR_HP_JUST_DETECTED] = "\xEF\x81\xB1 Recién detectado",
    [STR_HP_DISCONNECTED_MSG] = "\xEF\x81\xB1 Bomba de calor desconectada\n\nEstado de errores no disponible.\nVerifique la conexión Modbus.",
    [STR_HP_NO_ERRORS] = "\xEF\x80\x8C Sin errores activos\n\nTodos los sistemas funcionan normalmente.",
    [STR_HP_SYSTEM_OK] = "\xEF\x80\x8C Sin errores activos",
    [STR_HP_ACTIVE_ERRORS] = "Errores activos",
    [STR_HP_CLEAR_HISTORY] = "Borrar",
    [STR_HP_ERROR_HISTORY] = "Historial de errores",
    
    // Heat Pump - Control Screen
    [STR_HP_ADVANCED] = "Control",
    [STR_HP_DEMO_ADVANCED] = "DEMO - Control",
    [STR_HP_EDIT_PARAMETER] = "Editar parámetro",
    [STR_HP_RANGE_FMT] = "Rango:",
    [STR_HP_KRATIO_REDUCE] = "Reduce la frecuencia de funcionamiento en %d pasos por %d Hz",
    [STR_HP_KRATIO_NONE] = "Sin cambios en la frecuencia de funcionamiento",
    [STR_HP_COOLING_SETPOINT] = "Consigna enfriamiento",
    [STR_HP_HEATING_SETPOINT] = "Consigna calefacción",
    [STR_HP_HOT_WATER_SETPOINT] = "Consigna agua caliente",
    [STR_HP_CANNOT_SAVE] = "No se puede guardar: bomba de calor no conectada",
    [STR_HP_CANNOT_SAVE_SETPOINT] = "No se puede guardar la consigna: bomba de calor no conectada",
    [STR_HP_CAT_EEV] = "Válvula (EEV)",
    [STR_HP_CAT_DEFROST] = "Descongelación",
    [STR_HP_CAT_PROTECTION] = "Protección",
    [STR_HP_CAT_AUTO_MODE] = "Modo auto",
    [STR_HP_CAT_PUMP_VALVE] = "Bomba y válvula",
    // P-parameter names
    [STR_HP_PARAM_EEV_OPENING] = "Apertura EEV",
    [STR_HP_PARAM_EEV_MODE] = "Modo EEV",
    [STR_HP_PARAM_TARGET_SUPERHEAT] = "Sobrecalent. objetivo",
    [STR_HP_PARAM_DEFROST_CYCLE] = "Ciclo descongelación",
    [STR_HP_PARAM_DEFROST_ENTER_TEMP] = "Temp. inicio descong.",
    [STR_HP_PARAM_DEFROST_EXTEND_TEMP] = "Temp. extensión descong.",
    [STR_HP_PARAM_DEFROST_TEMP_DIFF] = "Dif. temp. descong.",
    [STR_HP_PARAM_DEFROST_EXTEND_TIME] = "Tiempo ext. descong.",
    [STR_HP_PARAM_MAX_DEFROST_TIME] = "Tiempo máx. descong.",
    [STR_HP_PARAM_DEFROST_EXIT_TEMP] = "Temp. salida descong.",
    [STR_HP_PARAM_LOW_AMBIENT_PROTECT] = "Protección baja temp.",
    [STR_HP_PARAM_FREQ_REDUCTION] = "Reducción frecuencia",
    [STR_HP_PARAM_COOLING_LOW_AMBIENT] = "Enfr. baja temp.",
    [STR_HP_PARAM_MAX_SETTING_TEMP] = "Temp. ajuste máx.",
    [STR_HP_PARAM_COOLING_AUTO_TEMP] = "Temp. auto enfr.",
    [STR_HP_PARAM_HEATING_AUTO_TEMP] = "Temp. auto calef.",
    [STR_HP_PARAM_MODE_SWITCH_DELAY] = "Retraso cambio modo",
    [STR_HP_PARAM_STERILIZE_TIME] = "Tiempo esterilización",
    [STR_HP_PARAM_WATER_RETURN_TEMP] = "Temp. retorno agua",
    [STR_HP_PARAM_WATER_RETURN_TIME] = "Tiempo retorno agua",
    [STR_HP_PARAM_3WAY_VALVE_TIME] = "Tiempo válv. 3 vías",
    [STR_HP_PARAM_PUMP_MODE] = "Modo bomba",
    [STR_HP_PARAM_PUMP_INTERVAL] = "Intervalo bomba",
    [STR_HP_PARAM_PUMP_LOW_AMBIENT] = "Bomba baja temp.",
    [STR_HP_PARAM_WATERWAY_CLEAN] = "Limpieza circuito",

    // Heat Pump - Dashboard Labels
    [STR_HP_COMP_RUNNING] = "EN MARCHA",
    [STR_HP_COMP_IDLE] = "INACTIVO",
    [STR_HP_STANDBY_DEMAND] = "Espera \xe2\x80\x94 esperando demanda",
    [STR_HP_DELTA_ABOVE_FMT] = "+%d\xc2\xb0 arriba",
    [STR_HP_DELTA_BELOW_FMT] = "%d\xc2\xb0 abajo",
    [STR_HP_DELTA_AT] = "En consigna",
    [STR_HP_LABEL_INLET] = "ENTRADA",
    [STR_HP_LABEL_OUTLET] = "SALIDA",
    [STR_HP_LABEL_OUTDOOR] = "EXTERIOR",
    [STR_HP_LABEL_COIL] = "BOBINA",
    [STR_HP_STATE_FAULT] = "FALLO",
    [STR_HP_ENERGY] = "Energ" "\xc3\xad" "a",
    [STR_HP_LABEL_COP] = "COP",
    [STR_HP_LABEL_POWER] = "POTENCIA",
    [STR_HP_LABEL_FAN] = "VENTIL.",
    [STR_HP_LABEL_DISCHARGE] = "DESCARGA",
    [STR_HP_LABEL_SUCTION] = "SUCCI" "\xc3\x93" "N",
    [STR_HP_LABEL_EEV] = "EEV",
    [STR_HP_LABEL_HI_PRESS] = "PRES. ALTA",
    [STR_HP_LABEL_LO_PRESS] = "PRES. BAJA",
    [STR_HP_LABEL_POWER_IN] = "POT. ENTR.",
    [STR_HP_LABEL_HEAT_OUT] = "CALOR",
    [STR_HP_LABEL_COOLING_OUT] = "FRÍO",

    // Event Log
    [STR_EVENT_LOG] = "Eventos",
    [STR_EVENT_SYSTEM_START] = "Inicio del sistema",
    [STR_EVENT_POWER_ON] = "Encendido",
    [STR_EVENT_POWER_OFF] = "Apagado",
    [STR_EVENT_MODE_CHANGED] = "Modo cambiado",
    [STR_EVENT_SETPOINT_CHANGED] = "Consigna cambiada",
    [STR_EVENT_COMPRESSOR_ON] = "Compresor ON",
    [STR_EVENT_COMPRESSOR_OFF] = "Compresor OFF",
    [STR_EVENT_FAN_ON] = "Ventilador ON",
    [STR_EVENT_FAN_OFF] = "Ventilador OFF",
    [STR_EVENT_PUMP_ON] = "Bomba de agua ON",
    [STR_EVENT_PUMP_OFF] = "Bomba de agua OFF",
    [STR_EVENT_AUX_HEATER_ON] = "Calef. aux ON",
    [STR_EVENT_AUX_HEATER_OFF] = "Calef. aux OFF",
    [STR_EVENT_DEFROST_START] = "Inicio descongelación",
    [STR_EVENT_DEFROST_END] = "Fin descongelación",
    [STR_EVENT_ERROR_APPEARED] = "Error detectado",
    [STR_EVENT_ERROR_CLEARED] = "Error eliminado",
    [STR_EVENT_CONNECTED] = "Conectado",
    [STR_EVENT_DISCONNECTED] = "Desconectado",
    [STR_EVENT_BROWNOUT_RESET] = "Reinicio por baja tensión",
    [STR_EVENT_APPLICATION_CRASH] = "Fallo de la aplicación",
    [STR_EVENT_WATCHDOG_RESET] = "Reinicio por vigilancia",
    [STR_EVENT_WATCHDOG_INTERRUPT] = "Vigilancia de interrupción",
    [STR_EVENT_WATCHDOG_TASK] = "Vigilancia de tarea",
    [STR_EVENT_WATCHDOG_OTHER] = "Vigilancia del sistema",
    [STR_EVENT_CLEAR] = "Borrar eventos",
    [STR_EVENT_NO_EVENTS] = "Sin eventos registrados",
    [STR_EVENT_SHOW_OLDER] = "Ver eventos anteriores",
    [STR_EVENT_TODAY] = "Hoy",
    [STR_EVENT_YESTERDAY] = "Ayer",
    [STR_EVENT_SINCE_RESTART] = "Desde el reinicio",
    [STR_EVENT_SEARCH] = "Buscar eventos...",
    [STR_EVENT_FILTERS] = "Filtros",
    [STR_EVENT_PROBLEMS] = "Problemas",
    [STR_EVENT_EQUIPMENT] = "Equipo",
    [STR_EVENT_CHANGES] = "Cambios",
    [STR_EVENT_SYSTEM] = "Sistema",
    [STR_EVENT_RESULTS_FMT] = "%d de %d eventos",
    [STR_EVENT_NO_MATCHES] = "No hay eventos coincidentes",
    [STR_EVENT_NEW_MATCHES] = "Nuevos eventos coincidentes",
    [STR_EVENT_TIME_ALL] = "Todo el período",
    [STR_EVENT_TIME_TODAY] = "Hoy",
    [STR_EVENT_TIME_24_HOURS] = "Últimas 24 horas",
    [STR_EVENT_TIME_7_DAYS] = "Últimos 7 días",
    [STR_EVENT_TIME_SINCE_RESTART] = "Desde el reinicio",
    [STR_EVENT_RESET_FILTERS] = "Restablecer filtros",
    [STR_EVENT_APPLY_SEARCH] = "Buscar",
    [STR_EVENT_APPLY_FILTERS] = "Aplicar",
    [STR_EVENT_CLEAR_CONFIRM_TITLE] = "¿Borrar el historial?",
    [STR_EVENT_CLEAR_CONFIRM_TEXT] = "Se eliminarán todos los eventos registrados.",
    [STR_EVENT_MONTH_JAN] = "ene",
    [STR_EVENT_MONTH_FEB] = "feb",
    [STR_EVENT_MONTH_MAR] = "mar",
    [STR_EVENT_MONTH_APR] = "abr",
    [STR_EVENT_MONTH_MAY] = "may",
    [STR_EVENT_MONTH_JUN] = "jun",
    [STR_EVENT_MONTH_JUL] = "jul",
    [STR_EVENT_MONTH_AUG] = "ago",
    [STR_EVENT_MONTH_SEP] = "sep",
    [STR_EVENT_MONTH_OCT] = "oct",
    [STR_EVENT_MONTH_NOV] = "nov",
    [STR_EVENT_MONTH_DEC] = "dic",

    // Reboot confirmation
    [STR_DEMO_MODE_CHANGED] = "Modo demo cambiado.",
    [STR_RESTART_REQUIRED] = "Reinicio necesario.",
    [STR_RESTART] = "Reiniciar",
    [STR_FACTORY_RESET_TITLE] = "¿Restablecer este controlador?",
    [STR_FACTORY_RESET_DESCRIPTION] = "Esto borra permanentemente el WiFi, los ajustes, los certificados, el historial de eventos y los archivos almacenados.",
    [STR_FACTORY_RESET_WARNING] = "Esta acción no se puede deshacer.",
    [STR_FACTORY_RESET_CONFIRM] = "Borrar y restablecer",
    [STR_FACTORY_RESET_ERASING] = "Borrando...",
    [STR_FACTORY_RESET_START_FAILED] = "No se pudo iniciar el restablecimiento. Inténtelo de nuevo.",
};

// Language names in their own language (native names)
static const char* language_names_native[LANG_COUNT] = {
    [LANG_ENGLISH] = "English",
    [LANG_FRENCH] = "Français",
    [LANG_SPANISH] = "Español",
};

// All string tables indexed by language
static const char** string_tables[LANG_COUNT] = {
    [LANG_ENGLISH] = strings_en,
    [LANG_FRENCH] = strings_fr,
    [LANG_SPANISH] = strings_es,
};

// ============================================================================
// Public API
// ============================================================================

void i18n_init(void)
{
    // Load saved language from NVS
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t lang = 0;
        if (nvs_get_u8(nvs, NVS_KEY_LANGUAGE, &lang) == ESP_OK) {
            if (lang < LANG_COUNT) {
                s_current_language = (language_t)lang;
                ESP_LOGI(TAG, "Loaded language: %s", language_names_native[s_current_language]);
            }
        }
        nvs_close(nvs);
    }
    
    if (s_current_language == LANG_ENGLISH) {
        ESP_LOGI(TAG, "Using default language: English");
    }
}

const char* i18n_get(string_id_t id)
{
    if (id >= STR_COUNT) {
        return "???";
    }
    
    // Try current language first
    const char* str = string_tables[s_current_language][id];
    if (str != NULL) {
        return str;
    }
    
    // Fallback to English
    str = strings_en[id];
    if (str != NULL) {
        return str;
    }
    
    return "???";
}

// ============================================================================
// Keyed (library-sourced) translations
// ============================================================================
// The macon library owns the English source of truth for parameter names,
// detail paragraphs and enum meanings; the controller stores ONLY the
// non-English translations here, keyed by the stable msg_id the library emits.
// English text is intentionally absent — i18n_get_key() returns the library's
// English fallback for LANG_ENGLISH and for any key without a translation, so no
// prose is ever duplicated in the controller.
//
// This table is NULL-terminated (sentinel row) so it stays valid while empty.
// Add rows ABOVE the sentinel as translations are authored (e.g. Phase 2 adds
// the AP14-20 "ap.freq_ratio_kN.name/.detail" entries).
typedef struct {
    const char* key;
    const char* fr;
    const char* es;
} keyed_translation_t;

static const keyed_translation_t s_keyed_translations[] = {
    { "ap.freq_ratio_k1.name",   "Rapport de fréquence K1", "Relación de frecuencia K1" },
    { "ap.freq_ratio_k1.detail",
      "Ajuste la vitesse du compresseur en chauffage ou production d'eau chaude lorsque l'air extérieur est à {T:-9} ou moins et l'eau d'entrée à {T:43} ou moins.",
      "Ajusta la velocidad del compresor en calefacción o producción de agua caliente cuando el aire exterior está a {T:-9} o menos y el agua de entrada a {T:43} o menos." },
    { "ap.freq_ratio_k2.name",   "Rapport de fréquence K2", "Relación de frecuencia K2" },
    { "ap.freq_ratio_k2.detail",
      "Ajuste la vitesse du compresseur en chauffage ou production d'eau chaude lorsque l'air extérieur est entre {T:-9} et {T:18} et l'eau d'entrée à {T:43} ou moins.",
      "Ajusta la velocidad del compresor en calefacción o producción de agua caliente cuando el aire exterior está entre {T:-9} y {T:18} y el agua de entrada a {T:43} o menos." },
    { "ap.freq_ratio_k3.name",   "Rapport de fréquence K3", "Relación de frecuencia K3" },
    { "ap.freq_ratio_k3.detail",
      "Ajuste la vitesse du compresseur en chauffage ou production d'eau chaude lorsque l'air extérieur est au-dessus de {T:18} et l'eau d'entrée à {T:43} ou moins.",
      "Ajusta la velocidad del compresor en calefacción o producción de agua caliente cuando el aire exterior está por encima de {T:18} y el agua de entrada a {T:43} o menos." },
    { "ap.freq_ratio_k4.name",   "Rapport de fréquence K4", "Relación de frecuencia K4" },
    { "ap.freq_ratio_k4.detail",
      "Ajuste la vitesse du compresseur en chauffage ou production d'eau chaude lorsque l'air extérieur est à {T:-9} ou moins et l'eau d'entrée au-dessus de {T:43}.",
      "Ajusta la velocidad del compresor en calefacción o producción de agua caliente cuando el aire exterior está a {T:-9} o menos y el agua de entrada por encima de {T:43}." },
    { "ap.freq_ratio_k5.name",   "Rapport de fréquence K5", "Relación de frecuencia K5" },
    { "ap.freq_ratio_k5.detail",
      "Ajuste la vitesse du compresseur en chauffage ou production d'eau chaude lorsque l'air extérieur est entre {T:-9} et {T:18} et l'eau d'entrée au-dessus de {T:43}.",
      "Ajusta la velocidad del compresor en calefacción o producción de agua caliente cuando el aire exterior está entre {T:-9} y {T:18} y el agua de entrada por encima de {T:43}." },
    { "ap.freq_ratio_k6.name",   "Rapport de fréquence K6", "Relación de frecuencia K6" },
    { "ap.freq_ratio_k6.detail",
      "Ajuste la vitesse du compresseur en chauffage ou production d'eau chaude lorsque l'air extérieur est au-dessus de {T:18} et l'eau d'entrée au-dessus de {T:43}.",
      "Ajusta la velocidad del compresor en calefacción o producción de agua caliente cuando el aire exterior está por encima de {T:18} y el agua de entrada por encima de {T:43}." },
    { "ap.freq_ratio_k7.name",   "Rapport de fréquence K7", "Relación de frecuencia K7" },
    { "ap.freq_ratio_k7.detail",
      "Ajuste la vitesse du compresseur en mode refroidissement.",
      "Ajusta la velocidad del compresor en modo refrigeración." },

    { "ap.max_hw_setpoint.name",  "Consigne d'eau chaude maximale", "Consigna máxima de agua caliente" },
    { "ap.max_hw_setpoint.detail",
      "Température d'eau la plus élevée que l'appareil atteindra en mode chauffage ou eau chaude.",
      "Temperatura de agua más alta que la unidad alcanzará en modo calefacción o agua caliente." },

    { "ap.heating_comp_stop_ambient.name", "Arrêt compresseur (chauffage)", "Parada del compresor (calefacción)" },
    { "ap.heating_comp_stop_ambient.detail",
      "Mode chauffage : le compresseur s'arrête dès que la température ambiante extérieure atteint cette valeur ou plus.",
      "Modo calefacción: el compresor se detiene cuando la temperatura ambiente exterior sube a este valor o más." },

    { "ap.backup_eheater_start_ambient.name", "Démarrage chauffage d'appoint", "Arranque del calefactor de apoyo" },
    { "ap.backup_eheater_start_ambient.detail",
      "Le chauffage électrique d'appoint peut démarrer lorsque la température ambiante extérieure descend à cette valeur.",
      "El calefactor eléctrico de apoyo puede arrancar cuando la temperatura ambiente exterior baja a este valor." },

    { "ap.quiet_mode_freq_decrease.name", "Baisse de fréquence mode silencieux", "Reducción de frecuencia modo silencioso" },
    { "ap.quiet_mode_freq_decrease.detail",
      "Palier de fréquence dont le compresseur diminue lorsque le mode silencieux est actif.",
      "Escalón de frecuencia que el compresor reduce mientras el modo silencioso está activo." },

    { "ap.fast_heat_freq_increase.name", "Hausse de fréquence chauffage rapide", "Aumento de frecuencia calefacción rápida" },
    { "ap.fast_heat_freq_increase.detail",
      "Palier de fréquence dont le compresseur augmente lorsque le mode chauffage rapide est actif.",
      "Escalón de frecuencia que el compresor aumenta mientras el modo calefacción rápida está activo." },

    { "ap.auto_mode_switch_wait.name", "Délai de commutation mode auto", "Tiempo de espera de cambio modo auto" },
    { "ap.auto_mode_switch_wait.detail",
      "Mode automatique : temps d'attente avant de basculer entre chauffage et refroidissement.",
      "Modo automático: tiempo de espera antes de cambiar entre calefacción y refrigeración." },

    { "ap.comp_runtime_before_defrost.name", "Temps de marche avant dégivrage", "Tiempo de marcha antes del desescarche" },
    { "ap.comp_runtime_before_defrost.detail",
      "Temps de fonctionnement cumulé du compresseur devant s'écouler avant qu'un cycle de dégivrage soit autorisé.",
      "Tiempo de funcionamiento acumulado del compresor que debe transcurrir antes de permitir un ciclo de desescarche." },

    { "ap.coil_temp_enter_defrost.name", "Temp. serpentin entrée dégivrage", "Temp. serpentín inicio desescarche" },
    { "ap.coil_temp_enter_defrost.detail",
      "Température du serpentin extérieur à laquelle, ou en dessous de laquelle, un cycle de dégivrage démarre.",
      "Temperatura del serpentín exterior a la que, o por debajo de la cual, se inicia un ciclo de desescarche." },

    { "ap.defrost_outdoor_temp.name", "Réglage temp. extérieure dégivrage", "Ajuste temp. exterior desescarche" },
    { "ap.defrost_outdoor_temp.detail",
      "Réglage de la température ambiante extérieure utilisé comme l'une des conditions de démarrage du dégivrage.",
      "Ajuste de la temperatura ambiente exterior usado como una de las condiciones para iniciar el desescarche." },

    { "ap.air_coil_diff_defrost.name", "Écart air/serpentin entrée dégivrage", "Diferencia aire/serpentín inicio desescarche" },
    { "ap.air_coil_diff_defrost.detail",
      "Écart de température entre l'air extérieur et le serpentin qui déclenche un cycle de dégivrage.",
      "Diferencia de temperatura entre el aire exterior y el serpentín que activa un ciclo de desescarche." },

    { "ap.extended_defrost_time.name", "Temps de dégivrage prolongé", "Tiempo de desescarche prolongado" },
    { "ap.extended_defrost_time.detail",
      "Temps supplémentaire ajouté à un cycle de dégivrage.",
      "Tiempo adicional añadido a un ciclo de desescarche." },

    { "ap.max_defrost_time.name", "Temps de dégivrage maximal", "Tiempo máximo de desescarche" },
    { "ap.max_defrost_time.detail",
      "Durée maximale de dégivrage ; l'appareil sort du dégivrage une fois ce temps atteint.",
      "Duración máxima del desescarche; la unidad sale del desescarche al alcanzar este tiempo." },

    { "ap.coil_temp_exit_defrost.name", "Temp. serpentin sortie dégivrage", "Temp. serpentín fin desescarche" },
    { "ap.coil_temp_exit_defrost.detail",
      "Température du serpentin à laquelle le dégivrage est considéré terminé et l'appareil en sort.",
      "Temperatura del serpentín a la que el desescarche se considera completo y la unidad sale de él." },

    { "ap.low_ambient_protection.name", "Protection basse température extérieure", "Protección de baja temperatura exterior" },
    { "ap.low_ambient_protection.detail",
      "L'appareil s'arrête lorsque la température ambiante extérieure descend sous cette valeur. Le plancher du capteur est -30 °C ; régler une valeur inférieure à -30 désactive cette protection, gardez-la donc entre -30 et 0 °C.",
      "La unidad se detiene cuando la temperatura ambiente exterior baja por debajo de este valor. El límite del sensor es -30 °C; ajustar un valor inferior a -30 desactiva esta protección, así que manténgalo entre -30 y 0 °C." },

    { "ap.freq_reduce_delay.name", "Délai de réduction de fréquence", "Retardo de reducción de frecuencia" },
    { "ap.freq_reduce_delay.detail",
      "Temps après l'atteinte de la consigne avant que la fréquence de fonctionnement soit réduite. Laisser la valeur par défaut (40) désactive la réduction ; toute autre valeur l'active.",
      "Tiempo tras alcanzar la consigna antes de reducir la frecuencia de funcionamiento. Dejar el valor predeterminado (40) desactiva la reducción; cualquier otro valor la activa." },

    { "ap.cooling_comp_stop_ambient.name", "Arrêt compresseur (refroidissement)", "Parada del compresor (refrigeración)" },
    { "ap.cooling_comp_stop_ambient.detail",
      "Mode refroidissement : le compresseur s'arrête lorsque la température ambiante extérieure descend à cette valeur. Utilisez une valeur négative pour les systèmes au glycol et positive pour les systèmes à eau ; la magnitude est le seuil ambiant.",
      "Modo refrigeración: el compresor se detiene cuando la temperatura ambiente exterior baja a este valor. Use un valor negativo para sistemas con glicol y positivo para sistemas de agua; la magnitud es el umbral ambiente." },

    { "ap.main_eev_superheat_method.name", "Méthode de surchauffe détendeur principal", "Método de sobrecalentamiento válvula principal" },
    { "ap.main_eev_superheat_method.detail",
      "Méthode de régulation de la surchauffe du détendeur principal (0 = par degré de surchauffe, 1 = par une courbe étalonnée en laboratoire). Lecture seule : la mise à l'échelle sur l'appareil n'est pas confirmée.",
      "Método de control del sobrecalentamiento de la válvula de expansión principal (0 = por grado de sobrecalentamiento, 1 = por una curva calibrada en laboratorio). Solo lectura: la escala en la unidad no está confirmada." },

    { "ap.target_superheat_main_eev.name", "Surchauffe cible, détendeur principal", "Sobrecalentamiento objetivo, válvula principal" },
    { "ap.target_superheat_main_eev.detail",
      "Surchauffe cible que le détendeur principal maintient.",
      "Sobrecalentamiento objetivo que regula la válvula de expansión principal." },

    { "ap.three_way_valve2_switch_time.name", "Temps de commutation vanne 3 voies 2", "Tiempo de conmutación válvula de 3 vías 2" },
    { "ap.three_way_valve2_switch_time.detail",
      "Temps de commutation de la vanne à trois voies 2. Toute valeur autre que 5 annule le contrôle externe Cn31 ; remettez 5 pour réactiver Cn31.",
      "Tiempo de conmutación de la válvula de tres vías 2. Cualquier valor distinto de 5 cancela el control externo Cn31; vuelva a 5 para reactivar Cn31." },

    { "ap.water_pump_mode.name", "Mode pompe à eau", "Modo de bomba de agua" },
    { "ap.water_pump_mode.detail",
      "Mode de fonctionnement de la pompe à eau (0 = par intervalles, 1 = suit le compresseur, 2 = en continu).",
      "Modo de funcionamiento de la bomba de agua (0 = por intervalos, 1 = sigue al compresor, 2 = en continuo)." },
    { "pump_mode_intervals", "Par intervalles", "Por intervalos" },
    { "pump_mode_follow", "Suit le compresseur", "Sigue el compresor" },
    { "pump_mode_continuous", "En continu", "Continuo" },

    { "ap.water_pump_run_interval.name", "Intervalle de marche pompe à eau", "Intervalo de marcha bomba de agua" },
    { "ap.water_pump_run_interval.detail",
      "Intervalle entre les cycles de la pompe à eau lorsqu'elle est en mode par intervalles.",
      "Intervalo entre ciclos de la bomba de agua cuando está en modo por intervalos." },

    { "ap.force_pump_low_temp.name", "Consigne basse temp. pompe forcée", "Consigna baja temp. bomba forzada" },
    { "ap.force_pump_low_temp.detail",
      "Température extérieure à laquelle, ou en dessous de laquelle, la pompe à eau est forcée de fonctionner pour la protection antigel.",
      "Temperatura exterior a la que, o por debajo de la cual, la bomba de agua se fuerza a funcionar para protección anticongelación." },

    { "ap.water_system_cleaning.name", "Nettoyage du circuit d'eau", "Limpieza del circuito de agua" },
    { "ap.water_system_cleaning.detail",
      "Nettoyage / test du circuit d'eau (1 = test pompe, 2 = test pompe + vanne 3 voies 1, 3 = test pompe + vanne 3 voies 2). La protection de débit est désactivée pendant l'exécution.",
      "Limpieza / prueba del circuito de agua (1 = prueba bomba, 2 = prueba bomba + válvula 3 vías 1, 3 = prueba bomba + válvula 3 vías 2). La protección de caudal se desactiva mientras se ejecuta." },

    { "ap.enable_manual_freq_eev.name", "Activer fréquence/EEV manuelles", "Habilitar frecuencia/EEV manual" },
    { "ap.enable_manual_freq_eev.detail",
      "Active la commande manuelle de la fréquence du compresseur et de l'ouverture du détendeur (usage service / test uniquement).",
      "Habilita el control manual de la frecuencia del compresor y la apertura de la válvula de expansión (solo uso de servicio / prueba)." },

    { "ap.manual_frequency.name", "Fréquence manuelle", "Frecuencia manual" },
    { "ap.manual_frequency.detail",
      "Fréquence manuelle du compresseur ; utilisée uniquement lorsque le mode manuel (AP48) est activé.",
      "Frecuencia manual del compresor; se usa solo cuando el modo manual (AP48) está habilitado." },

    { "ap.manual_main_eev_opening.name", "Ouverture manuelle détendeur principal", "Apertura manual válvula principal" },
    { "ap.manual_main_eev_opening.detail",
      "Ouverture manuelle du détendeur principal ; utilisée uniquement lorsque le mode manuel (AP48) est activé.",
      "Apertura manual de la válvula de expansión principal; se usa solo cuando el modo manual (AP48) está habilitado." },

    { "ap.manual_evi_eev_opening.name", "Ouverture manuelle détendeur EVI", "Apertura manual válvula EVI" },
    { "ap.manual_evi_eev_opening.detail",
      "Ouverture manuelle du détendeur EVI / auxiliaire ; utilisée uniquement lorsque le mode manuel (AP48) est activé.",
      "Apertura manual de la válvula de expansión EVI / auxiliar; se usa solo cuando el modo manual (AP48) está habilitado." },

    { NULL, NULL, NULL },  // sentinel — keep last
};

const char* i18n_get_key(const char* key, const char* english_fallback)
{
    // English (and any missing key) always resolves to the library source text.
    if (!key || s_current_language == LANG_ENGLISH) {
        return english_fallback;
    }
    for (const keyed_translation_t* e = s_keyed_translations; e->key != NULL; ++e) {
        if (strcmp(e->key, key) == 0) {
            const char* t = (s_current_language == LANG_FRENCH) ? e->fr : e->es;
            if (t != NULL && t[0] != '\0') {
                return t;
            }
            break;  // key known but untranslated -> fall back to English
        }
    }
    return english_fallback;
}

string_id_t i18n_find_by_english(const char* english_text)
{
    if (!english_text) return STR_COUNT;
    for (int i = 0; i < STR_COUNT; i++) {
        if (strings_en[i] && strcmp(strings_en[i], english_text) == 0) {
            return (string_id_t)i;
        }
    }
    return STR_COUNT;
}

const char* i18n_translate(const char* english_text)
{
    string_id_t id = i18n_find_by_english(english_text);
    if (id < STR_COUNT) {
        return i18n_get(id);
    }
    return english_text;  // no match, return as-is
}

const char* i18n_get_english(const char* localized_text)
{
    if (!localized_text) return NULL;
    // Check if it's already English
    for (int i = 0; i < STR_COUNT; i++) {
        if (strings_en[i] && strcmp(strings_en[i], localized_text) == 0) {
            return strings_en[i];
        }
    }
    // Search all language tables for a match, then return the English equivalent
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        for (int i = 0; i < STR_COUNT; i++) {
            if (string_tables[lang][i] && strcmp(string_tables[lang][i], localized_text) == 0) {
                return strings_en[i];  // return English version
            }
        }
    }
    return NULL;
}

language_t i18n_get_language(void)
{
    return s_current_language;
}

void i18n_set_language(language_t lang)
{
    if (lang >= LANG_COUNT) {
        return;
    }
    
    s_current_language = lang;
    ESP_LOGI(TAG, "Language set to: %s", language_names_native[lang]);
    
    // Save to NVS
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_LANGUAGE, (uint8_t)lang);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

const char* i18n_get_language_name(language_t lang)
{
    if (lang >= LANG_COUNT) {
        return "???";
    }
    return language_names_native[lang];
}

const char* i18n_get_language_name_localized(language_t lang)
{
    if (lang >= LANG_COUNT) {
        return "???";
    }
    
    // Return the language name in the current UI language
    switch (lang) {
        case LANG_ENGLISH:
            return i18n_get(STR_LANG_ENGLISH);
        case LANG_FRENCH:
            return i18n_get(STR_LANG_FRENCH);
        case LANG_SPANISH:
            return i18n_get(STR_LANG_SPANISH);
        default:
            return "???";
    }
}
