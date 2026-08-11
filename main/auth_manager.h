/*
 * Arctic Heat Pump Controller
 * Authentication Manager - Session and API Key Management
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Session token length (32 hex chars = 128 bits)
#define AUTH_SESSION_TOKEN_LEN 32
// API key length (32 hex chars)
#define AUTH_API_KEY_LEN 32
// Home Assistant integration token length (64 hex chars = 256 bits)
#define AUTH_INTEGRATION_TOKEN_LEN 64
// Maximum concurrent sessions
#define AUTH_MAX_SESSIONS 4
// Session lifetime in seconds (7 days)
#define AUTH_SESSION_LIFETIME_SEC (7 * 24 * 60 * 60)
// Maximum username length
#define AUTH_MAX_USERNAME_LEN 32
// Maximum password length
#define AUTH_MAX_PASSWORD_LEN 64

/**
 * @brief Initialize authentication manager
 *        Loads settings from NVS
 */
void auth_mgr_init(void);

// ============================================================================
// Web Authentication (username/password + session)
// ============================================================================

/**
 * @brief Check if web authentication is enabled
 * @return true if web auth is required
 */
bool auth_mgr_web_auth_enabled(void);

/**
 * @brief Enable or disable web authentication
 * @param enabled true to enable, false to disable
 */
void auth_mgr_set_web_auth_enabled(bool enabled);

/**
 * @brief Set web authentication credentials
 * @param username Username (NULL to keep existing)
 * @param password Password (NULL to keep existing, will be hashed)
 * @return true on success
 */
bool auth_mgr_set_credentials(const char* username, const char* password);

/**
 * @brief Check whether the factory credentials still need replacement.
 */
bool auth_mgr_credentials_change_required(void);

/**
 * @brief Get the current username
 * @return Username string (empty if not set)
 */
const char* auth_mgr_get_username(void);

/**
 * @brief Validate username/password and create session
 * @param username Username to validate
 * @param password Password to validate
 * @param session_token Buffer to receive session token (must be AUTH_SESSION_TOKEN_LEN+1)
 * @return true if credentials valid and session created
 */
bool auth_mgr_login(const char* username, const char* password, char* session_token);

/**
 * @brief Validate a session token
 * @param token Session token to validate
 * @return true if session is valid and not expired
 */
bool auth_mgr_validate_session(const char* token);

/**
 * @brief Invalidate/logout a session
 * @param token Session token to invalidate
 */
void auth_mgr_logout(const char* token);

/**
 * @brief Invalidate all sessions (force re-login)
 */
void auth_mgr_logout_all(void);

// ============================================================================
// API Key Authentication
// ============================================================================

/**
 * @brief Check if API key authentication is enabled.
 * @return true; remote API authentication is mandatory.
 */
bool auth_mgr_api_auth_enabled(void);

/**
 * @brief Set API key authentication.
 * @param enabled must be true; disabling is rejected.
 */
void auth_mgr_set_api_auth_enabled(bool enabled);

/**
 * @brief Get the current API key
 * @param buffer Buffer to receive key (must be AUTH_API_KEY_LEN+1)
 * @return true if API key exists
 */
bool auth_mgr_get_api_key(char* buffer);

/**
 * @brief Regenerate the API key
 * @param buffer Buffer to receive new key (must be AUTH_API_KEY_LEN+1)
 * @return true on success
 */
bool auth_mgr_regenerate_api_key(char* buffer);

/**
 * @brief Validate an API key
 * @param key API key to validate
 * @return true if key matches
 */
bool auth_mgr_validate_api_key(const char* key);

// ============================================================================
// Home Assistant Integration Authentication
// ============================================================================

/**
 * @brief Check whether a dedicated integration credential has been paired.
 */
bool auth_mgr_has_integration_token(void);

/**
 * @brief Generate and persist a new integration credential.
 *
 * The plaintext token is returned once and only its SHA-256 hash is stored.
 * This function must only be called from a physically authorized pairing flow.
 *
 * @param buffer Buffer to receive the token (AUTH_INTEGRATION_TOKEN_LEN+1).
 * @return true when the hash was committed to NVS.
 */
bool auth_mgr_issue_integration_token(char* buffer);

/**
 * @brief Revoke the current integration credential.
 * @return true when revocation was committed to NVS.
 */
bool auth_mgr_revoke_integration_token(void);

/**
 * @brief Strictly validate an integration credential.
 *
 * Unlike the legacy API key helper, this never bypasses authentication based
 * on web/API authentication settings.
 */
bool auth_mgr_validate_integration_token(const char* token);

/**
 * @brief Validate a token and return the credential generation atomically.
 *
 * WebSocket sessions retain this generation and are disconnected immediately
 * when pairing rotation or revocation advances it.
 */
bool auth_mgr_validate_integration_token_with_generation(
    const char* token,
    uint32_t* generation_out);

/**
 * @brief Current in-memory integration credential generation.
 */
uint32_t auth_mgr_get_integration_generation(void);

#ifdef __cplusplus
}
#endif
