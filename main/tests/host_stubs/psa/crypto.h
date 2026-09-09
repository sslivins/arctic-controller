/*
 * Host stub for psa/crypto.h -- only what main/ actually uses, which is
 * psa_hash_compute() with PSA_ALG_SHA_256.
 *
 * The SHA-256 here is real, not a placeholder. auth_manager.cpp stores only
 * hashes and compares hashes, so a fake digest (say, a sum of bytes) would
 * make distinct passwords collide and let a broken comparison pass.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t)-132)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t)-134)

#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)
#define PSA_HASH_LENGTH(alg) ((size_t)((alg) == PSA_ALG_SHA_256 ? 32u : 0u))

psa_status_t psa_crypto_init(void);

psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input,
                              size_t input_length, uint8_t *hash,
                              size_t hash_size, size_t *hash_length);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace psa_fake {

// Make the next `count` psa_hash_compute() calls fail. auth_manager logs and
// zeroes the hash on failure; without injection that branch is unreachable.
void fail_next_hash(int count);
void reset();
int hash_calls();

}  // namespace psa_fake
#endif
