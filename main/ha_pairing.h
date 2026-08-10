/*
 * Arctic Heat Pump Controller
 * Physical Home Assistant pairing authorization
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "auth_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HA_PAIRING_CODE_LEN 6
#define HA_PAIRING_WINDOW_SECONDS 300
#define HA_PAIRING_MAX_ATTEMPTS 5

typedef struct {
    bool active;
    uint32_t remaining_seconds;
    uint8_t failed_attempts;
} ha_pairing_status_t;

typedef enum {
    HA_PAIRING_CLAIM_OK,
    HA_PAIRING_CLAIM_NOT_OPEN,
    HA_PAIRING_CLAIM_INVALID_CODE,
    HA_PAIRING_CLAIM_LOCKED,
    HA_PAIRING_CLAIM_STORAGE_ERROR,
} ha_pairing_claim_result_t;

/**
 * @brief Open a new physical pairing window.
 *
 * Starting a window does not revoke the current integration token. The token
 * is rotated only after a successful claim.
 *
 * @param code_out Receives the six-digit code (HA_PAIRING_CODE_LEN + 1).
 */
bool ha_pairing_start(char* code_out);

/**
 * @brief Cancel and erase the active pairing code.
 */
void ha_pairing_cancel(void);

/**
 * @brief Read the current pairing-window status.
 */
ha_pairing_status_t ha_pairing_get_status(void);

/**
 * @brief Claim the active pairing window and rotate the integration token.
 *
 * @param code Six-digit code displayed on the physical controller.
 * @param token_out Receives the one-time plaintext integration token.
 */
ha_pairing_claim_result_t ha_pairing_claim(
    const char* code,
    char* token_out);

#ifdef __cplusplus
}
#endif
