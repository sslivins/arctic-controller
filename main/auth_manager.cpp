/*
 * Arctic Heat Pump Controller
 * Authentication Manager - Session and API Key Implementation
 */
#include "auth_manager.h"
#include <esp_log.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

static const char* TAG = "auth_mgr";

// NVS namespace and keys
static const char* NVS_NAMESPACE = "auth";
static const char* NVS_KEY_WEB_ENABLED = "web_en";
static const char* NVS_KEY_API_ENABLED = "api_en";
static const char* NVS_KEY_USERNAME = "username";
static const char* NVS_KEY_PASS_HASH = "pass_hash";
static const char* NVS_KEY_API_KEY = "api_key";

// Session structure
typedef struct {
    bool active;
    char token[AUTH_SESSION_TOKEN_LEN + 1];
    time_t created_at;
    time_t last_used;
} session_t;

// Local state
static struct {
    bool initialized;
    bool web_auth_enabled;
    bool api_auth_enabled;
    char username[AUTH_MAX_USERNAME_LEN + 1];
    uint8_t password_hash[32];  // SHA-256
    bool password_set;
    char api_key[AUTH_API_KEY_LEN + 1];
    session_t sessions[AUTH_MAX_SESSIONS];
} state = {};

// ============================================================================
// Helper Functions
// ============================================================================

static void generate_random_hex(char* buffer, size_t len)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        buffer[i] = hex_chars[esp_random() % 16];
    }
    buffer[len] = '\0';
}

static void hash_password(const char* password, uint8_t* hash_out)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256 (not SHA-224)
    mbedtls_sha256_update(&ctx, (const unsigned char*)password, strlen(password));
    mbedtls_sha256_finish(&ctx, hash_out);
    mbedtls_sha256_free(&ctx);
}

static bool load_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No auth settings in NVS, using defaults");
        return false;
    }
    
    // Web auth enabled
    uint8_t enabled = 0;
    if (nvs_get_u8(nvs, NVS_KEY_WEB_ENABLED, &enabled) == ESP_OK) {
        state.web_auth_enabled = enabled != 0;
        ESP_LOGI(TAG, "Loaded web_auth_enabled=%d from NVS", enabled);
    } else {
        ESP_LOGI(TAG, "web_auth_enabled not in NVS, defaulting to false");
    }
    
    // API auth enabled
    if (nvs_get_u8(nvs, NVS_KEY_API_ENABLED, &enabled) == ESP_OK) {
        state.api_auth_enabled = enabled != 0;
        ESP_LOGI(TAG, "Loaded api_auth_enabled=%d from NVS", enabled);
    }
    
    // Username
    size_t len = sizeof(state.username);
    if (nvs_get_str(nvs, NVS_KEY_USERNAME, state.username, &len) != ESP_OK) {
        state.username[0] = '\0';
    }
    
    // Password hash
    len = sizeof(state.password_hash);
    if (nvs_get_blob(nvs, NVS_KEY_PASS_HASH, state.password_hash, &len) == ESP_OK) {
        state.password_set = true;
    }
    
    // API key
    len = sizeof(state.api_key);
    if (nvs_get_str(nvs, NVS_KEY_API_KEY, state.api_key, &len) != ESP_OK) {
        state.api_key[0] = '\0';
    }
    
    nvs_close(nvs);
    return true;
}

static bool save_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "Saving to NVS: web_auth=%d, api_auth=%d", 
             state.web_auth_enabled ? 1 : 0, state.api_auth_enabled ? 1 : 0);
    
    nvs_set_u8(nvs, NVS_KEY_WEB_ENABLED, state.web_auth_enabled ? 1 : 0);
    nvs_set_u8(nvs, NVS_KEY_API_ENABLED, state.api_auth_enabled ? 1 : 0);
    
    if (state.username[0] != '\0') {
        nvs_set_str(nvs, NVS_KEY_USERNAME, state.username);
    }
    
    if (state.password_set) {
        nvs_set_blob(nvs, NVS_KEY_PASS_HASH, state.password_hash, sizeof(state.password_hash));
    }
    
    if (state.api_key[0] != '\0') {
        nvs_set_str(nvs, NVS_KEY_API_KEY, state.api_key);
    }
    
    nvs_commit(nvs);
    nvs_close(nvs);
    return true;
}

static session_t* find_session(const char* token)
{
    if (token == NULL || token[0] == '\0') return NULL;
    
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (state.sessions[i].active && 
            strcmp(state.sessions[i].token, token) == 0) {
            return &state.sessions[i];
        }
    }
    return NULL;
}

static session_t* find_free_session(void)
{
    session_t* oldest = NULL;
    time_t oldest_time = 0;
    
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (!state.sessions[i].active) {
            return &state.sessions[i];
        }
        // Track oldest for eviction if needed
        if (oldest == NULL || state.sessions[i].last_used < oldest_time) {
            oldest = &state.sessions[i];
            oldest_time = state.sessions[i].last_used;
        }
    }
    
    // All slots full - evict oldest
    if (oldest != NULL) {
        ESP_LOGW(TAG, "Evicting oldest session to make room");
        oldest->active = false;
        return oldest;
    }
    
    return NULL;
}

static bool is_session_expired(const session_t* session)
{
    if (session == NULL || !session->active) return true;
    
    time_t now = time(NULL);
    return (now - session->created_at) > AUTH_SESSION_LIFETIME_SEC;
}

// ============================================================================
// Public API
// ============================================================================

