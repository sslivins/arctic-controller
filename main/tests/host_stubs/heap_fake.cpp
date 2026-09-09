/*
 * Host heap stub. See heap_fake.h.
 */

#include "heap_fake.h"

#include <cstdlib>
#include <cstring>

namespace {
int g_fail_remaining = 0;
int g_allocs = 0;
int g_frees = 0;

bool should_fail() {
    if (g_fail_remaining == 0) {
        return false;
    }
    if (g_fail_remaining > 0) {
        --g_fail_remaining;
    }
    return true;
}
}  // namespace

extern "C" {

void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    if (should_fail()) {
        return nullptr;
    }
    void *p = std::malloc(size);
    if (p) {
        ++g_allocs;
    }
    return p;
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    if (should_fail()) {
        return nullptr;
    }
    void *p = std::calloc(n, size);
    if (p) {
        ++g_allocs;
    }
    return p;
}

void heap_caps_free(void *ptr) {
    if (ptr) {
        ++g_frees;
    }
    std::free(ptr);
}

// Fixed plausible values: nothing host-tested reasons about the real numbers,
// and returning 0 would make callers believe the device is out of memory.
size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return 200 * 1024;
}

size_t heap_caps_get_minimum_free_size(uint32_t caps) {
    (void)caps;
    return 100 * 1024;
}

}  // extern "C"

namespace heap_fake {

void fail_next_alloc(int count) { g_fail_remaining = count; }
void clear_failures() { g_fail_remaining = 0; }

void reset() {
    g_fail_remaining = 0;
    g_allocs = 0;
    g_frees = 0;
}

int alloc_count() { return g_allocs; }
int free_count() { return g_frees; }
int outstanding() { return g_allocs - g_frees; }

}  // namespace heap_fake
