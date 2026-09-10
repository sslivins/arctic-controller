/*
 * Deterministic, steerable RNG behind esp_random(). See esp_random.h.
 */

#include "esp_random.h"

#include <atomic>
#include <mutex>

namespace {

std::mutex g_m;
uint32_t g_state = 1;
uint32_t g_forced = 0;
int g_forced_remaining = 0;
std::atomic<int> g_calls{0};

}  // namespace

extern "C" uint32_t esp_random(void) {
    std::lock_guard<std::mutex> lock(g_m);
    g_calls.fetch_add(1);
    if (g_forced_remaining > 0) {
        --g_forced_remaining;
        return g_forced;
    }
    // xorshift32: cheap, well-distributed enough that hex tokens built from it
    // do not repeat within a test run.
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}

namespace random_fake {

void reset(uint32_t seed) {
    std::lock_guard<std::mutex> lock(g_m);
    g_state = seed ? seed : 1;
    g_forced_remaining = 0;
    g_calls.store(0);
}

void force_next(uint32_t value, int count) {
    std::lock_guard<std::mutex> lock(g_m);
    g_forced = value;
    g_forced_remaining = count;
}

int calls() { return g_calls.load(); }

}  // namespace random_fake
