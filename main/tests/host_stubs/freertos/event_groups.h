/*
 * Host stub for freertos/event_groups.h.
 *
 * time_manager uses an event group as a one-way latch: the NTP sync callback
 * sets TIME_VALID_BIT and time_mgr_is_synced() reads it. The bit operations
 * are real (not no-ops) so a test can distinguish "the callback set the bit"
 * from "the callback declined to", which is the whole point of the guard
 * against a bogus NTP reply.
 *
 * Creation can be made to fail, because time_mgr_init() has an early-return
 * path for it that is otherwise unreachable.
 */
#pragma once

#include "freertos/FreeRTOS.h"

#include <stdint.h>

typedef uint32_t EventBits_t;

struct EventGroupDef_t;
typedef struct EventGroupDef_t *EventGroupHandle_t;

#ifndef BIT0
#define BIT0 (1U << 0)
#define BIT1 (1U << 1)
#define BIT2 (1U << 2)
#define BIT3 (1U << 3)
#endif

#ifdef __cplusplus
extern "C" {
#endif

EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t group);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t group);

#ifdef __cplusplus
}

namespace event_group_fake {

// Make the next xEventGroupCreate() return nullptr, as it does when the device
// is out of internal RAM (see #247).
void fail_next_create();

// Forget every group and clear the injected failure.
void reset();

int created();

}  // namespace event_group_fake
#endif