void auth_mgr_init(void)
{
    if (state.initialized) return;
    
    ESP_LOGI(TAG, "Initializing authentication manager...");
    
    memset(&state, 0, sizeof(state));
    load_from_nvs();
    
    // Set default credentials if none exist
    if (!state.password_set || state.username[0] == '\0') {
        strncpy(state.username, "arctic", sizeof(state.username) - 1);
        state.username[sizeof(state.username) - 1] = '\0';
        hash_password("arctic", state.password_hash);
        state.password_set = true;
        save_to_nvs();
        ESP_LOGI(TAG, "Default credentials set (arctic/arctic)");
    }
    
    // Generate API key if not set
    if (state.api_key[0] == '\0') {
        generate_random_hex(state.api_key, AUTH_API_KEY_LEN);
        save_to_nvs();
        ESP_LOGI(TAG, "Generated new API key");
    }
    
    state.initialized = true;
    
    ESP_LOGI(TAG, "Auth manager initialized - web_auth=%s, api_auth=%s",
             state.web_auth_enabled ? "enabled" : "disabled",
             state.api_auth_enabled ? "enabled" : "disabled");
}

// ============================================================================
// Web Authentication
// ============================================================================

bool auth_mgr_web_auth_enabled(void)
{
    return state.web_auth_enabled;
}

void auth_mgr_set_web_auth_enabled(bool enabled)
{
    if (state.web_auth_enabled != enabled) {
        state.web_auth_enabled = enabled;
        save_to_nvs();
        ESP_LOGI(TAG, "Web auth %s", enabled ? "enabled" : "disabled");
        
        // If disabling, clear all sessions
        if (!enabled) {
            auth_mgr_logout_all();
        }
    }
}

bool auth_mgr_set_credentials(const char* username, const char* password)
{
    bool changed = false;
    
    if (username != NULL) {
        strncpy(state.username, username, AUTH_MAX_USERNAME_LEN);
        state.username[AUTH_MAX_USERNAME_LEN] = '\0';
        changed = true;
    }
    
    if (password != NULL && password[0] != '\0') {
        hash_password(password, state.password_hash);
        state.password_set = true;
        changed = true;
    }
    
    if (changed) {
        save_to_nvs();
        ESP_LOGI(TAG, "Credentials updated");
    }
    
    return true;
}

const char* auth_mgr_get_username(void)
{
    return state.username;
}

bool auth_mgr_login(const char* username, const char* password, char* session_token)
{
    if (!state.web_auth_enabled) {
        // Auth disabled - always succeed with no token
        return true;
    }
    
    if (username == NULL || password == NULL) {
        ESP_LOGW(TAG, "Login failed: null credentials");
        return false;
    }
    
    // Check username
    if (strcmp(username, state.username) != 0) {
        ESP_LOGW(TAG, "Login failed: invalid username");
        return false;
    }
    
    // Check password
    if (!state.password_set) {
        ESP_LOGW(TAG, "Login failed: no password set");
        return false;
    }
    
    uint8_t input_hash[32];
    hash_password(password, input_hash);
    
    if (memcmp(input_hash, state.password_hash, 32) != 0) {
        ESP_LOGW(TAG, "Login failed: invalid password");
        return false;
    }
    
    // Create session
    session_t* session = find_free_session();
    if (session == NULL) {
        ESP_LOGE(TAG, "Login failed: no session slots");
        return false;
    }
    
    session->active = true;
    generate_random_hex(session->token, AUTH_SESSION_TOKEN_LEN);
    session->created_at = time(NULL);
    session->last_used = session->created_at;
    
    if (session_token != NULL) {
        strcpy(session_token, session->token);
    }
    
    ESP_LOGI(TAG, "Login successful, session created");
    return true;
}

bool auth_mgr_validate_session(const char* token)
{
    if (!state.web_auth_enabled) {
        return true;  // Auth disabled
    }
    
    session_t* session = find_session(token);
    if (session == NULL) {
        return false;
    }
    
    if (is_session_expired(session)) {
        ESP_LOGI(TAG, "Session expired");
        session->active = false;
        return false;
    }
    
    // Update last used time
    session->last_used = time(NULL);
    return true;
}

void auth_mgr_logout(const char* token)
{
    session_t* session = find_session(token);
    if (session != NULL) {
        session->active = false;
        ESP_LOGI(TAG, "Session logged out");
    }
}

void auth_mgr_logout_all(void)
{
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        state.sessions[i].active = false;
    }
    ESP_LOGI(TAG, "All sessions logged out");
}

// ============================================================================
// API Key Authentication
// ============================================================================

bool auth_mgr_api_auth_enabled(void)
{
    return state.api_auth_enabled;
}

void auth_mgr_set_api_auth_enabled(bool enabled)
{
    if (state.api_auth_enabled != enabled) {
        state.api_auth_enabled = enabled;
        save_to_nvs();
        ESP_LOGI(TAG, "API auth %s", enabled ? "enabled" : "disabled");
    }
}

bool auth_mgr_get_api_key(char* buffer)
{
    if (buffer == NULL) return false;
    
    if (state.api_key[0] == '\0') {
        buffer[0] = '\0';
        return false;
    }
    
    strcpy(buffer, state.api_key);
    return true;
}

bool auth_mgr_regenerate_api_key(char* buffer)
{
    generate_random_hex(state.api_key, AUTH_API_KEY_LEN);
    save_to_nvs();
    
    if (buffer != NULL) {
        strcpy(buffer, state.api_key);
    }
    
    ESP_LOGI(TAG, "API key regenerated");
    return true;
}

bool auth_mgr_validate_api_key(const char* key)
{
    if (!state.api_auth_enabled) {
        return true;  // Auth disabled
    }
    
    if (key == NULL || key[0] == '\0') {
        return false;
    }
    
    return strcmp(key, state.api_key) == 0;
}
