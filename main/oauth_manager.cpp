/*
 * Arctic Heat Pump Controller
 * OAuth 2.1 Manager - JWT Validation Implementation
 */
#include "oauth_manager.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <mbedtls/rsa.h>
#include <mbedtls/error.h>
#include <cJSON.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static const char* TAG = "oauth";

// NVS namespace and keys
static const char* NVS_NAMESPACE = "oauth";
static const char* NVS_KEY_ENABLED = "enabled";
static const char* NVS_KEY_ISSUER = "issuer";
static const char* NVS_KEY_AUDIENCE = "audience";
static const char* NVS_KEY_JWKS_URI = "jwks_uri";
static const char* NVS_KEY_FALLBACK = "fallback";

// ============================================================================
// JWKS Key Storage
// ============================================================================

typedef struct {
    char kid[64];                 // Key ID
    mbedtls_pk_context pk;        // Public key context
    bool valid;                   // Key is loaded and valid
} jwks_key_t;

// Local state
static struct {
    bool initialized;
    oauth_config_t config;
    jwks_key_t keys[OAUTH_MAX_JWKS_KEYS];
    int num_keys;
    time_t jwks_fetched_at;
} state = {};

// ============================================================================
// Base64URL Decoding
// ============================================================================

// Base64URL to Base64 conversion (replace - with +, _ with /)
static void base64url_to_base64(char* str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '-') str[i] = '+';
        else if (str[i] == '_') str[i] = '/';
    }
}

// Decode Base64URL to binary
static int base64url_decode(const char* input, size_t input_len, 
                            uint8_t* output, size_t output_size, size_t* output_len)
{
    // Copy and convert to standard Base64
    char* b64 = (char*)malloc(input_len + 4);
    if (!b64) return -1;
    
    memcpy(b64, input, input_len);
    base64url_to_base64(b64, input_len);
    
    // Add padding if needed
    size_t pad_len = (4 - (input_len % 4)) % 4;
    for (size_t i = 0; i < pad_len; i++) {
        b64[input_len + i] = '=';
    }
    b64[input_len + pad_len] = '\0';
    
    // Decode
    int ret = mbedtls_base64_decode(output, output_size, output_len,
                                     (const unsigned char*)b64, input_len + pad_len);
    free(b64);
    return ret;
}

// ============================================================================
// Scope Handling
// ============================================================================

static const struct {
    oauth_scope_t scope;
    const char* name;
} SCOPE_NAMES[] = {
    { OAUTH_SCOPE_STATUS,  "arctic:status" },
    { OAUTH_SCOPE_CONTROL, "arctic:control" },
    { OAUTH_SCOPE_CONFIG,  "arctic:config" },
    { OAUTH_SCOPE_PARAMS,  "arctic:params" },
    { OAUTH_SCOPE_ADMIN,   "arctic:admin" },
    { OAUTH_SCOPE_MCP,     "arctic:mcp" },
};

static const int NUM_SCOPES = sizeof(SCOPE_NAMES) / sizeof(SCOPE_NAMES[0]);

uint32_t oauth_mgr_parse_scopes(const char* scope_string)
{
    if (!scope_string || scope_string[0] == '\0') {
        return OAUTH_SCOPE_NONE;
    }
    
    uint32_t scopes = OAUTH_SCOPE_NONE;
    
    // Parse space-separated scopes
    char* copy = strdup(scope_string);
    if (!copy) return OAUTH_SCOPE_NONE;
    
    char* token = strtok(copy, " ");
    while (token) {
        for (int i = 0; i < NUM_SCOPES; i++) {
            if (strcmp(token, SCOPE_NAMES[i].name) == 0) {
                scopes |= SCOPE_NAMES[i].scope;
                break;
            }
        }
        token = strtok(NULL, " ");
    }
    
    free(copy);
    return scopes;
}

const char* oauth_mgr_scope_name(oauth_scope_t scope)
{
    for (int i = 0; i < NUM_SCOPES; i++) {
        if (SCOPE_NAMES[i].scope == scope) {
            return SCOPE_NAMES[i].name;
        }
    }
    return NULL;
}

