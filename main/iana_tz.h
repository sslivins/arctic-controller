/*
 * Arctic Heat Pump Controller
 * IANA timezone name -> POSIX TZ string mapping.
 *
 * Open-Meteo's geocoding API returns an IANA zone name (e.g. "America/Vancouver")
 * for each location. The device configures time via POSIX TZ strings (with DST
 * rules), so we translate the IANA name to the matching POSIX rule string. The
 * table is a curated subset (North America is exhaustive; other regions cover the
 * common zones). Unmapped zones return NULL, in which case the caller should fall
 * back to manual timezone selection.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Translate an IANA timezone name to a POSIX TZ string.
 * @param iana IANA zone name (e.g. "America/Vancouver"). May be NULL.
 * @return POSIX TZ string (e.g. "PST8PDT,M3.2.0,M11.1.0") or NULL if unmapped.
 *         The returned pointer is static storage; do not free it.
 */
const char* iana_to_posix(const char* iana);

#ifdef __cplusplus
}
#endif
