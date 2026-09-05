/*
 * Arctic Heat Pump Controller
 * TLS Certificate Manager — NVS storage and runtime provisioning
 */
#include "tls_manager.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509.h>
#include <string.h>

static const char* TAG = "tls_mgr";

// NVS namespace and keys
static const char* NVS_NAMESPACE = "tls";
static const char* NVS_KEY_CERT  = "cert";
static const char* NVS_KEY_KEY   = "key";
static const char* NVS_KEY_IDENTITY_CERT = "id_cert";
static const char* NVS_KEY_IDENTITY_KEY = "id_key";

// Runtime state
static struct {
    bool initialised;
    bool has_certs;
    bool https_active;
    uint8_t cert[TLS_MAX_CERT_LEN];
    size_t  cert_len;                   // includes null terminator
    uint8_t key[TLS_MAX_KEY_LEN];
    size_t  key_len;                    // includes null terminator
    bool has_identity;
    uint8_t identity_cert[TLS_MAX_CERT_LEN];
    size_t identity_cert_len;
    uint8_t identity_key[TLS_MAX_KEY_LEN];
    size_t identity_key_len;
    uint8_t identity_fingerprint[32];
} state = {};

static bool calculate_identity_fingerprint(void)
{
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    const int ret = mbedtls_x509_crt_parse(
        &cert, state.identity_cert, state.identity_cert_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse identity certificate: -0x%04x", -ret);
        mbedtls_x509_crt_free(&cert);
        return false;
    }

    size_t fingerprint_len = 0;
    const psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256, cert.raw.p, cert.raw.len,
        state.identity_fingerprint, sizeof(state.identity_fingerprint),
        &fingerprint_len);
    mbedtls_x509_crt_free(&cert);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to hash identity certificate: %d", (int)status);
        return false;
    }
    return true;
}