bool oauth_mgr_check_scopes(const oauth_claims_t* claims, uint32_t required_scopes)
{
    if (!claims) return false;
    return (claims->scopes & required_scopes) == required_scopes;
}

// ============================================================================
// NVS Persistence
// ============================================================================

static bool load_config_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No OAuth config in NVS");
        return false;
    }
    
    uint8_t enabled = 0;
    if (nvs_get_u8(nvs, NVS_KEY_ENABLED, &enabled) == ESP_OK) {
        state.config.enabled = enabled != 0;
    }
    
    uint8_t fallback = 1;  // Default: allow fallback
    if (nvs_get_u8(nvs, NVS_KEY_FALLBACK, &fallback) == ESP_OK) {
        state.config.allow_api_key_fallback = fallback != 0;
    }
    
    size_t len = sizeof(state.config.issuer);
    nvs_get_str(nvs, NVS_KEY_ISSUER, state.config.issuer, &len);
    
    len = sizeof(state.config.audience);
    nvs_get_str(nvs, NVS_KEY_AUDIENCE, state.config.audience, &len);
    
    len = sizeof(state.config.jwks_uri);
    nvs_get_str(nvs, NVS_KEY_JWKS_URI, state.config.jwks_uri, &len);
    
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Loaded OAuth config: enabled=%d, issuer=%s", 
             state.config.enabled, state.config.issuer);
    return true;
}

static bool save_config_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    nvs_set_u8(nvs, NVS_KEY_ENABLED, state.config.enabled ? 1 : 0);
    nvs_set_u8(nvs, NVS_KEY_FALLBACK, state.config.allow_api_key_fallback ? 1 : 0);
    nvs_set_str(nvs, NVS_KEY_ISSUER, state.config.issuer);
    nvs_set_str(nvs, NVS_KEY_AUDIENCE, state.config.audience);
    nvs_set_str(nvs, NVS_KEY_JWKS_URI, state.config.jwks_uri);
    
    nvs_commit(nvs);
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Saved OAuth config");
    return true;
}

// ============================================================================
// Initialization
// ============================================================================

void oauth_mgr_init(void)
{
    if (state.initialized) return;
    
    ESP_LOGI(TAG, "Initializing OAuth manager");
    
    // Initialize all keys
    for (int i = 0; i < OAUTH_MAX_JWKS_KEYS; i++) {
        mbedtls_pk_init(&state.keys[i].pk);
        state.keys[i].valid = false;
    }
    
    // Load config from NVS
    load_config_from_nvs();
    
    state.initialized = true;
}

void oauth_mgr_get_config(oauth_config_t* config)
{
    if (config) {
        *config = state.config;
    }
}

bool oauth_mgr_set_config(const oauth_config_t* config)
{
    if (!config) return false;
    
    state.config = *config;
    
    // Clear cached JWKS when config changes
    for (int i = 0; i < state.num_keys; i++) {
        mbedtls_pk_free(&state.keys[i].pk);
        mbedtls_pk_init(&state.keys[i].pk);
        state.keys[i].valid = false;
    }
    state.num_keys = 0;
    state.jwks_fetched_at = 0;
    
    return save_config_to_nvs();
}

bool oauth_mgr_is_enabled(void)
{
    return state.config.enabled;
}

void oauth_mgr_set_enabled(bool enabled)
{
    if (state.config.enabled != enabled) {
        state.config.enabled = enabled;
        save_config_to_nvs();
    }
}

// ============================================================================
// JWKS Fetching
// ============================================================================

// HTTP response buffer for JWKS
static char* jwks_response_buf = NULL;
static int jwks_response_len = 0;
static const int JWKS_MAX_SIZE = 16384;  // 16 KB max JWKS response

