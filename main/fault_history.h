#pragma once

#include "event_log.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Fault spans for the history charts, rebuilt from the event log's
 * error_appeared / error_cleared pairs rather than stored separately.
 */
typedef struct {
    uint32_t start;   // Unix seconds
    uint32_t end;     // Unix seconds; 0 = still active
    uint16_t site;    // arctic::MaconFaultSiteId
} fault_interval_t;

/**
 * Pair fault events (oldest first) into spans that overlap [window_start,
 * window_end], oldest first.
 *
 * A fault that is still active when the controller reboots is logged again by
 * the next boot and never gets a clear, so a span left open by an earlier boot
 * is closed at the last timed event of that boot. Only a span opened in
 * current_boot_id can be reported as still active.
 *
 * @param total If non-null, receives how many spans overlap the window,
 *              including any dropped by the cap.
 * @return Number of spans written (at most cap). When there are more, the
 *         newest (by start) are kept.
 */
size_t fault_intervals_from_events(const event_entry_t* oldest_first, size_t n,
                                   uint32_t window_start, uint32_t window_end,
                                   uint32_t current_boot_id,
                                   fault_interval_t* out, size_t cap,
                                   size_t* total = nullptr);

/**
 * fault_intervals_from_events() over the live event log.
 */
size_t fault_history_query(uint32_t window_start, uint32_t window_end,
                           fault_interval_t* out, size_t cap,
                           size_t* total = nullptr);
