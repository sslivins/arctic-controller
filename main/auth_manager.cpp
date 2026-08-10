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
#include <mbedtls/platform_util.h>
#include <freertos/FreeRTOS.h>
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
static const char* NVS_KEY_INTEGRATION_HASH = "ha_hash";

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
    uint8_t integration_token_hash[32];
    bool integration_token_set;
    uint32_t integration_generation;
    session_t sessions[AUTH_MAX_SESSIONS];
} state = {};
static portMUX_TYPE integration_token_lock = portMUX_INITIALIZER_UNLOCKED;

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

static bool constant_time_equal(const uint8_t* lhs, const uint8_t* rhs, size_t len)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < len; ++i) {
        difference |= lhs[i] ^ rhs[i];
    }
    return difference == 0;
}

static bool load_from_nvs(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No auth settings in NVS (err=%s), will use defaults", esp_err_to_name(err));
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
    err = nvs_get_str(nvs, NVS_KEY_USERNAME, state.username, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded username='%s' from NVS (len=%d)", state.username, (int)len);
    } else {
        state.username[0] = '\0';
        ESP_LOGI(TAG, "Username not in NVS (err=%s)", esp_err_to_name(err));
    }
    
    // Password hash
    len = sizeof(state.password_hash);
    err = nvs_get_blob(nvs, NVS_KEY_PASS_HASH, state.password_hash, &len);
    if (err == ESP_OK) {
        state.password_set = true;
        ESP_LOGI(TAG, "Loaded password hash from NVS (len=%d)", (int)len);
    } else {
        ESP_LOGI(TAG, "Password hash not in NVS (err=%s)", esp_err_to_name(err));
    }
    
    // API key
    len = sizeof(state.api_key);
    if (nvs_get_str(nvs, NVS_KEY_API_KEY, state.api_key, &len) != ESP_OK) {
        state.api_key[0] = '\0';
    }

    len = sizeof(state.integration_token_hash);
    err = nvs_get_blob(nvs, NVS_KEY_INTEGRATION_HASH,
                       state.integration_token_hash, &len);
    state.integration_token_set =
        err == ESP_OK && len == sizeof(state.integration_token_hash);
    state.integration_generation = state.integration_token_set ? 1 : 0;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to load integration token hash: %s",
                 esp_err_to_name(err));
    }
    
    nvs_close(nvs);
    return true;
}

