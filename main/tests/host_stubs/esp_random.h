/*
 * Host stub for esp_random.h.
 *
 * Deterministic by default so failures reproduce, and steerable so a test can
 * force outcomes the hardware RNG would essentially never produce -- notably
 * two sessions minting the same token. Without that control, "tokens are
 * unique" can only ever be asserted probabilistically.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_random(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <cstddef>

namespace random_fake {

// Restart the sequence from a known seed.
void reset(uint32_t seed = 1);

// Return `value` for the next `count` calls, then resume the normal sequence.
// Used to force two token generations to produce identical output.
void force_next(uint32_t value, int count);

int calls();

}  // namespace random_fake
#endif
