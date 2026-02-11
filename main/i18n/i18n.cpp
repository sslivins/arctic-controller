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
    
    // WiFi Panel
    [STR_WIFI_CONNECTED] = "Connected",
    [STR_WIFI_DISCONNECTED] = "Disconnected",
    [STR_WIFI_DISCONNECT] = "Disconnect",
    [STR_WIFI_CONNECT] = "Connect",
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
    [STR_HP_HOLD_POWER_OFF] = "\xEF\x80\x91 Powering off in %d...",
    [STR_HP_RESERVED_1] = "",
    [STR_HP_RESERVED_2] = "",
    [STR_HP_DEMO_MODE_ENABLED] = "Demo Mode Enabled",
    [STR_HP_NOT_CONNECTED] = "Heat pump not connected",
    [STR_HP_BTN_TEMPS] = "\xEF\x81\xA8 Temps",
    [STR_HP_BTN_SYSTEM] = "\xEF\x80\x8B System",
    [STR_HP_BTN_ADVANCED] = "\xEF\x80\x93 Advanced",
    
    // Heat Pump - Mode Names
    [STR_HP_MODE_COOLING] = "COOLING",
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
    
    // Heat Pump - Advanced/Params Screen
    [STR_HP_ADVANCED] = "Advanced",
    [STR_HP_DEMO_ADVANCED] = "DEMO MODE - Advanced",
    [STR_HP_EDIT_PARAMETER] = "Edit Parameter",
    [STR_HP_RANGE_FMT] = "Range:",
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
    [STR_HP_LABEL_AMBIENT] = "AMBIENT",
    [STR_HP_LABEL_COIL] = "COIL",
    [STR_HP_STATE_FAULT] = "FAULT",
    [STR_HP_ENERGY] = "Energy",
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
    
    // WiFi Panel
    [STR_WIFI_CONNECTED] = "Connecté",
    [STR_WIFI_DISCONNECTED] = "Déconnecté",
    [STR_WIFI_DISCONNECT] = "Déconnecter",
    [STR_WIFI_CONNECT] = "Connecter",
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
    [STR_HP_HOLD_POWER_OFF] = "\xEF\x80\x91 Arrêt dans %d...",
    [STR_HP_RESERVED_1] = "",
    [STR_HP_RESERVED_2] = "",
    [STR_HP_DEMO_MODE_ENABLED] = "Mode démo activé",
    [STR_HP_NOT_CONNECTED] = "Pompe à chaleur non connectée",
    [STR_HP_BTN_TEMPS] = "\xEF\x81\xA8 Temp.",
    [STR_HP_BTN_SYSTEM] = "\xEF\x80\x8B Système",
    [STR_HP_BTN_ADVANCED] = "\xEF\x80\x93 Avancé",
    
    // Heat Pump - Mode Names
    [STR_HP_MODE_COOLING] = "REFROIDISSEMENT",
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
    
    // Heat Pump - Advanced/Params Screen
    [STR_HP_ADVANCED] = "Avancé",
    [STR_HP_DEMO_ADVANCED] = "DÉMO - Avancé",
    [STR_HP_EDIT_PARAMETER] = "Modifier le paramètre",
    [STR_HP_RANGE_FMT] = "Plage :",
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
    [STR_HP_LABEL_AMBIENT] = "EXT" "\xc3\x89" "RIEUR",
    [STR_HP_LABEL_COIL] = "\xc3\x89" "CHANGEUR",
    [STR_HP_STATE_FAULT] = "PANNE",
    [STR_HP_ENERGY] = "\xc3\x89nergie",
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
    
    // WiFi Panel
    [STR_WIFI_CONNECTED] = "Conectado",
    [STR_WIFI_DISCONNECTED] = "Desconectado",
    [STR_WIFI_DISCONNECT] = "Desconectar",
    [STR_WIFI_CONNECT] = "Conectar",
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
    [STR_HP_HOLD_POWER_OFF] = "\xEF\x80\x91 Apagando en %d...",
    [STR_HP_RESERVED_1] = "",
    [STR_HP_RESERVED_2] = "",
    [STR_HP_DEMO_MODE_ENABLED] = "Modo demo activado",
    [STR_HP_NOT_CONNECTED] = "Bomba de calor no conectada",
    [STR_HP_BTN_TEMPS] = "\xEF\x81\xA8 Temp.",
    [STR_HP_BTN_SYSTEM] = "\xEF\x80\x8B Sistema",
    [STR_HP_BTN_ADVANCED] = "\xEF\x80\x93 Avanzado",
    
    // Heat Pump - Mode Names
    [STR_HP_MODE_COOLING] = "ENFRIAMIENTO",
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
    
    // Heat Pump - Advanced/Params Screen
    [STR_HP_ADVANCED] = "Avanzado",
    [STR_HP_DEMO_ADVANCED] = "DEMO - Avanzado",
    [STR_HP_EDIT_PARAMETER] = "Editar parámetro",
    [STR_HP_RANGE_FMT] = "Rango:",
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
    [STR_HP_LABEL_AMBIENT] = "EXTERIOR",
    [STR_HP_LABEL_COIL] = "BOBINA",
    [STR_HP_STATE_FAULT] = "FALLO",
    [STR_HP_ENERGY] = "Energ" "\xc3\xad" "a",
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