static esp_err_t jwks_http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (jwks_response_buf && jwks_response_len + evt->data_len < JWKS_MAX_SIZE) {
                    memcpy(jwks_response_buf + jwks_response_len, evt->data, evt->data_len);
                    jwks_response_len += evt->data_len;
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Parse RSA key from JWK
static bool parse_jwk_rsa(const cJSON* jwk, jwks_key_t* key)
{
    const cJSON* kty = cJSON_GetObjectItem(jwk, "kty");
    if (!kty || strcmp(kty->valuestring, "RSA") != 0) {
        return false;
    }
    
    const cJSON* kid = cJSON_GetObjectItem(jwk, "kid");
    const cJSON* n = cJSON_GetObjectItem(jwk, "n");
    const cJSON* e = cJSON_GetObjectItem(jwk, "e");
    
    if (!n || !e || !n->valuestring || !e->valuestring) {
        ESP_LOGW(TAG, "JWK missing n or e");
        return false;
    }
    
    // Store key ID
    if (kid && kid->valuestring) {
        strncpy(key->kid, kid->valuestring, sizeof(key->kid) - 1);
    } else {
        key->kid[0] = '\0';
    }
    
    // Decode n (modulus) and e (exponent)
    uint8_t n_buf[512], e_buf[8];
    size_t n_len = 0, e_len = 0;
    
    if (base64url_decode(n->valuestring, strlen(n->valuestring), n_buf, sizeof(n_buf), &n_len) != 0) {
        ESP_LOGW(TAG, "Failed to decode JWK modulus");
        return false;
    }
    
    if (base64url_decode(e->valuestring, strlen(e->valuestring), e_buf, sizeof(e_buf), &e_len) != 0) {
        ESP_LOGW(TAG, "Failed to decode JWK exponent");
        return false;
    }
    
    // Initialize RSA context
    mbedtls_pk_free(&key->pk);
    mbedtls_pk_init(&key->pk);
    
    int ret = mbedtls_pk_setup(&key->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) {
        ESP_LOGW(TAG, "Failed to setup PK context: %d", ret);
        return false;
    }
    
    mbedtls_rsa_context* rsa = mbedtls_pk_rsa(key->pk);
    
    // Import n and e
    ret = mbedtls_rsa_import_raw(rsa, n_buf, n_len, NULL, 0, NULL, 0, NULL, 0, e_buf, e_len);
    if (ret != 0) {
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGW(TAG, "Failed to import RSA key: %s", err_buf);
        return false;
    }
    
    ret = mbedtls_rsa_complete(rsa);
    if (ret != 0) {
        ESP_LOGW(TAG, "Failed to complete RSA key: %d", ret);
        return false;
    }
    
    key->valid = true;
    ESP_LOGI(TAG, "Loaded RSA key kid=%s", key->kid);
    return true;
}

bool oauth_mgr_refresh_jwks(void)
{
    if (state.config.jwks_uri[0] == '\0') {
        ESP_LOGW(TAG, "No JWKS URI configured");
        return false;
    }
    
    ESP_LOGI(TAG, "Fetching JWKS from %s", state.config.jwks_uri);
    
    // Allocate response buffer
    jwks_response_buf = (char*)malloc(JWKS_MAX_SIZE);
    if (!jwks_response_buf) {
        ESP_LOGE(TAG, "Failed to allocate JWKS buffer");
        return false;
    }
    jwks_response_len = 0;
    
    // Configure HTTP client
    esp_http_client_config_t http_config = {};
    http_config.url = state.config.jwks_uri;
    http_config.event_handler = jwks_http_event_handler;
    http_config.timeout_ms = 10000;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(jwks_response_buf);
        jwks_response_buf = NULL;
        return false;
    }
    
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "JWKS fetch failed: err=%s, status=%d", esp_err_to_name(err), status);
        free(jwks_response_buf);
        jwks_response_buf = NULL;
        return false;
    }
    
    // Null-terminate response
    jwks_response_buf[jwks_response_len] = '\0';
    
    // Parse JWKS JSON
    cJSON* jwks = cJSON_Parse(jwks_response_buf);
    free(jwks_response_buf);
    jwks_response_buf = NULL;
    
    if (!jwks) {
        ESP_LOGE(TAG, "Failed to parse JWKS JSON");
        return false;
    }
    
    // Clear existing keys
    for (int i = 0; i < state.num_keys; i++) {
        mbedtls_pk_free(&state.keys[i].pk);
        mbedtls_pk_init(&state.keys[i].pk);
        state.keys[i].valid = false;
    }
    state.num_keys = 0;
    
    // Parse keys array
    cJSON* keys = cJSON_GetObjectItem(jwks, "keys");
    if (!cJSON_IsArray(keys)) {
        ESP_LOGE(TAG, "JWKS missing 'keys' array");
        cJSON_Delete(jwks);
        return false;
    }
    
    int num_keys = cJSON_GetArraySize(keys);
    ESP_LOGI(TAG, "JWKS contains %d keys", num_keys);
    
    for (int i = 0; i < num_keys && state.num_keys < OAUTH_MAX_JWKS_KEYS; i++) {
        cJSON* jwk = cJSON_GetArrayItem(keys, i);
        if (parse_jwk_rsa(jwk, &state.keys[state.num_keys])) {
            state.num_keys++;
        }
    }
    
    cJSON_Delete(jwks);
    state.jwks_fetched_at = time(NULL);
    
    ESP_LOGI(TAG, "Loaded %d signing keys", state.num_keys);
    return state.num_keys > 0;
}

