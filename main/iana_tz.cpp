/*
 * Arctic Heat Pump Controller
 * IANA timezone name -> POSIX TZ string mapping (implementation).
 */
#include "iana_tz.h"
#include <string.h>

typedef struct {
    const char* iana;
    const char* posix;
} iana_posix_entry_t;

// POSIX rule strings mirror the curated set offered by the manual timezone
// roller (settings_time_screen.cpp) so an auto-derived zone and a manually
// picked one produce identical device behaviour.
static const iana_posix_entry_t k_map[] = {
    // ---- North America: US ----
    {"America/New_York",     "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Detroit",      "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Indiana/Indianapolis", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago",      "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver",       "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Boise",        "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix",      "MST7"},
    {"America/Los_Angeles",  "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage",    "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Juneau",       "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Sitka",        "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu",     "HST10"},

    // ---- North America: Canada ----
    {"America/Toronto",      "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Montreal",     "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Nipigon",      "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Winnipeg",     "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Regina",       "CST6"},              // Saskatchewan: no DST
    {"America/Swift_Current", "CST6"},
    {"America/Edmonton",     "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Vancouver",    "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Dawson_Creek", "MST7"},              // BC Peace River: no DST
    {"America/Fort_Nelson",  "MST7"},              // permanent MST
    {"America/Whitehorse",   "MST7"},              // Yukon: permanent MST
    {"America/Dawson",       "MST7"},
    {"America/Halifax",      "AST4ADT,M3.2.0,M11.1.0"},
    {"America/Moncton",      "AST4ADT,M3.2.0,M11.1.0"},
    {"America/Glace_Bay",    "AST4ADT,M3.2.0,M11.1.0"},
    {"America/St_Johns",     "NST3:30NDT,M3.2.0,M11.1.0"},

    // ---- North America: Mexico ----
    {"America/Mexico_City",  "CST6"},              // DST abolished 2022
    {"America/Monterrey",    "CST6"},
    {"America/Tijuana",      "PST8PDT,M3.2.0,M11.1.0"},

    // ---- Europe ----
    {"Europe/London",        "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin",        "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon",        "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris",         "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome",          "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam",     "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels",      "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm",     "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Helsinki",      "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Athens",        "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Bucharest",     "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kiev",          "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv",          "EET-2EEST,M3.5.0/3,M10.5.0/4"},

    // ---- Asia ----
    {"Asia/Tokyo",           "JST-9"},
    {"Asia/Shanghai",        "CST-8"},
    {"Asia/Hong_Kong",       "HKT-8"},
    {"Asia/Singapore",       "SGT-8"},
    {"Asia/Seoul",           "KST-9"},
    {"Asia/Kolkata",         "IST-5:30"},
    {"Asia/Calcutta",        "IST-5:30"},
    {"Asia/Dubai",           "GST-4"},

    // ---- Australia / NZ ----
    {"Australia/Sydney",     "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne",  "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane",   "AEST-10"},           // no DST
    {"Australia/Adelaide",   "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Perth",      "AWST-8"},
    {"Pacific/Auckland",     "NZST-12NZDT,M9.5.0,M4.1.0/3"},

    // ---- UTC ----
    {"UTC",                  "UTC0"},
    {"Etc/UTC",              "UTC0"},
    {"Etc/GMT",              "UTC0"},
};

#define K_MAP_LEN (sizeof(k_map) / sizeof(k_map[0]))

const char* iana_to_posix(const char* iana)
{
    if (!iana || iana[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < K_MAP_LEN; i++) {
        if (strcmp(k_map[i].iana, iana) == 0) {
            return k_map[i].posix;
        }
    }
    return NULL;
}
