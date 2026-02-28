/*
 * Arctic Heat Pump Controller
 * TLS Certificate Manager — NVS storage and runtime provisioning
 */
#include "tls_manager.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>

static const char* TAG = "tls_mgr";

// NVS namespace and keys
static const char* NVS_NAMESPACE = "tls";
static const char* NVS_KEY_CERT  = "cert";
static const char* NVS_KEY_KEY   = "key";

// Runtime state
static struct {
    bool initialised;
    bool has_certs;
    bool https_active;
    uint8_t cert[TLS_MAX_CERT_LEN];
    size_t  cert_len;                   // includes null terminator
    uint8_t key[TLS_MAX_KEY_LEN];
    size_t  key_len;                    // includes null terminator
} state = {};

// ============================================================================
// Init / load
// ============================================================================

void tls_mgr_init(void)
{
    if (state.initialised) return;
    state.initialised = true;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No TLS certs in NVS (err=%s)", esp_err_to_name(err));
        return;
    }

    // Load certificate
    size_t len = sizeof(state.cert);
    err = nvs_get_blob(nvs, NVS_KEY_CERT, state.cert, &len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No TLS cert in NVS");
        nvs_close(nvs);
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
        return;
    }
    state.key_len = len;

    nvs_close(nvs);

    state.has_certs = true;
    ESP_LOGI(TAG, "Loaded TLS certs from NVS (cert=%d B, key=%d B)",
             (int)state.cert_len, (int)state.key_len);
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