bool oauth_mgr_has_valid_jwks(void)
{
    return state.num_keys > 0 && oauth_mgr_jwks_ttl() > 0;
}

int oauth_mgr_jwks_ttl(void)
{
    if (state.jwks_fetched_at == 0) return 0;
    
    time_t now = time(NULL);
    time_t expires_at = state.jwks_fetched_at + OAUTH_JWKS_CACHE_TTL;
    
    if (now >= expires_at) return 0;
    return (int)(expires_at - now);
}

// ============================================================================
// JWT Validation
// ============================================================================

// Find key by kid
static jwks_key_t* find_key(const char* kid)
{
    // If kid is NULL or empty, return first valid key
    if (!kid || kid[0] == '\0') {
        for (int i = 0; i < state.num_keys; i++) {
            if (state.keys[i].valid) {
                return &state.keys[i];
            }
        }
        return NULL;
    }
    
    // Find by kid
    for (int i = 0; i < state.num_keys; i++) {
        if (state.keys[i].valid && strcmp(state.keys[i].kid, kid) == 0) {
            return &state.keys[i];
        }
    }
    return NULL;
}

// Verify RS256 signature
static bool verify_rs256(const char* signing_input, size_t signing_input_len,
                         const uint8_t* signature, size_t signature_len,
                         jwks_key_t* key)
{
    // Hash the signing input with SHA-256
    uint8_t hash[32];
    mbedtls_md_context_t md_ctx;
    mbedtls_md_init(&md_ctx);
    
    int ret = mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    if (ret != 0) {
        mbedtls_md_free(&md_ctx);
        return false;
    }
    
    mbedtls_md_starts(&md_ctx);
    mbedtls_md_update(&md_ctx, (const unsigned char*)signing_input, signing_input_len);
    mbedtls_md_finish(&md_ctx, hash);
    mbedtls_md_free(&md_ctx);
    
    // Verify signature
    ret = mbedtls_pk_verify(&key->pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), 
                            signature, signature_len);
    
    if (ret != 0) {
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGD(TAG, "Signature verification failed: %s", err_buf);
        return false;
    }
    
    return true;
}