static bool store_identity(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(
            nvs, NVS_KEY_IDENTITY_CERT, state.identity_cert,
            state.identity_cert_len);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(
            nvs, NVS_KEY_IDENTITY_KEY, state.identity_key,
            state.identity_key_len);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs != 0) {
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist integration identity: %s",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool generate_identity(void)
{
    int ret = 0;
    // Mbed TLS 4.x (ESP-IDF 6.x) makes entropy/ctr_drbg/ecp private; PSA Crypto
    // now provides both the RNG and key generation. PSA is initialized by
    // ESP-IDF during startup, so no psa_crypto_init() call is needed here.
    psa_status_t status = PSA_SUCCESS;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    mbedtls_pk_context key;
    mbedtls_x509write_cert cert;
    uint8_t serial[16];
    static const char* NAME = "CN=Arctic Controller Integration";

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&cert);

    // secp256r1 ECDSA-SHA256, matching the previous mbedtls_ecp_gen_key() key.
    // EXPORT is required so the private key can be written out as PEM below.
    psa_set_key_usage_flags(
        &attributes,
        PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH |
            PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(
        &attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);

    status = psa_generate_key(&attributes, &key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Identity key generation failed: %d", (int)status);
        ret = -1;
        goto cleanup;
    }

    // Copy the key material out of PSA; the PK context then owns an
    // independent copy that can be exported as PEM.
    ret = mbedtls_pk_copy_from_psa(key_id, &key);
    if (ret != 0) {
        ESP_LOGE(TAG, "Identity key import failed: -0x%04x", -ret);
        goto cleanup;
    }

    status = psa_generate_random(serial, sizeof(serial));
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Identity serial generation failed: %d", (int)status);
        ret = -1;
        goto cleanup;
    }
    serial[0] &= 0x7f;
    if (serial[0] == 0) {
        serial[0] = 1;
    }

    mbedtls_x509write_crt_set_version(
        &cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    if ((ret = mbedtls_x509write_crt_set_subject_name(&cert, NAME)) != 0 ||
        (ret = mbedtls_x509write_crt_set_issuer_name(&cert, NAME)) != 0 ||
        (ret = mbedtls_x509write_crt_set_serial_raw(
             &cert, serial, sizeof(serial))) != 0 ||
        (ret = mbedtls_x509write_crt_set_validity(
             &cert, "20200101000000", "20491231235959")) != 0 ||
        (ret = mbedtls_x509write_crt_set_basic_constraints(
             &cert, 0, -1)) != 0 ||
        (ret = mbedtls_x509write_crt_set_key_usage(
             &cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) != 0) {
        ESP_LOGE(TAG, "Identity certificate setup failed: -0x%04x", -ret);
        goto cleanup;
    }

    memset(state.identity_cert, 0, sizeof(state.identity_cert));
    ret = mbedtls_x509write_crt_pem(
        &cert, state.identity_cert, sizeof(state.identity_cert));
    if (ret != 0) {
        ESP_LOGE(TAG, "Identity certificate encoding failed: -0x%04x", -ret);
        goto cleanup;
    }
    state.identity_cert_len =
        strlen(reinterpret_cast<const char*>(state.identity_cert)) + 1;

    memset(state.identity_key, 0, sizeof(state.identity_key));
    ret = mbedtls_pk_write_key_pem(
        &key, state.identity_key, sizeof(state.identity_key));
    if (ret != 0) {
        ESP_LOGE(TAG, "Identity key encoding failed: -0x%04x", -ret);
        goto cleanup;
    }
    state.identity_key_len =
        strlen(reinterpret_cast<const char*>(state.identity_key)) + 1;

    if (!calculate_identity_fingerprint() || !store_identity()) {
        ret = -1;
        goto cleanup;
    }

    state.has_identity = true;
    ESP_LOGI(TAG, "Generated persistent integration TLS identity");

cleanup:
    mbedtls_x509write_crt_free(&cert);
    mbedtls_pk_free(&key);
    psa_destroy_key(key_id);
    if (ret != 0) {
        memset(state.identity_cert, 0, sizeof(state.identity_cert));
        memset(state.identity_key, 0, sizeof(state.identity_key));
        state.identity_cert_len = 0;
        state.identity_key_len = 0;
        state.has_identity = false;
        return false;
    }
    return true;
}

static bool load_identity(nvs_handle_t nvs)
{
    size_t len = sizeof(state.identity_cert);
    esp_err_t err = nvs_get_blob(
        nvs, NVS_KEY_IDENTITY_CERT, state.identity_cert, &len);
    if (err != ESP_OK) {
        return false;
    }
    state.identity_cert_len = len;

    len = sizeof(state.identity_key);
    err = nvs_get_blob(
        nvs, NVS_KEY_IDENTITY_KEY, state.identity_key, &len);
    if (err != ESP_OK) {
        state.identity_cert_len = 0;
        return false;
    }
    state.identity_key_len = len;
    if (!calculate_identity_fingerprint()) {
        state.identity_cert_len = 0;
        state.identity_key_len = 0;
        return false;
    }
    state.has_identity = true;
    return true;
}

// ============================================================================
// Init / load
// ============================================================================

void tls_mgr_init(void)
{
    if (state.initialised) return;
    state.initialised = true;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No TLS certs in NVS (err=%s)", esp_err_to_name(err));
        if (!generate_identity()) {
            ESP_LOGE(TAG, "Integration TLS identity is unavailable");
        }
        return;
    }

    const bool identity_loaded = load_identity(nvs);

    // Load certificate
    size_t len = sizeof(state.cert);
    err = nvs_get_blob(nvs, NVS_KEY_CERT, state.cert, &len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No TLS cert in NVS");
        nvs_close(nvs);
        if (!identity_loaded && !generate_identity()) {
            ESP_LOGE(TAG, "Integration TLS identity is unavailable");
        }
        return;
    }
    state.cert_len = len;

    // Load private key
    len = sizeof(state.key);
    err = nvs_get_blob(nvs, NVS_KEY_KEY, state.key, &len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No TLS key in NVS");
        state.cert_len = 0;
        nvs_close(nvs);
        if (!identity_loaded && !generate_identity()) {
            ESP_LOGE(TAG, "Integration TLS identity is unavailable");
        }
        return;
    }
    state.key_len = len;

    nvs_close(nvs);

    state.has_certs = true;
    ESP_LOGI(TAG, "Loaded TLS certs from NVS (cert=%d B, key=%d B)",
             (int)state.cert_len, (int)state.key_len);
    if (!identity_loaded && !generate_identity()) {
        ESP_LOGE(TAG, "Integration TLS identity is unavailable");
    } else if (identity_loaded) {
        ESP_LOGI(TAG, "Loaded persistent integration TLS identity");
    }
}

// ============================================================================
// Accessors
// ============================================================================

bool tls_mgr_has_certs(void)
{
    return state.has_certs;
}

bool tls_mgr_is_https_active(void)
{
    return state.https_active;
}

void tls_mgr_set_https_active(bool active)
{
    state.https_active = active;
}

const uint8_t* tls_mgr_get_cert(size_t* out_len)
{
    if (!state.has_certs) return NULL;
    if (out_len) *out_len = state.cert_len;
    return state.cert;
}

const uint8_t* tls_mgr_get_key(size_t* out_len)
{
    if (!state.has_certs) return NULL;
    if (out_len) *out_len = state.key_len;
    return state.key;
}

// ============================================================================
// Store / clear
// ============================================================================

bool tls_mgr_store_certs(const char* cert, size_t cert_len,
                         const char* key, size_t key_len)
{
    if (!cert || !key || cert_len == 0 || key_len == 0) {
        ESP_LOGE(TAG, "Invalid cert/key data");
        return false;
    }
    if (cert_len > TLS_MAX_CERT_LEN || key_len > TLS_MAX_KEY_LEN) {
        ESP_LOGE(TAG, "Cert or key too large (cert=%d, key=%d)",
                 (int)cert_len, (int)key_len);
        return false;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(nvs, NVS_KEY_CERT, cert, cert_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store cert: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return false;
    }

    err = nvs_set_blob(nvs, NVS_KEY_KEY, key, key_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store key: %s", esp_err_to_name(err));
        // Roll back the cert we just wrote
        nvs_erase_key(nvs, NVS_KEY_CERT);
        nvs_commit(nvs);
        nvs_close(nvs);
        return false;
    }

    nvs_commit(nvs);
    nvs_close(nvs);

    // Update in-memory state
    memcpy(state.cert, cert, cert_len);
    state.cert_len = cert_len;
    memcpy(state.key, key, key_len);
    state.key_len = key_len;
    state.has_certs = true;

    ESP_LOGI(TAG, "TLS certs stored (cert=%d B, key=%d B). Reboot to activate.",
             (int)cert_len, (int)key_len);
    return true;
}

bool tls_mgr_clear_certs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    nvs_erase_key(nvs, NVS_KEY_CERT);
    nvs_erase_key(nvs, NVS_KEY_KEY);
    nvs_commit(nvs);
    nvs_close(nvs);

    state.has_certs = false;
    state.cert_len = 0;
    state.key_len = 0;
    memset(state.cert, 0, sizeof(state.cert));
    memset(state.key, 0, sizeof(state.key));

    ESP_LOGI(TAG, "TLS certs cleared. Reboot to revert to HTTP.");
    return true;
}

bool tls_mgr_has_identity(void)
{
    return state.has_identity;
}

const uint8_t* tls_mgr_get_identity_cert(size_t* out_len)
{
    if (!state.has_identity) return NULL;
    if (out_len) *out_len = state.identity_cert_len;
    return state.identity_cert;
}

const uint8_t* tls_mgr_get_identity_key(size_t* out_len)
{
    if (!state.has_identity) return NULL;
    if (out_len) *out_len = state.identity_key_len;
    return state.identity_key;
}

bool tls_mgr_get_identity_fingerprint(char buffer[65])
{
    if (!state.has_identity || buffer == NULL) {
        return false;
    }
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(state.identity_fingerprint); ++i) {
        buffer[i * 2] = HEX[state.identity_fingerprint[i] >> 4];
        buffer[i * 2 + 1] = HEX[state.identity_fingerprint[i] & 0x0f];
    }
    buffer[64] = '\0';
    return true;
}
