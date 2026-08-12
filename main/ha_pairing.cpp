/*
 * Arctic Heat Pump Controller
 * Physical Home Assistant pairing authorization
 */
#include "ha_pairing.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/platform_util.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "ha_pairing";

static struct {
    bool active;
    char code[HA_PAIRING_CODE_LEN + 1];
    int64_t deadline_us;
    uint8_t failed_attempts;
} state = {};

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;

static void clear_locked(void)
{
    mbedtls_platform_zeroize(state.code, sizeof(state.code));
    state.active = false;
    state.deadline_us = 0;
    state.failed_attempts = 0;
}

static bool expired_locked(int64_t now_us)
{
    if (state.active && now_us >= state.deadline_us) {
        clear_locked();
        return true;
    }
    return false;
}

static bool valid_code_shape(const char* code)
{
    if (code == NULL || strlen(code) != HA_PAIRING_CODE_LEN) {
        return false;
    }
    for (size_t i = 0; i < HA_PAIRING_CODE_LEN; ++i) {
        if (code[i] < '0' || code[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool constant_time_code_equal(const char* lhs, const char* rhs)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < HA_PAIRING_CODE_LEN; ++i) {
        difference |= (uint8_t)lhs[i] ^ (uint8_t)rhs[i];
    }
    return difference == 0;
}

bool ha_pairing_start(char* code_out)
{
    if (code_out == NULL) {
        return false;
    }

    char code[HA_PAIRING_CODE_LEN + 1];
    static const uint32_t CODE_SPACE = 1000000U;
    static const uint32_t UNBIASED_LIMIT =
        UINT32_MAX - (UINT32_MAX % CODE_SPACE);
    uint32_t random_value;
    do {
        random_value = esp_random();
    } while (random_value >= UNBIASED_LIMIT);
    const uint32_t value = random_value % CODE_SPACE;
    snprintf(code, sizeof(code), "%06lu", (unsigned long)value);

    portENTER_CRITICAL(&state_lock);
    clear_locked();
    memcpy(state.code, code, sizeof(code));
    state.deadline_us =
        esp_timer_get_time() + (int64_t)HA_PAIRING_WINDOW_SECONDS * 1000000LL;
    state.active = true;
    memcpy(code_out, code, sizeof(code));
    portEXIT_CRITICAL(&state_lock);

    mbedtls_platform_zeroize(code, sizeof(code));
    ESP_LOGI(TAG, "Physical pairing window opened for %u seconds",
             HA_PAIRING_WINDOW_SECONDS);
    return true;
}

void ha_pairing_cancel(void)
{
    portENTER_CRITICAL(&state_lock);
    clear_locked();
    portEXIT_CRITICAL(&state_lock);
    ESP_LOGI(TAG, "Physical pairing window closed");
}

ha_pairing_status_t ha_pairing_get_status(void)
{
    ha_pairing_status_t result = {};
    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&state_lock);
    expired_locked(now_us);
    result.active = state.active;
    result.failed_attempts = state.failed_attempts;
    if (state.active) {
        const int64_t remaining_us = state.deadline_us - now_us;
        result.remaining_seconds =
            (uint32_t)((remaining_us + 999999LL) / 1000000LL);
    }
    portEXIT_CRITICAL(&state_lock);
    return result;
}

static ha_pairing_claim_result_t consume_code(const char* code)
{
    const bool shape_valid = valid_code_shape(code);
    bool matched = false;
    bool locked = false;

    portENTER_CRITICAL(&state_lock);
    expired_locked(esp_timer_get_time());
    if (!state.active) {
        portEXIT_CRITICAL(&state_lock);
        return HA_PAIRING_CLAIM_NOT_OPEN;
    }

    if (shape_valid) {
        matched = constant_time_code_equal(code, state.code);
    }
    if (!matched) {
        state.failed_attempts++;
        locked = state.failed_attempts >= HA_PAIRING_MAX_ATTEMPTS;
        if (locked) {
            clear_locked();
        }
        portEXIT_CRITICAL(&state_lock);
        ESP_LOGW(TAG, "Rejected integration pairing claim");
        return locked
            ? HA_PAIRING_CLAIM_LOCKED
            : HA_PAIRING_CLAIM_INVALID_CODE;
    }

    // Close before persisting so concurrent requests cannot claim twice.
    clear_locked();
    portEXIT_CRITICAL(&state_lock);
    return HA_PAIRING_CLAIM_OK;
}

ha_pairing_claim_result_t ha_pairing_claim(
    const char* code,
    char* token_out)
{
    if (token_out == NULL) {
        return HA_PAIRING_CLAIM_STORAGE_ERROR;
    }
    token_out[0] = '\0';

    const ha_pairing_claim_result_t authorization = consume_code(code);
    if (authorization != HA_PAIRING_CLAIM_OK) {
        return authorization;
    }

    if (!auth_mgr_issue_integration_token(token_out)) {
        ESP_LOGE(TAG, "Could not persist token for pairing claim");
        return HA_PAIRING_CLAIM_STORAGE_ERROR;
    }

    ESP_LOGI(TAG, "Integration pairing claim completed");
    return HA_PAIRING_CLAIM_OK;
}

ha_pairing_claim_result_t ha_pairing_authorize(const char* code)
{
    const ha_pairing_claim_result_t result = consume_code(code);
    if (result == HA_PAIRING_CLAIM_OK) {
        ESP_LOGI(TAG, "Physical administrator authorization completed");
    }
    return result;
}
