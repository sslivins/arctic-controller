/*
 * Arctic Heat Pump Controller
 * Heat pump fault notice (the "heat pump problems" bell entry)
 *
 * A fault can come and go in a minute while nobody is looking at the screen.
 * The event log records it, but nothing draws attention to it, so this module
 * keeps one summary of the faults seen since the user last acknowledged them:
 * how many, since when, and which one was the latest. The notification bell
 * shows it until the user taps it.
 *
 * It is ONE entry no matter how many faults happen, so it cannot grow if the
 * bell is ignored for months: the count saturates at FAULT_NOTICE_COUNT_MAX.
 *
 * The summary is kept in NVS so it survives a reboot or firmware update. The
 * first fault after an acknowledgement is written at once; later ones only
 * bump the count in RAM and are written at most every
 * FAULT_NOTICE_FLUSH_INTERVAL_S (by fault_notice_tick()), which bounds flash
 * wear when a fault flaps. A reboot inside that window can lose some of the
 * count, but never the notice itself.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "i18n/i18n.h"

#define FAULT_NOTICE_COUNT_MAX        9999u
#define FAULT_NOTICE_FLUSH_INTERVAL_S 600u

typedef struct {
    bool     pending;      // there is something to show
    uint32_t count;        // fault onsets since the last acknowledgement
    int64_t  first;        // wall time of the first one; 0 if the clock was not set
    int64_t  latest;       // wall time of the latest one; 0 if the clock was not set
    uint16_t latest_site;  // MaconFaultSiteId of the latest one
} fault_notice_t;

/** Load the saved notice from NVS. Call once at startup, after NVS init. */
void fault_notice_init(void);

/**
 * @brief Record that a fault has just appeared.
 *
 * @param site     MaconFaultSiteId of the fault
 * @param now      Wall-clock time; values before 2024 are treated as "not set"
 * @param mono_s   Monotonic seconds since boot (for the write throttle)
 * @param at_boot  The fault was already active on the first reading after boot.
 *                 It is only recorded when no notice is pending, so a fault
 *                 that is still active across a reboot is not counted twice.
 */
void fault_notice_record(uint16_t site, time_t now, uint32_t mono_s, bool at_boot);

/** Write a deferred count to NVS once the throttle interval has passed. */
void fault_notice_tick(uint32_t mono_s);

/** The user acknowledged the notice: forget it, in RAM and in NVS. */
void fault_notice_clear(void);

/** Copy the current notice. Returns out->pending. */
bool fault_notice_get(fault_notice_t* out);

/** Bumped on every change, so the UI can tell when to refresh the bell. */
uint32_t fault_notice_revision(void);

/**
 * @brief Format the bell text for a notice.
 *
 * One fault:  "Heat pump problem: P06 Refrigerant pressure too low"
 * Several:    "14 heat pump problems – latest P06"
 * No date: the bell only flags the problem; tapping it opens the error screen,
 * which has the times.
 *
 * @return Number of characters written (0 if nothing is pending)
 */
size_t fault_notice_format(char* buf, size_t buf_size, const fault_notice_t* notice,
                           language_t lang);
