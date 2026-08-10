/*
 * Arctic Heat Pump Controller
 * TLS Certificate Manager — NVS storage and runtime provisioning
 *
 * Certificates are stored in NVS and loaded at boot.  If present the API
 * server starts in HTTPS mode (port 443); otherwise it falls back to
 * plain HTTP (port 80).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum PEM sizes — Let's Encrypt fullchain is ~3.2 KB, key ~250 B */
#define TLS_MAX_CERT_LEN 4000
#define TLS_MAX_KEY_LEN  2000
#define TLS_SHA256_FINGERPRINT_HEX_LEN 64

/**
 * @brief Initialise the TLS manager (loads certs from NVS if available)
 */
void tls_mgr_init(void);

/**
 * @brief Check whether TLS certificates have been provisioned
 * @return true if both cert and key are available
 */
bool tls_mgr_has_certs(void);

/**
 * @brief Check whether the server is currently running in HTTPS mode
 * @return true if HTTPS is active for this boot
 */
bool tls_mgr_is_https_active(void);

/**
 * @brief Mark HTTPS as active (called by api_server after successful start)
 */
void tls_mgr_set_https_active(bool active);

/**
 * @brief Get the certificate chain (PEM, null-terminated)
 * @param[out] out_len  receives the length including null terminator
 * @return pointer to cert data, or NULL if not provisioned
 */
const uint8_t* tls_mgr_get_cert(size_t* out_len);

/**
 * @brief Get the private key (PEM, null-terminated)
 * @param[out] out_len  receives the length including null terminator
 * @return pointer to key data, or NULL if not provisioned
 */
const uint8_t* tls_mgr_get_key(size_t* out_len);

/**
 * @brief Store a certificate + private key in NVS
 *
 * Both must be PEM-encoded, null-terminated strings.
 * A reboot is required for the new certs to take effect.
 *
 * @param cert      PEM certificate chain
 * @param cert_len  length including null terminator
 * @param key       PEM private key
 * @param key_len   length including null terminator
 * @return true on success
 */
bool tls_mgr_store_certs(const char* cert, size_t cert_len,
                         const char* key, size_t key_len);

/**
 * @brief Remove stored certificates from NVS
 *
 * After clearing, the next boot will fall back to HTTP.
 *
 * @return true on success
 */
bool tls_mgr_clear_certs(void);

/**
 * @brief Check whether the automatic integration identity is available.
 */
bool tls_mgr_has_identity(void);

/**
 * @brief Get the automatic integration identity certificate and key.
 */
const uint8_t* tls_mgr_get_identity_cert(size_t* out_len);
const uint8_t* tls_mgr_get_identity_key(size_t* out_len);

/**
 * @brief Get the SHA-256 fingerprint of the integration certificate DER.
 *
 * @param buffer Receives 64 lowercase hex characters plus null terminator.
 */
bool tls_mgr_get_identity_fingerprint(
    char buffer[TLS_SHA256_FINGERPRINT_HEX_LEN + 1]);

#ifdef __cplusplus
}
#endif
