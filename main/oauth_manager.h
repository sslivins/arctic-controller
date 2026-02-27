/*
 * Arctic Heat Pump Controller
 * OAuth 2.1 Manager - JWT Validation with External Identity Provider
 * 
 * Validates Bearer tokens (JWTs) from external OAuth 2.1 providers like
 * Zitadel or Keycloak. Supports scope-based access control for granular
 * permissions on API and MCP endpoints.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Scope Definitions
// ============================================================================

// Scope bits for efficient permission checking
typedef enum {
    OAUTH_SCOPE_NONE         = 0,
    OAUTH_SCOPE_STATUS       = (1 << 0),  // arctic:status - Read status, temps, mode, errors
    OAUTH_SCOPE_CONTROL      = (1 << 1),  // arctic:control - Set temp, mode, power on/off
    OAUTH_SCOPE_CONFIG       = (1 << 2),  // arctic:config - WiFi, timezone, preferences
    OAUTH_SCOPE_PARAMS       = (1 << 3),  // arctic:params - Read/write Modbus P-parameters
    OAUTH_SCOPE_ADMIN        = (1 << 4),  // arctic:admin - Firmware update, reboot, raw registers
    OAUTH_SCOPE_MCP          = (1 << 5),  // arctic:mcp - Access MCP endpoint (tools/resources)
} oauth_scope_t;

// Combined scope sets for common use cases
#define OAUTH_SCOPE_READ_ONLY   (OAUTH_SCOPE_STATUS)
#define OAUTH_SCOPE_OPERATOR    (OAUTH_SCOPE_STATUS | OAUTH_SCOPE_CONTROL)
#define OAUTH_SCOPE_FULL        (OAUTH_SCOPE_STATUS | OAUTH_SCOPE_CONTROL | OAUTH_SCOPE_CONFIG | \
                                 OAUTH_SCOPE_PARAMS | OAUTH_SCOPE_ADMIN | OAUTH_SCOPE_MCP)

// Maximum number of cached JWKS keys
#define OAUTH_MAX_JWKS_KEYS     8

// JWKS cache lifetime in seconds (24 hours)
#define OAUTH_JWKS_CACHE_TTL    (24 * 60 * 60)

// Maximum JWT token length
#define OAUTH_MAX_TOKEN_LEN     4096

// Maximum issuer/audience URL length
#define OAUTH_MAX_URL_LEN       256

// ============================================================================
// Configuration
// ============================================================================

/**
 * @brief OAuth configuration structure
 */
typedef struct {
    char issuer[OAUTH_MAX_URL_LEN];           // Token issuer (iss claim)
    char audience[OAUTH_MAX_URL_LEN];         // Expected audience (aud claim)
    char jwks_uri[OAUTH_MAX_URL_LEN];         // JWKS endpoint URL
    bool enabled;                              // Whether OAuth validation is enabled
    bool allow_api_key_fallback;              // Allow API key when OAuth fails
} oauth_config_t;

/**
 * @brief Validated token claims
 */
typedef struct {
    char subject[128];                         // sub claim (user/client ID)
    char client_id[128];                       // azp/client_id claim
    uint32_t scopes;                           // Parsed scopes as bitmask
    int64_t expires_at;                        // exp claim (Unix timestamp)
    int64_t issued_at;                         // iat claim
} oauth_claims_t;

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize OAuth manager
 *        Loads configuration from NVS
 */
void oauth_mgr_init(void);

/**
 * @brief Get current OAuth configuration
 * @param config Output configuration
 */
void oauth_mgr_get_config(oauth_config_t* config);

/**
 * @brief Set OAuth configuration
 * @param config New configuration (saved to NVS)
 * @return true on success
 */
bool oauth_mgr_set_config(const oauth_config_t* config);

/**
 * @brief Check if OAuth is enabled
 * @return true if OAuth validation is active
 */
bool oauth_mgr_is_enabled(void);

/**
 * @brief Enable or disable OAuth
 * @param enabled true to enable
 */
void oauth_mgr_set_enabled(bool enabled);

// ============================================================================
// JWKS Management
// ============================================================================

/**
 * @brief Refresh JWKS from the identity provider
 *        Called automatically when needed, but can be triggered manually
 * @return true if JWKS was successfully fetched
 */
bool oauth_mgr_refresh_jwks(void);

/**
 * @brief Check if JWKS is loaded and valid
 * @return true if we have valid signing keys
 */
bool oauth_mgr_has_valid_jwks(void);

/**
 * @brief Get time until JWKS cache expires
 * @return Seconds until expiry, or 0 if expired/invalid
 */
int oauth_mgr_jwks_ttl(void);

// ============================================================================
// Token Validation
// ============================================================================

/**
 * @brief Validate a Bearer token (JWT)
 * @param token The JWT string (without "Bearer " prefix)
 * @param claims_out Parsed claims if validation succeeds (can be NULL)
 * @return true if token is valid (signature, expiry, iss, aud all OK)
 */
bool oauth_mgr_validate_token(const char* token, oauth_claims_t* claims_out);

/**
 * @brief Check if a token has the required scope(s)
 * @param claims Validated token claims
 * @param required_scopes Scope bitmask to check
 * @return true if all required scopes are present
 */
bool oauth_mgr_check_scopes(const oauth_claims_t* claims, uint32_t required_scopes);

/**
 * @brief Parse scope string to bitmask
 * @param scope_string Space-separated scope string (e.g., "arctic:status arctic:control")
 * @return Scope bitmask
 */
uint32_t oauth_mgr_parse_scopes(const char* scope_string);

/**
 * @brief Get human-readable scope name
 * @param scope Single scope bit
 * @return Scope name (e.g., "arctic:status") or NULL
 */
const char* oauth_mgr_scope_name(oauth_scope_t scope);

// ============================================================================
// HTTP Helper
// ============================================================================

/**
 * @brief Extract and validate Bearer token from HTTP request
 * @param auth_header The Authorization header value
 * @param claims_out Parsed claims if validation succeeds (can be NULL)
 * @return Scope bitmask if valid, or 0 if invalid/missing
 * 
 * This parses "Bearer <token>" format and validates the JWT.
 */
uint32_t oauth_mgr_validate_bearer(const char* auth_header, oauth_claims_t* claims_out);

#ifdef __cplusplus
}
#endif
