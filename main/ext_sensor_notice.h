/*
 * Arctic Heat Pump Controller
 * External temperature sensor notice (the "temperature sensor offline" bell entry)
 *
 * When a BACnet / Modbus-TCP supply or return sensor stops answering, COP and
 * heat output quietly fall back to the heat pump's own sensors. This module
 * keeps one summary of those outages for the notification bell: whether one is
 * still going on, how many there were since the user last tapped the bell, and
 * how long the latest one lasted.
 *
 * It is ONE entry however many outages happen (supply and return going down
 * together count as one), so a flapping sensor cannot fill the bell. The
 * outage detection itself (ext_temp_sensors.cpp, perf::OutageTracker) already
 * needs 5 minutes down and 10 minutes clean before it reports a change.
 *
 * RAM only: after a reboot the tracker starts again and re-reports a sensor
 * that is still down.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "i18n/i18n.h"

#define EXT_SENSOR_NOTICE_COUNT_MAX 9999u

typedef struct {
    bool     pending;      // there is something to show
    bool     ongoing;      // a sensor is down right now
    uint32_t count;        // outages since the last acknowledgement
    uint32_t last_down_s;  // length of the latest finished outage
} ext_sensor_notice_t;

/** A sensor slot (0 supply, 1 return) has been failing long enough to count as lost. */
void ext_sensor_notice_record_lost(int slot);

/** A lost sensor slot has been reading again; it was down for down_s seconds. */
void ext_sensor_notice_record_restored(int slot, uint32_t down_s);

/** The sensor settings changed: forget sensors that were down (they may be gone). */
void ext_sensor_notice_settings_changed(void);

/** The user acknowledged the notice. */
void ext_sensor_notice_clear(void);

/** Copy the current notice. Returns out->pending. */
bool ext_sensor_notice_get(ext_sensor_notice_t* out);

/** Bumped on every change, so the UI can tell when to refresh the bell. */
uint32_t ext_sensor_notice_revision(void);

/**
 * @brief Format the bell text.
 *
 * Down now:   "Temperature sensor not responding – COP using heat pump sensors"
 * Back, once: "Temperature sensor was offline for 45 min"
 * Back, more: "Temperature sensor went offline 3 times – latest for 1 h 05 min"
 *
 * @return Number of characters written (0 if nothing is pending)
 */
size_t ext_sensor_notice_format(char* buf, size_t buf_size, const ext_sensor_notice_t* notice,
                                language_t lang);
