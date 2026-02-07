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