bool oauth_mgr_validate_token(const char* token, oauth_claims_t* claims_out)
{
    if (!token || token[0] == '\0') {
        return false;
    }
    
    // Ensure JWKS is loaded
    if (!oauth_mgr_has_valid_jwks()) {
        ESP_LOGI(TAG, "JWKS expired or not loaded, refreshing...");
        if (!oauth_mgr_refresh_jwks()) {
            ESP_LOGW(TAG, "Failed to refresh JWKS");
            return false;
        }
    }
    
    // Find the two dots separating header.payload.signature
    const char* dot1 = strchr(token, '.');
    if (!dot1) {
        ESP_LOGD(TAG, "Invalid JWT: missing first dot");
        return false;
    }
    
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) {
        ESP_LOGD(TAG, "Invalid JWT: missing second dot");
        return false;
    }
    
    size_t header_b64_len = dot1 - token;
    size_t payload_b64_len = dot2 - dot1 - 1;
    size_t sig_b64_len = strlen(dot2 + 1);
    
    // Decode header
    uint8_t header_buf[512];
    size_t header_len;
    if (base64url_decode(token, header_b64_len, header_buf, sizeof(header_buf) - 1, &header_len) != 0) {
        ESP_LOGD(TAG, "Failed to decode JWT header");
        return false;
    }
    header_buf[header_len] = '\0';
    
    // Parse header JSON
    cJSON* header = cJSON_Parse((char*)header_buf);
    if (!header) {
        ESP_LOGD(TAG, "Failed to parse JWT header JSON");
        return false;
    }
    
    // Check algorithm
    cJSON* alg = cJSON_GetObjectItem(header, "alg");
    if (!alg || strcmp(alg->valuestring, "RS256") != 0) {
        ESP_LOGW(TAG, "Unsupported JWT algorithm: %s", alg ? alg->valuestring : "null");
        cJSON_Delete(header);
        return false;
    }
    
    // Get key ID
    cJSON* kid = cJSON_GetObjectItem(header, "kid");
    const char* kid_str = (kid && kid->valuestring) ? kid->valuestring : NULL;
    cJSON_Delete(header);
    
    // Find signing key
    jwks_key_t* key = find_key(kid_str);
    if (!key) {
        ESP_LOGW(TAG, "No matching signing key for kid=%s", kid_str ? kid_str : "(none)");
        return false;
    }
    
    // Decode signature
    uint8_t sig_buf[512];
    size_t sig_len;
    if (base64url_decode(dot2 + 1, sig_b64_len, sig_buf, sizeof(sig_buf), &sig_len) != 0) {
        ESP_LOGD(TAG, "Failed to decode JWT signature");
        return false;
    }
    
    // Verify signature (over header.payload)
    size_t signing_input_len = dot2 - token;
    if (!verify_rs256(token, signing_input_len, sig_buf, sig_len, key)) {
        ESP_LOGW(TAG, "JWT signature verification failed");
        return false;
    }
    
    // Decode payload
    uint8_t payload_buf[2048];
    size_t payload_len;
    if (base64url_decode(dot1 + 1, payload_b64_len, payload_buf, sizeof(payload_buf) - 1, &payload_len) != 0) {
        ESP_LOGD(TAG, "Failed to decode JWT payload");
        return false;
    }
    payload_buf[payload_len] = '\0';
    
    // Parse payload JSON
    cJSON* payload = cJSON_Parse((char*)payload_buf);
    if (!payload) {
        ESP_LOGD(TAG, "Failed to parse JWT payload JSON");
        return false;
    }
    
    // Validate issuer
    cJSON* iss = cJSON_GetObjectItem(payload, "iss");
    if (state.config.issuer[0] != '\0') {
        if (!iss || !iss->valuestring || strcmp(iss->valuestring, state.config.issuer) != 0) {
            ESP_LOGW(TAG, "JWT issuer mismatch: expected=%s, got=%s", 
                     state.config.issuer, iss ? iss->valuestring : "null");
            cJSON_Delete(payload);
            return false;
        }
    }
    
    // Validate audience
    cJSON* aud = cJSON_GetObjectItem(payload, "aud");
    if (state.config.audience[0] != '\0') {
        bool aud_match = false;
        if (cJSON_IsString(aud)) {
            aud_match = (strcmp(aud->valuestring, state.config.audience) == 0);
        } else if (cJSON_IsArray(aud)) {
            int aud_count = cJSON_GetArraySize(aud);
            for (int i = 0; i < aud_count; i++) {
                cJSON* aud_item = cJSON_GetArrayItem(aud, i);
                if (cJSON_IsString(aud_item) && strcmp(aud_item->valuestring, state.config.audience) == 0) {
                    aud_match = true;
                    break;
                }
            }
        }
        if (!aud_match) {
            ESP_LOGW(TAG, "JWT audience mismatch");
            cJSON_Delete(payload);
            return false;
        }
    }
    
    // Validate expiration
    cJSON* exp = cJSON_GetObjectItem(payload, "exp");
    time_t now = time(NULL);
    if (exp && cJSON_IsNumber(exp)) {
        if (now > (time_t)exp->valuedouble) {
            ESP_LOGW(TAG, "JWT expired");
            cJSON_Delete(payload);
            return false;
        }
    }
    
    // Validate not-before
    cJSON* nbf = cJSON_GetObjectItem(payload, "nbf");
    if (nbf && cJSON_IsNumber(nbf)) {
        if (now < (time_t)nbf->valuedouble) {
            ESP_LOGW(TAG, "JWT not yet valid (nbf)");
            cJSON_Delete(payload);
            return false;
        }
    }
    
    // Extract claims if requested
    if (claims_out) {
        memset(claims_out, 0, sizeof(*claims_out));
        
        cJSON* sub = cJSON_GetObjectItem(payload, "sub");
        if (sub && sub->valuestring) {
            strncpy(claims_out->subject, sub->valuestring, sizeof(claims_out->subject) - 1);
        }
        
        cJSON* azp = cJSON_GetObjectItem(payload, "azp");
        if (!azp) azp = cJSON_GetObjectItem(payload, "client_id");
        if (azp && azp->valuestring) {
            strncpy(claims_out->client_id, azp->valuestring, sizeof(claims_out->client_id) - 1);
        }
        
        if (exp) claims_out->expires_at = (int64_t)exp->valuedouble;
        
        cJSON* iat = cJSON_GetObjectItem(payload, "iat");
        if (iat) claims_out->issued_at = (int64_t)iat->valuedouble;
        
        // Parse scopes - check both "scope" (OAuth2) and "scp" (Azure AD)
        cJSON* scope = cJSON_GetObjectItem(payload, "scope");
        if (!scope) scope = cJSON_GetObjectItem(payload, "scp");
        if (scope && scope->valuestring) {
            claims_out->scopes = oauth_mgr_parse_scopes(scope->valuestring);
        }
        
        // Some IdPs use "scopes" array
        cJSON* scopes_arr = cJSON_GetObjectItem(payload, "scopes");
        if (scopes_arr && cJSON_IsArray(scopes_arr)) {
            char scope_str[512] = {0};
            int len = 0;
            int arr_size = cJSON_GetArraySize(scopes_arr);
            for (int i = 0; i < arr_size && len < (int)sizeof(scope_str) - 64; i++) {
                cJSON* s = cJSON_GetArrayItem(scopes_arr, i);
                if (s && s->valuestring) {
                    if (len > 0) scope_str[len++] = ' ';
                    strcpy(scope_str + len, s->valuestring);
                    len += strlen(s->valuestring);
                }
            }
            if (len > 0) {
                claims_out->scopes |= oauth_mgr_parse_scopes(scope_str);
            }
        }
    }
    
    cJSON_Delete(payload);
    
    ESP_LOGD(TAG, "JWT validated successfully");
    return true;
}

uint32_t oauth_mgr_validate_bearer(const char* auth_header, oauth_claims_t* claims_out)
{
    if (!auth_header) return OAUTH_SCOPE_NONE;
    
    // Check for "Bearer " prefix
    if (strncasecmp(auth_header, "Bearer ", 7) != 0) {
        return OAUTH_SCOPE_NONE;
    }
    
    const char* token = auth_header + 7;
    
    // Skip whitespace
    while (*token == ' ') token++;
    
    if (*token == '\0') {
        return OAUTH_SCOPE_NONE;
    }
    
    oauth_claims_t claims;
    if (!oauth_mgr_validate_token(token, &claims)) {
        return OAUTH_SCOPE_NONE;
    }
    
    if (claims_out) {
        *claims_out = claims;
    }
    
    return claims.scopes;
}
