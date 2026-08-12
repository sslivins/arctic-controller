/*
 * Arctic Heat Pump Controller
 * Physical-presence setup/pairing authorization primitive.
 *
 * Generates a short-lived on-device code proving physical presence. Consumed
 * by first-boot administrator credential securing and Home Assistant pairing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "auth_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETUP_PAIRING_CODE_LEN 6
#define SETUP_PAIRING_WINDOW_SECONDS 300
#define SETUP_PAIRING_MAX_ATTEMPTS 5

typedef struct {
    bool active;
    uint32_t remaining_seconds;
    uint8_t failed_attempts;
} setup_pairing_status_t;

typedef enum {
    SETUP_PAIRING_CLAIM_OK,
    SETUP_PAIRING_CLAIM_NOT_OPEN,
    SETUP_PAIRING_CLAIM_INVALID_CODE,
    SETUP_PAIRING_CLAIM_LOCKED,
    SETUP_PAIRING_CLAIM_STORAGE_ERROR,
} setup_pairing_claim_result_t;

/**
 * @brief Open a new physical pairing window.
 *
 * Starting a window does not revoke the current integration token. The token
 * is rotated only after a successful claim.
 *
 * @param code_out Receives the six-digit code (SETUP_PAIRING_CODE_LEN + 1).
 */
bool setup_pairing_start(char* code_out);

/**
 * @brief Cancel and erase the active pairing code.
 */
void setup_pairing_cancel(void);

/**
 * @brief Read the current pairing-window status.
 */
setup_pairing_status_t setup_pairing_get_status(void);

/**
 * @brief Claim the active pairing window and rotate the integration token.
 *
 * @param code Six-digit code displayed on the physical controller.
 * @param token_out Receives the one-time plaintext integration token.
 */
setup_pairing_claim_result_t setup_pairing_claim(
    const char* code,
    char* token_out);

/**
 * @brief Consume the physical pairing code without issuing an HA token.
 *
 * Used for first-boot administrator credential replacement.
 */
setup_pairing_claim_result_t setup_pairing_authorize(const char* code);

#ifdef __cplusplus
}
#endif
