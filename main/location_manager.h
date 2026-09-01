/*
 * Arctic Heat Pump Controller
 * Location Manager - persistent device location + automatic timezone.
 *
 * Stores the user-selected location (latitude/longitude + display name + IANA
 * timezone) in NVS. When timezone auto-mode is enabled (the default), selecting
 * a location derives the matching POSIX TZ string and applies it via the time
 * manager. When auto-mode is disabled, the manual timezone roller is authoritative
 * and the location is used only for weather.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool   valid;          // false until a location has ever been set
    double latitude;
    double longitude;
    char   name[96];       // display label, e.g. "Sun Peaks, British Columbia, CA"
    char   iana_tz[48];    // IANA zone, e.g. "America/Vancouver" (may be empty)
} location_t;

/**
 * @brief Load persisted location + tz-auto preference from NVS (or seed defaults),
 *        then, if auto-mode is on, apply the derived timezone.
 *        Call once at startup AFTER time_mgr_init().
 */
void location_mgr_init(void);

/**
 * @brief Get the current location (never NULL).
 */
const location_t* location_mgr_get(void);

/**
 * @brief Set and persist the device location.
 * @param lat,lon  Coordinates.
 * @param name     Display label (copied).
 * @param iana_tz  IANA timezone name (copied; may be NULL/empty).
 *
 * If timezone auto-mode is enabled and @p iana_tz maps to a known POSIX zone,
 * the device timezone is updated immediately.
 */
void location_mgr_set(double lat, double lon, const char* name, const char* iana_tz);

/**
 * @brief Whether the timezone is derived automatically from the location.
 */
bool location_mgr_get_tz_auto(void);

/**
 * @brief Enable/disable automatic timezone. Persists the preference.
 *        Enabling re-derives and applies the timezone from the current location.
 */
void location_mgr_set_tz_auto(bool automatic);

/**
 * @brief POSIX TZ string derived from the current location's IANA zone,
 *        or NULL if there is no location or the zone is unmapped.
 */
const char* location_mgr_derived_posix(void);

#ifdef __cplusplus
}
#endif
