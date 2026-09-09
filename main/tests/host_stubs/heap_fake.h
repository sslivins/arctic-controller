/*
 * Test-only controls for the host heap stub.
 *
 * An unchecked heap_caps_malloc is a device-only crash: on the host malloc
 * effectively never fails, so the NULL branch is never taken and never tested.
 * fail_next_alloc() makes that branch reachable.
 */
#pragma once

#include "esp_heap_caps.h"

#include <cstddef>

namespace heap_fake {

// Fail the next `count` allocations by returning NULL.
void fail_next_alloc(int count = 1);
void clear_failures();

// Allocation bookkeeping since reset(). outstanding() > 0 after an operation
// that should have cleaned up is a leak.
void reset();
int alloc_count();
int free_count();
int outstanding();

}  // namespace heap_fake
