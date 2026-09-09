/*
 * Test-only controls for the in-memory NVS fake.
 *
 * Exposed separately from nvs.h so that production sources cannot reach these
 * even by accident: they include <nvs.h>, never this header.
 *
 * The failure injection exists because NVS failure is a real device condition
 * (worn or full flash, a corrupt partition after an interrupted OTA) that is
 * impractical to provoke on hardware, and because code paths that ignore an
 * NVS error are exactly the ones that lose a user's settings silently.
 */
#pragma once

#include "nvs.h"

#include <string>

namespace nvs_fake {

// Forget every namespace, key and committed value, and clear all injected
// failures. Call at the start of any test that touches persistence.
void reset();

// Make the next `count` calls of the named operation return `err` instead of
// doing their work. `count < 0` means "until reset or cleared".
enum class Op {
    Open,
    GetU8,
    SetU8,
    GetU32,
    SetU32,
    GetStr,
    SetStr,
    Commit,
    EraseKey,
};
void fail_next(Op op, esp_err_t err, int count = 1);
void clear_failures();

// Number of times an operation has been invoked since reset(). Lets a test
// assert that a code path really did try to persist something, as opposed to
// passing because it never wrote at all.
int call_count(Op op);

// Direct inspection of committed state, bypassing handles. Returns false when
// the key is absent.
bool peek_u8(const std::string &ns, const std::string &key, uint8_t *out);
bool peek_str(const std::string &ns, const std::string &key, std::string *out);

// Seed committed state, as if a previous boot had written it.
void seed_u8(const std::string &ns, const std::string &key, uint8_t value);
void seed_str(const std::string &ns, const std::string &key, const std::string &value);

// True once a handle opened for writing has been committed. Uncommitted writes
// are visible through the same handle but are dropped by reset_volatile(),
// which models power loss before commit.
void reset_volatile();

}  // namespace nvs_fake