static bool save_to_nvs(void)
{
    nvs_handle_t nvs = 0;
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
    bool loaded = load_from_nvs();
    
    // Set default credentials only if BOTH username and password are missing
    // This ensures we don't overwrite user-set credentials
    if (!loaded || (!state.password_set && state.username[0] == '\0')) {
        ESP_LOGI(TAG, "No credentials found in NVS, setting defaults (arctic/arctic)");
        strncpy(state.username, "arctic", sizeof(state.username) - 1);
        state.username[sizeof(state.username) - 1] = '\0';
        hash_password("arctic", state.password_hash);
        state.password_set = true;
        save_to_nvs();
    } else {
        ESP_LOGI(TAG, "Credentials loaded from NVS: username='%s', password_set=%d", 
                 state.username, state.password_set ? 1 : 0);
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
    
    ESP_LOGI(TAG, "set_credentials called: username='%s', password=%s",
             username ? username : "(null)", 
             password ? (password[0] ? "(provided)" : "(empty)") : "(null)");
    
    if (username != NULL && username[0] != '\0') {
        strncpy(state.username, username, AUTH_MAX_USERNAME_LEN);
        state.username[AUTH_MAX_USERNAME_LEN] = '\0';
        changed = true;
        ESP_LOGI(TAG, "Username updated to '%s'", state.username);
    }
    
    if (password != NULL && password[0] != '\0') {
        hash_password(password, state.password_hash);
        state.password_set = true;
        changed = true;
        ESP_LOGI(TAG, "Password hash updated");
    }
    
    if (changed) {
        bool saved = save_to_nvs();
        ESP_LOGI(TAG, "Credentials updated, NVS save %s", saved ? "successful" : "FAILED");
        
        // Invalidate all sessions when credentials change for security
        auth_mgr_logout_all();
        ESP_LOGI(TAG, "All sessions invalidated due to credential change");
    } else {
        ESP_LOGW(TAG, "No credential changes to save");
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

// ============================================================================
// Home Assistant Integration Authentication
// ============================================================================

bool auth_mgr_has_integration_token(void)
{
    portENTER_CRITICAL(&integration_token_lock);
    const bool configured = state.integration_token_set;
    portEXIT_CRITICAL(&integration_token_lock);
    return configured;
}

bool auth_mgr_issue_integration_token(char* buffer)
{
    if (buffer == NULL) {
        return false;
    }

    char token[AUTH_INTEGRATION_TOKEN_LEN + 1];
    uint8_t token_hash[sizeof(state.integration_token_hash)];
    generate_random_hex(token, AUTH_INTEGRATION_TOKEN_LEN);
    hash_password(token, token_hash);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY_INTEGRATION_HASH,
                           token_hash, sizeof(token_hash));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist integration token hash: %s",
                 esp_err_to_name(err));
        buffer[0] = '\0';
        mbedtls_platform_zeroize(token, sizeof(token));
        mbedtls_platform_zeroize(token_hash, sizeof(token_hash));
        return false;
    }

    portENTER_CRITICAL(&integration_token_lock);
    memcpy(state.integration_token_hash, token_hash, sizeof(token_hash));
    state.integration_token_set = true;
    state.integration_generation++;
    portEXIT_CRITICAL(&integration_token_lock);
    memcpy(buffer, token, sizeof(token));
    mbedtls_platform_zeroize(token, sizeof(token));
    mbedtls_platform_zeroize(token_hash, sizeof(token_hash));
    ESP_LOGI(TAG, "Integration token issued");
    return true;
}

bool auth_mgr_revoke_integration_token(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, NVS_KEY_INTEGRATION_HASH);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to revoke integration token: %s",
                 esp_err_to_name(err));
        return false;
    }

    portENTER_CRITICAL(&integration_token_lock);
    mbedtls_platform_zeroize(state.integration_token_hash,
                             sizeof(state.integration_token_hash));
    state.integration_token_set = false;
    state.integration_generation++;
    portEXIT_CRITICAL(&integration_token_lock);
    ESP_LOGI(TAG, "Integration token revoked");
    return true;
}

bool auth_mgr_validate_integration_token(const char* token)
{
    return auth_mgr_validate_integration_token_with_generation(token, NULL);
}

bool auth_mgr_validate_integration_token_with_generation(
    const char* token,
    uint32_t* generation_out)
{
    if (token == NULL || strlen(token) != AUTH_INTEGRATION_TOKEN_LEN) {
        return false;
    }

    uint8_t token_hash[sizeof(state.integration_token_hash)];
    uint8_t expected_hash[sizeof(state.integration_token_hash)];
    portENTER_CRITICAL(&integration_token_lock);
    const bool configured = state.integration_token_set;
    const uint32_t generation = state.integration_generation;
    memcpy(expected_hash, state.integration_token_hash, sizeof(expected_hash));
    portEXIT_CRITICAL(&integration_token_lock);
    if (!configured) {
        mbedtls_platform_zeroize(expected_hash, sizeof(expected_hash));
        return false;
    }

    hash_password(token, token_hash);
    const bool valid = constant_time_equal(
        token_hash, expected_hash, sizeof(token_hash));
    mbedtls_platform_zeroize(token_hash, sizeof(token_hash));
    mbedtls_platform_zeroize(expected_hash, sizeof(expected_hash));
    if (valid && generation_out != NULL) {
        *generation_out = generation;
    }
    return valid;
}

uint32_t auth_mgr_get_integration_generation(void)
{
    portENTER_CRITICAL(&integration_token_lock);
    const uint32_t generation = state.integration_generation;
    portEXIT_CRITICAL(&integration_token_lock);
    return generation;
}
