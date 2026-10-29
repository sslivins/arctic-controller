/*
 * Arctic Heat Pump Controller
 * JSON form of an event-log entry (see event_json.h).
 */

#include "event_json.h"

#include "fault_help_links.h"
#include "heatpump_types.h"
#include "macon_advanced_params.h"
#include "macon_faults.h"

#include <cstdio>

namespace {

// Decoded, client-friendly fields for an event's payload, so REST/HA/web
// clients never unpack the raw bit layout themselves.
void add_event_details(cJSON* evt, const event_entry_t& e)
{
    const uint32_t p = e.payload;
    switch (e.type) {
        case EVENT_MODE_CHANGED:
            cJSON_AddStringToObject(evt, "from_mode", arctic::workingModeToString(
                static_cast<arctic::WorkingMode>(event_payload_from(p))));
            cJSON_AddStringToObject(evt, "to_mode", arctic::workingModeToString(
                static_cast<arctic::WorkingMode>(event_payload_to(p))));
            break;
        case EVENT_SETPOINT_CHANGED: {
            const char* key = event_setpoint_key(p);
            if (key) cJSON_AddStringToObject(evt, "setpoint", key);
            else cJSON_AddNullToObject(evt, "setpoint");
            cJSON_AddNumberToObject(evt, "from", event_payload_from(p));
            cJSON_AddNumberToObject(evt, "to", event_payload_to(p));
            break;
        }
        case EVENT_ERROR_APPEARED:
        case EVENT_ERROR_CLEARED: {
            const arctic::MaconFaultBit* fb = arctic::macon_fault_bit_for_site(
                static_cast<arctic::MaconFaultSiteId>(p));
            if (fb) {
                cJSON_AddStringToObject(evt, "fault_code", fb->code);
                cJSON_AddStringToObject(evt, "fault_label", fb->label);
                cJSON_AddStringToObject(evt, "help_url", arctic::faultHelpUrl(fb->code));
            } else {
                cJSON_AddNullToObject(evt, "fault_code");
                cJSON_AddNullToObject(evt, "fault_label");
                cJSON_AddNullToObject(evt, "help_url");
            }
            break;
        }
        case EVENT_WATCHDOG_RESET:
            cJSON_AddStringToObject(evt, "watchdog", event_watchdog_key(p));
            break;
        case EVENT_NETWORK_RECOVERED: {
            const char* key = event_network_recovery_key(p);
            if (key) cJSON_AddStringToObject(evt, "recovery", key);
            else cJSON_AddNullToObject(evt, "recovery");
            break;
        }
        case EVENT_WEB_SERVER_DOWN:
        case EVENT_HTTPS_FALLBACK:
            cJSON_AddStringToObject(evt, "cause", event_web_server_cause_key(p));
            break;
        case EVENT_WEB_SERVER_RECOVERED: {
            const char* key = event_web_server_recovery_key(p);
            if (key) cJSON_AddStringToObject(evt, "recovery", key);
            else cJSON_AddNullToObject(evt, "recovery");
            break;
        }
        case EVENT_WIFI_LOST:
            cJSON_AddNumberToObject(evt, "reason", p);
            cJSON_AddStringToObject(evt, "reason_key", event_wifi_reason_key(p));
            break;
        case EVENT_WIFI_RESTORED:
            cJSON_AddNumberToObject(evt, "down_s", p);
            break;
        case EVENT_WIFI_UNSTABLE:
            cJSON_AddNumberToObject(evt, "drops", p);
            break;
        case EVENT_EXT_SENSOR_LOST:
        case EVENT_EXT_SENSOR_RESTORED: {
            const char* slot = event_ext_sensor_slot_key(p);
            if (slot) cJSON_AddStringToObject(evt, "sensor", slot);
            else cJSON_AddNullToObject(evt, "sensor");
            if (e.type == EVENT_EXT_SENSOR_LOST)
                cJSON_AddStringToObject(evt, "cause", event_ext_sensor_cause_key(p));
            else
                cJSON_AddNumberToObject(evt, "down_s", event_ext_sensor_down_s(p));
            break;
        }
        case EVENT_WIFI_ROAMED:
            cJSON_AddNumberToObject(evt, "from_rssi_dbm", -(int)(p & 0xFF));
            cJSON_AddNumberToObject(evt, "to_rssi_dbm", -(int)((p >> 8) & 0xFF));
            cJSON_AddNumberToObject(evt, "channel", (p >> 16) & 0xFF);
            break;
        case EVENT_FIRMWARE_UPDATED: {
            char v[16];
            if (event_firmware_version_format(event_firmware_from(p), v, sizeof(v)))
                cJSON_AddStringToObject(evt, "from_version", v);
            else
                cJSON_AddNullToObject(evt, "from_version");
            if (event_firmware_version_format(event_firmware_to(p), v, sizeof(v)))
                cJSON_AddStringToObject(evt, "to_version", v);
            else
                cJSON_AddNullToObject(evt, "to_version");
            break;
        }
        case EVENT_PARAM_CHANGED: {
            const uint8_t ap = event_param_ap(p);
            char key[8];
            snprintf(key, sizeof(key), "AP%u", (unsigned)ap);
            cJSON_AddStringToObject(evt, "param", key);
            const arctic::AdvancedParam* param = arctic::advanced_param_lookup(ap);
            if (param) cJSON_AddStringToObject(evt, "param_name", param->name);
            else cJSON_AddNullToObject(evt, "param_name");
            const char* unit = arctic::advanced_display_unit(ap);
            cJSON_AddStringToObject(evt, "unit", unit ? unit : "");
            const char* kind = "none";
            switch (arctic::advanced_temperature_kind(ap)) {
                case arctic::AdvancedTemperatureKind::Absolute: kind = "absolute"; break;
                case arctic::AdvancedTemperatureKind::Differential: kind = "differential"; break;
                default: break;
            }
            cJSON_AddStringToObject(evt, "temperature_kind", kind);
            const bool choice = arctic::advanced_enum_option_count(ap) > 0;
            const int values[2] = {event_param_from(p), event_param_to(p)};
            const char* names[2] = {"from", "to"};
            const char* labels[2] = {"from_label", "to_label"};
            for (int i = 0; i < 2; i++) {
                if (values[i] == EVENT_PARAM_UNKNOWN) {
                    cJSON_AddNullToObject(evt, names[i]);
                    continue;
                }
                cJSON_AddNumberToObject(evt, names[i], values[i]);
                if (!choice) continue;
                const arctic::AdvEnumOption* o = arctic::advanced_enum_option_at(ap, (size_t)values[i]);
                if (o && o->label) cJSON_AddStringToObject(evt, labels[i], o->label);
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace

cJSON* event_json_create(const event_entry_t& e)
{
    cJSON* evt = cJSON_CreateObject();
    if (evt == nullptr) return nullptr;
    cJSON_AddStringToObject(evt, "type", event_type_name(e.type));
    cJSON_AddStringToObject(evt, "category",
                            event_category_name(event_type_category(e.type)));
    cJSON_AddNumberToObject(evt, "timestamp", e.timestamp);
    cJSON_AddNumberToObject(evt, "boot_id", e.boot_id);
    cJSON_AddNumberToObject(evt, "uptime_ms", (double)e.uptime_ms);
    cJSON_AddNumberToObject(evt, "payload", e.payload);
    add_event_details(evt, e);
    return evt;
}
