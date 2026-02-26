/*
 * Arctic Heat Pump Controller
 * Embedded MCP (Model Context Protocol) Server
 *
 * Implements MCP protocol version 2025-03-26 using Streamable HTTP transport.
 * Single endpoint: POST /mcp accepts JSON-RPC 2.0 requests and returns
 * JSON-RPC 2.0 responses with Content-Type: application/json.
 *
 * Supported methods:
 *   - initialize          → capability negotiation
 *   - notifications/initialized → client ready (accepted, no response)
 *   - ping                → keepalive
 *   - tools/list          → enumerate available tools
 *   - tools/call          → execute a tool
 *   - resources/list      → enumerate available resources
 *   - resources/read      → read a resource
 */

#include "mcp_server.h"
#include "api_server.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "heatpump_params.h"
#include "heatpump_errors.h"
#include "event_log.h"
#include "log_buffer.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "ota_manager.h"
#include "auth_manager.h"
#include "app_preferences.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

static const char* TAG = "mcp";

// MCP protocol version we support
static const char* MCP_PROTOCOL_VERSION = "2025-03-26";

// Server identity
static const char* MCP_SERVER_NAME = "arctic-controller";
static const char* MCP_SERVER_VERSION = "1.0.0";

// Maximum JSON-RPC request body size (8 KB)
#define MCP_MAX_REQUEST_SIZE 8192

// ============================================================================
// Tool Definitions
// ============================================================================

struct ToolDef {
    const char* name;
    const char* description;
    const char* input_schema_json;  // Pre-built JSON Schema string
};

// All tool definitions — order matches the handler dispatch table
static const ToolDef TOOLS[] = {
    {
        "get_device_info",
        "Get device information including firmware version, platform, uptime, and memory usage. "
        "Use this to check the controller's health and identity.",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"
    },
    {
        "get_heatpump_status",
        "Get comprehensive heat pump status including operating mode, temperatures, setpoints, "
        "component states (compressor, fans, pump), electrical readings, and error status. "
        "This is the primary tool for understanding what the heat pump is currently doing.",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"
    },
    {
        "get_heatpump_errors",
        "Get active heat pump error codes with severity, descriptions, and resolution steps. "
        "Also returns error history. Use this when diagnosing problems.",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"
    },
    {
        "set_heatpump_power",
        "Turn the heat pump on or off. The heat pump is an ECO-600 air-source unit "
        "controlling heating, cooling, and domestic hot water for a residential system.",
        R"json({"type":"object","properties":{"on":{"type":"boolean","description":"true to turn on, false to turn off"}},"required":["on"],"additionalProperties":false})json"
    },
    {
        "set_heatpump_mode",
        "Set the heat pump operating mode. Available modes: cooling (space cooling via fan coils), "
        "floor_heating (radiant floor heating), fan_coil_heating (space heating via fan coils), "
        "hot_water (domestic hot water production only), auto (automatic mode selection).",
        R"json({"type":"object","properties":{"mode":{"type":"string","enum":["cooling","floor_heating","fan_coil_heating","hot_water","auto"],"description":"Operating mode to set"}},"required":["mode"],"additionalProperties":false})json"
    },
    {
        "set_temperature_setpoint",
        "Set a temperature setpoint. Units are in degrees Celsius. Typical ranges: "
        "cooling 18-30C, heating 20-55C, hot_water 40-60C.",
        R"json({"type":"object","properties":{"type":{"type":"string","enum":["cooling","heating","hot_water"],"description":"Which setpoint to change"},"temperature":{"type":"integer","description":"Target temperature in degrees Celsius"}},"required":["type","temperature"],"additionalProperties":false})json"
    },
    {
        "get_parameters",
        "Get all configurable P-parameters of the heat pump with current values, ranges, "
        "and descriptions. P-parameters control advanced behavior like defrost timing, "
        "compressor limits, and safety thresholds.",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"
    },
    {
        "set_parameter",
        "Set a heat pump P-parameter by its key name (e.g. defrost_interval) or P-code "
        "(e.g. P01). Use get_parameters first to see available parameters and valid ranges.",
        R"json({"type":"object","properties":{"id":{"type":"string","description":"Parameter key name or P-code"},"value":{"type":"integer","description":"New value to set (must be within min/max range)"}},"required":["id","value"],"additionalProperties":false})json"
    },
    {
        "get_wifi_status",
        "Get WiFi connection status including SSID, signal strength (RSSI), IP address, "
        "and mDNS hostname.",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"
    },
    {
        "get_event_log",
        "Get the operational event log showing system events like startups, connections, "
        "errors, and mode changes. Returns newest events first.",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"
    },
    {
        "get_system_logs",
        "Get recent ESP debug log entries. Useful for troubleshooting firmware issues. "
        "Optionally filter by log level.",
        R"json({"type":"object","properties":{"level":{"type":"string","enum":["error","warn","info","debug","verbose"],"description":"Minimum log level to return (default: info)"}},"additionalProperties":false})json"
    },
    {
        "reboot_device",
        "Reboot the controller. The device will disconnect and take about 10 seconds "
        "to come back online. Use only when necessary (e.g., after configuration changes "
        "that require a restart).",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"
    },
};

static const int NUM_TOOLS = sizeof(TOOLS) / sizeof(TOOLS[0]);

// ============================================================================
// Resource Definitions
// ============================================================================

struct ResourceDef {
    const char* uri;
    const char* name;
    const char* description;
    const char* mime_type;
};

static const ResourceDef RESOURCES[] = {
    {
        "arctic://capabilities",
        "Device Capabilities",
        "Static description of this heat pump controller's capabilities, supported modes, "
        "temperature ranges, and communication protocols. Read this first to understand "
        "what this device can do.",
        "application/json"
    },
    {
        "arctic://status",
        "Live Heat Pump Status",
        "Current heat pump status snapshot: temperatures, mode, setpoints, component states. "
        "Equivalent to calling the get_heatpump_status tool.",
        "application/json"
    },
};

static const int NUM_RESOURCES = sizeof(RESOURCES) / sizeof(RESOURCES[0]);

// ============================================================================
// JSON-RPC Helpers
// ============================================================================

static cJSON* make_jsonrpc_response(cJSON* id, cJSON* result)
{
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    }
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON* make_jsonrpc_error(cJSON* id, int code, const char* message)
{
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON* error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(resp, "error", error);
    return resp;
}

// Helper: wrap text content in MCP tool result format
static cJSON* make_tool_result(const char* text, bool is_error = false)
{
    cJSON* result = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    if (is_error) {
        cJSON_AddBoolToObject(result, "isError", true);
    }
    return result;
}

// Helper: wrap a cJSON object as text content in MCP tool result
static cJSON* make_tool_result_json(cJSON* data, bool is_error = false)
{
    char* text = cJSON_Print(data);
    cJSON* result = make_tool_result(text, is_error);
    free(text);
    cJSON_Delete(data);
    return result;
}

// ============================================================================
// Tool Handlers
// ============================================================================

static cJSON* handle_get_device_info(cJSON* /*args*/)
{
    cJSON* info = cJSON_CreateObject();
    
    cJSON_AddStringToObject(info, "name", "Arctic Heat Pump Controller");
    cJSON_AddStringToObject(info, "hostname", api_server_get_hostname());
    cJSON_AddStringToObject(info, "platform", "ESP32-P4");
    cJSON_AddStringToObject(info, "wifi_module", "ESP32-C6");
    
    const esp_app_desc_t* app_desc = esp_app_get_description();
    cJSON_AddStringToObject(info, "firmware_version", app_desc->version);
    cJSON_AddNumberToObject(info, "free_heap_bytes", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(info, "min_free_heap_bytes", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(info, "uptime_ms", (double)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    cJSON_AddStringToObject(info, "mcp_protocol_version", MCP_PROTOCOL_VERSION);
    
    bool demo_mode = arctic::isDemoMode();
    cJSON_AddBoolToObject(info, "demo_mode", demo_mode);
    
    return make_tool_result_json(info);
}

static cJSON* handle_get_heatpump_status(cJSON* /*args*/)
{
    arctic::HeatPumpState hp = arctic::getState();
    bool demo_mode = arctic::isDemoMode();
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", hp.connected);
    cJSON_AddBoolToObject(root, "demo_mode", demo_mode);
    cJSON_AddBoolToObject(root, "unit_on", hp.unit_on);
    cJSON_AddStringToObject(root, "mode", arctic::workingModeToString(hp.working_mode));
    cJSON_AddBoolToObject(root, "defrosting", hp.isDefrosting());
    
    // Components
    cJSON* comp = cJSON_AddObjectToObject(root, "components");
    cJSON_AddBoolToObject(comp, "compressor", hp.isCompressorRunning());
    cJSON_AddBoolToObject(comp, "fans", hp.isFanRunning());
    cJSON_AddNumberToObject(comp, "fan_speed_level", hp.getFanSpeedLevel());
    cJSON_AddBoolToObject(comp, "water_pump", hp.isWaterPumpRunning());
    cJSON_AddBoolToObject(comp, "aux_heater", hp.isBackupHeaterOn());
    
    // Temperatures
    cJSON* temps = cJSON_AddObjectToObject(root, "temperatures_celsius");
    cJSON_AddNumberToObject(temps, "water_tank", hp.water_tank_temp);
    cJSON_AddNumberToObject(temps, "outlet_water", hp.outlet_water_temp);
    cJSON_AddNumberToObject(temps, "inlet_water", hp.inlet_water_temp);
    cJSON_AddNumberToObject(temps, "outdoor_ambient", hp.outdoor_ambient_temp);
    cJSON_AddNumberToObject(temps, "discharge", hp.discharge_temp);
    cJSON_AddNumberToObject(temps, "suction", hp.suction_temp);
    cJSON_AddNumberToObject(temps, "outdoor_coil", hp.outdoor_coil_temp);
    cJSON_AddNumberToObject(temps, "indoor_coil", hp.indoor_coil_temp);
    
    // Setpoints
    cJSON* sp = cJSON_AddObjectToObject(root, "setpoints_celsius");
    cJSON_AddNumberToObject(sp, "cooling", hp.cooling_setpoint);
    cJSON_AddNumberToObject(sp, "heating", hp.heating_setpoint);
    cJSON_AddNumberToObject(sp, "hot_water", hp.hot_water_setpoint);
    
    // Electrical readings
    cJSON* elec = cJSON_AddObjectToObject(root, "electrical");
    cJSON_AddNumberToObject(elec, "compressor_freq_hz", hp.compressor_freq);
    cJSON_AddNumberToObject(elec, "fan_rpm", hp.fan_speed);
    cJSON_AddNumberToObject(elec, "ac_voltage", hp.ac_voltage);
    cJSON_AddNumberToObject(elec, "ac_current", hp.ac_current);
    cJSON_AddNumberToObject(elec, "power_consumption_w", (hp.ac_voltage * hp.ac_current) / 10);
    
    // Error summary
    cJSON_AddBoolToObject(root, "has_error", hp.hasAnyError());
    if (hp.hasAnyError()) {
        char error_buf[256];
        arctic::getErrorDescriptions(error_buf, sizeof(error_buf));
        cJSON_AddStringToObject(root, "active_errors", error_buf);
    }
    
    return make_tool_result_json(root);
}

static cJSON* handle_get_heatpump_errors(cJSON* /*args*/)
{
    cJSON* root = cJSON_CreateObject();
    
    int error_count = arctic::getActiveErrorCount();
    cJSON_AddBoolToObject(root, "has_errors", error_count > 0);
    cJSON_AddNumberToObject(root, "error_count", error_count);
    cJSON_AddStringToObject(root, "highest_severity",
        arctic::severityToString(arctic::getHighestSeverity()));
    
    // Active errors (use the JSON helper)
    char* active_json = arctic::getErrorsAsJson();
    if (active_json) {
        cJSON* active = cJSON_Parse(active_json);
        if (active) {
            cJSON_AddItemToObject(root, "active", active);
        }
        free(active_json);
    }
    
    // Error history
    char* history_json = arctic::getErrorHistoryAsJson();
    if (history_json) {
        cJSON* history = cJSON_Parse(history_json);
        if (history) {
            cJSON_AddItemToObject(root, "history", history);
        }
        free(history_json);
    }
    
    return make_tool_result_json(root);
}

static cJSON* handle_set_heatpump_power(cJSON* args)
{
    if (!arctic::isConnected() && !arctic::isDemoMode()) {
        return make_tool_result("Heat pump is not connected", true);
    }
    
    cJSON* on = cJSON_GetObjectItem(args, "on");
    if (!on || !cJSON_IsBool(on)) {
        return make_tool_result("Missing required parameter: 'on' (boolean)", true);
    }
    
    bool power = cJSON_IsTrue(on);
    bool success = arctic::setUnitPower(power);
    
    if (success) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Heat pump turned %s", power ? "ON" : "OFF");
        return make_tool_result(msg);
    }
    return make_tool_result("Failed to set power — check Modbus connection", true);
}

static cJSON* handle_set_heatpump_mode(cJSON* args)
{
    if (!arctic::isConnected() && !arctic::isDemoMode()) {
        return make_tool_result("Heat pump is not connected", true);
    }
    
    cJSON* mode_item = cJSON_GetObjectItem(args, "mode");
    if (!mode_item || !cJSON_IsString(mode_item)) {
        return make_tool_result("Missing required parameter: 'mode' (string)", true);
    }
    
    const char* mode_str = mode_item->valuestring;
    arctic::WorkingMode mode;
    
    if (strcmp(mode_str, "cooling") == 0) {
        mode = arctic::WorkingMode::COOLING;
    } else if (strcmp(mode_str, "floor_heating") == 0) {
        mode = arctic::WorkingMode::FLOOR_HEATING;
    } else if (strcmp(mode_str, "fan_coil_heating") == 0) {
        mode = arctic::WorkingMode::FAN_COIL_HEATING;
    } else if (strcmp(mode_str, "hot_water") == 0) {
        mode = arctic::WorkingMode::HOT_WATER;
    } else if (strcmp(mode_str, "auto") == 0) {
        mode = arctic::WorkingMode::AUTO;
    } else {
        return make_tool_result("Invalid mode. Valid: cooling, floor_heating, fan_coil_heating, hot_water, auto", true);
    }
    
    bool success = arctic::setWorkingMode(mode);
    if (success) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Operating mode set to: %s", mode_str);
        return make_tool_result(msg);
    }
    return make_tool_result("Failed to set mode — check Modbus connection", true);
}

static cJSON* handle_set_temperature_setpoint(cJSON* args)
{
    if (!arctic::isConnected() && !arctic::isDemoMode()) {
        return make_tool_result("Heat pump is not connected", true);
    }
    
    cJSON* type_item = cJSON_GetObjectItem(args, "type");
    cJSON* temp_item = cJSON_GetObjectItem(args, "temperature");
    
    if (!type_item || !cJSON_IsString(type_item)) {
        return make_tool_result("Missing required parameter: 'type' (string)", true);
    }
    if (!temp_item || !cJSON_IsNumber(temp_item)) {
        return make_tool_result("Missing required parameter: 'temperature' (integer)", true);
    }
    
    const char* type_str = type_item->valuestring;
    int16_t temp = (int16_t)temp_item->valueint;
    bool success = false;
    
    if (strcmp(type_str, "cooling") == 0) {
        success = arctic::setCoolingSetpoint(temp);
    } else if (strcmp(type_str, "heating") == 0) {
        success = arctic::setHeatingSetpoint(temp);
    } else if (strcmp(type_str, "hot_water") == 0) {
        success = arctic::setHotWaterSetpoint(temp);
    } else {
        return make_tool_result("Invalid type. Valid: cooling, heating, hot_water", true);
    }
    
    if (success) {
        char msg[80];
        snprintf(msg, sizeof(msg), "%s setpoint set to %d°C", type_str, temp);
        return make_tool_result(msg);
    }
    return make_tool_result("Failed to set setpoint — check Modbus connection", true);
}

static cJSON* handle_get_parameters(cJSON* /*args*/)
{
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddNumberToObject(root, "count", NUM_HEATPUMP_PARAMS);
    
    cJSON* params = cJSON_AddArrayToObject(root, "parameters");
    for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
        const HeatPumpParam* param = &HEATPUMP_PARAMS[i];
        
        bool read_ok = false;
        int16_t value = heatpump_param_read(param, &read_ok);
        
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "key", param->key);
        cJSON_AddStringToObject(p, "p_code", param->p_code);
        cJSON_AddStringToObject(p, "name", param->name);
        cJSON_AddStringToObject(p, "description", param->description);
        if (read_ok) {
            cJSON_AddNumberToObject(p, "value", value);
        } else {
            cJSON_AddNullToObject(p, "value");
        }
        cJSON_AddStringToObject(p, "unit", param_unit_to_string(param->unit_type));
        cJSON_AddNumberToObject(p, "min", param->min_val);
        cJSON_AddNumberToObject(p, "max", param->max_val);
        cJSON_AddStringToObject(p, "category", param->category);
        cJSON_AddItemToArray(params, p);
    }
    
    return make_tool_result_json(root);
}

static cJSON* handle_set_parameter(cJSON* args)
{
    if (!arctic::isConnected() && !arctic::isDemoMode()) {
        return make_tool_result("Heat pump is not connected", true);
    }
    
    cJSON* id_item = cJSON_GetObjectItem(args, "id");
    cJSON* value_item = cJSON_GetObjectItem(args, "value");
    
    if (!id_item || !cJSON_IsString(id_item)) {
        return make_tool_result("Missing required parameter: 'id' (string)", true);
    }
    if (!value_item || !cJSON_IsNumber(value_item)) {
        return make_tool_result("Missing required parameter: 'value' (integer)", true);
    }
    
    const char* param_id = id_item->valuestring;
    int16_t value = (int16_t)value_item->valueint;
    
    // Look up by key or p_code
    const HeatPumpParam* param = heatpump_param_find(param_id);
    if (!param) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Unknown parameter: '%s'. Use get_parameters to see valid IDs.", param_id);
        return make_tool_result(msg, true);
    }
    
    // Range check
    if (value < param->min_val || value > param->max_val) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Value %d out of range [%d, %d] for %s", 
                 value, param->min_val, param->max_val, param->name);
        return make_tool_result(msg, true);
    }
    
    bool success = heatpump_param_write(param, value);
    if (success) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Parameter %s (%s) set to %d %s",
                 param->p_code, param->name, value, param_unit_to_string(param->unit_type));
        return make_tool_result(msg);
    }
    return make_tool_result("Failed to write parameter — check Modbus connection", true);
}

static cJSON* handle_get_wifi_status(cJSON* /*args*/)
{
    cJSON* root = cJSON_CreateObject();
    
    wifi_mgr_state_t state = wifi_mgr_get_state();
    const char* state_str;
    switch (state) {
        case WIFI_MGR_STATE_CONNECTED: state_str = "connected"; break;
        case WIFI_MGR_STATE_CONNECTING: state_str = "connecting"; break;
        case WIFI_MGR_STATE_DISCONNECTED: state_str = "disconnected"; break;
        case WIFI_MGR_STATE_NOT_INITIALIZED: state_str = "not_initialized"; break;
        default: state_str = "error"; break;
    }
    cJSON_AddStringToObject(root, "state", state_str);
    
    if (state == WIFI_MGR_STATE_CONNECTED) {
        const char* ssid = wifi_mgr_get_connected_ssid();
        if (ssid) {
            cJSON_AddStringToObject(root, "ssid", ssid);
        }
        
        char ip[16] = {0};
        if (wifi_mgr_get_ip_addr(ip, sizeof(ip))) {
            cJSON_AddStringToObject(root, "ip", ip);
        }
        
        int8_t rssi = wifi_mgr_get_rssi();
        cJSON_AddNumberToObject(root, "rssi", rssi);
        cJSON_AddStringToObject(root, "hostname", api_server_get_hostname());
        
        // Signal quality description
        const char* quality;
        if (rssi >= -50) quality = "excellent";
        else if (rssi >= -60) quality = "good";
        else if (rssi >= -70) quality = "fair";
        else quality = "poor";
        cJSON_AddStringToObject(root, "signal_quality", quality);
    }
    
    return make_tool_result_json(root);
}

static cJSON* handle_get_event_log(cJSON* /*args*/)
{
    cJSON* root = cJSON_CreateObject();
    
    int total = event_log_count();
    cJSON_AddNumberToObject(root, "total", total);
    
    // Get events (newest first, up to 128)
    event_entry_t events[128];
    int count = event_log_get(events, 128, 0);
    
    cJSON* arr = cJSON_AddArrayToObject(root, "events");
    for (int i = 0; i < count; i++) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "type", event_type_name(events[i].type));
        cJSON_AddNumberToObject(e, "timestamp", events[i].timestamp);
        cJSON_AddNumberToObject(e, "uptime_ms", events[i].uptime_ms);
        cJSON_AddItemToArray(arr, e);
    }
    
    return make_tool_result_json(root);
}

static cJSON* handle_get_system_logs(cJSON* args)
{
    // Optional level filter
    esp_log_level_t min_level = ESP_LOG_INFO;  // Default: info and above
    if (args) {
        cJSON* level = cJSON_GetObjectItem(args, "level");
        if (level && cJSON_IsString(level)) {
            const char* l = level->valuestring;
            if (strcmp(l, "error") == 0) min_level = ESP_LOG_ERROR;
            else if (strcmp(l, "warn") == 0) min_level = ESP_LOG_WARN;
            else if (strcmp(l, "info") == 0) min_level = ESP_LOG_INFO;
            else if (strcmp(l, "debug") == 0) min_level = ESP_LOG_DEBUG;
            else min_level = ESP_LOG_VERBOSE;
        }
    }
    
    // Get log entries
    log_entry_t* entries = (log_entry_t*)malloc(LOG_BUFFER_MAX_ENTRIES * sizeof(log_entry_t));
    if (!entries) {
        return make_tool_result("Out of memory", true);
    }
    
    int count = log_buffer_get(entries, LOG_BUFFER_MAX_ENTRIES, 0, min_level);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", count);
    cJSON* logs = cJSON_AddArrayToObject(root, "entries");
    
    // Return last 50 entries to avoid huge responses
    int start = count > 50 ? count - 50 : 0;
    for (int i = start; i < count; i++) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "level", log_level_char(entries[i].level));
        cJSON_AddStringToObject(e, "tag", entries[i].tag);
        cJSON_AddStringToObject(e, "message", entries[i].message);
        cJSON_AddNumberToObject(e, "uptime_ms", (double)entries[i].uptime_ms);
        cJSON_AddItemToArray(logs, e);
    }
    
    free(entries);
    return make_tool_result_json(root);
}

static cJSON* handle_reboot_device(cJSON* /*args*/)
{
    // Schedule reboot (don't reboot mid-response)
    ESP_LOGW(TAG, "Reboot requested via MCP");
    
    // Use a timer to reboot after response is sent
    esp_timer_handle_t reboot_timer;
    esp_timer_create_args_t timer_args = {
        .callback = [](void*) { esp_restart(); },
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mcp_reboot",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&timer_args, &reboot_timer);
    esp_timer_start_once(reboot_timer, 500000);  // 500ms delay
    
    return make_tool_result("Device will reboot in 500ms. It should be back online in ~10 seconds.");
}

// Dispatch table — must match TOOLS[] order
typedef cJSON* (*ToolHandler)(cJSON* args);
static const ToolHandler TOOL_HANDLERS[] = {
    handle_get_device_info,             // 0: get_device_info
    handle_get_heatpump_status,         // 1: get_heatpump_status
    handle_get_heatpump_errors,         // 2: get_heatpump_errors
    handle_set_heatpump_power,          // 3: set_heatpump_power
    handle_set_heatpump_mode,           // 4: set_heatpump_mode
    handle_set_temperature_setpoint,    // 5: set_temperature_setpoint
    handle_get_parameters,              // 6: get_parameters
    handle_set_parameter,               // 7: set_parameter
    handle_get_wifi_status,             // 8: get_wifi_status
    handle_get_event_log,               // 9: get_event_log
    handle_get_system_logs,             // 10: get_system_logs
    handle_reboot_device,               // 11: reboot_device
};

// ============================================================================
// Resource Handlers
// ============================================================================

static cJSON* read_resource_capabilities()
{
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "device", "Arctic Heat Pump Controller");
    cJSON_AddStringToObject(root, "heatpump_model", "ECO-600 Air-Source Heat Pump");
    cJSON_AddStringToObject(root, "communication", "Modbus RTU over RS485");
    cJSON_AddStringToObject(root, "platform", "ESP32-P4 (M5Stack Tab5)");
    cJSON_AddStringToObject(root, "display", "720x1280 IPS touchscreen");
    
    cJSON* modes = cJSON_AddArrayToObject(root, "supported_modes");
    cJSON_AddItemToArray(modes, cJSON_CreateString("cooling"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("floor_heating"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("fan_coil_heating"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("hot_water"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("auto"));
    
    cJSON* temp_ranges = cJSON_AddObjectToObject(root, "temperature_ranges_celsius");
    cJSON* cooling_r = cJSON_AddObjectToObject(temp_ranges, "cooling_setpoint");
    cJSON_AddNumberToObject(cooling_r, "min", 18);
    cJSON_AddNumberToObject(cooling_r, "max", 30);
    cJSON* heating_r = cJSON_AddObjectToObject(temp_ranges, "heating_setpoint");
    cJSON_AddNumberToObject(heating_r, "min", 20);
    cJSON_AddNumberToObject(heating_r, "max", 55);
    cJSON* hw_r = cJSON_AddObjectToObject(temp_ranges, "hot_water_setpoint");
    cJSON_AddNumberToObject(hw_r, "min", 40);
    cJSON_AddNumberToObject(hw_r, "max", 60);
    
    cJSON* sensors = cJSON_AddArrayToObject(root, "temperature_sensors");
    const char* sensor_names[] = {
        "water_tank", "outlet_water", "inlet_water", "outdoor_ambient",
        "discharge", "suction", "outdoor_coil", "indoor_coil", "ipm"
    };
    for (int i = 0; i < 9; i++) {
        cJSON_AddItemToArray(sensors, cJSON_CreateString(sensor_names[i]));
    }
    
    cJSON* features = cJSON_AddArrayToObject(root, "features");
    cJSON_AddItemToArray(features, cJSON_CreateString("remote_power_control"));
    cJSON_AddItemToArray(features, cJSON_CreateString("mode_selection"));
    cJSON_AddItemToArray(features, cJSON_CreateString("setpoint_adjustment"));
    cJSON_AddItemToArray(features, cJSON_CreateString("parameter_tuning"));
    cJSON_AddItemToArray(features, cJSON_CreateString("error_monitoring"));
    cJSON_AddItemToArray(features, cJSON_CreateString("ota_updates"));
    cJSON_AddItemToArray(features, cJSON_CreateString("event_logging"));
    cJSON_AddItemToArray(features, cJSON_CreateString("display_screenshot"));
    
    return root;
}

// ============================================================================
// MCP Method Dispatch
// ============================================================================

static cJSON* handle_initialize(cJSON* id, cJSON* /*params*/)
{
    ESP_LOGI(TAG, "MCP initialize request received");
    
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);
    
    // Server capabilities
    cJSON* caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");      // We support tools
    cJSON_AddObjectToObject(caps, "resources");   // We support resources
    
    // Server info
    cJSON* server_info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(server_info, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(server_info, "version", MCP_SERVER_VERSION);
    
    // Instructions for the LLM
    cJSON_AddStringToObject(result, "instructions",
        "You are connected to an Arctic Heat Pump Controller — a residential "
        "air-source heat pump (ECO-600) managed by an ESP32-P4 microcontroller. "
        "Start with get_device_info to check the system, then use get_heatpump_status "
        "to see current conditions. Read the arctic://capabilities resource for full "
        "details on supported modes and temperature ranges. "
        "For diagnostics, check get_heatpump_errors and get_event_log. "
        "Temperature values are in Celsius unless the user specifies otherwise.");
    
    return make_jsonrpc_response(id, result);
}

static cJSON* handle_ping(cJSON* id)
{
    return make_jsonrpc_response(id, cJSON_CreateObject());
}

static cJSON* handle_tools_list(cJSON* id, cJSON* /*params*/)
{
    cJSON* result = cJSON_CreateObject();
    cJSON* tools = cJSON_AddArrayToObject(result, "tools");
    
    for (int i = 0; i < NUM_TOOLS; i++) {
        cJSON* tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", TOOLS[i].name);
        cJSON_AddStringToObject(tool, "description", TOOLS[i].description);
        
        // Parse the pre-built schema string into a cJSON object
        cJSON* schema = cJSON_Parse(TOOLS[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "inputSchema", schema);
        }
        
        cJSON_AddItemToArray(tools, tool);
    }
    
    return make_jsonrpc_response(id, result);
}

static cJSON* handle_tools_call(cJSON* id, cJSON* params)
{
    cJSON* name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        return make_jsonrpc_error(id, -32602, "Missing 'name' in tools/call params");
    }
    
    cJSON* args = cJSON_GetObjectItem(params, "arguments");
    
    // Find and execute the tool
    const char* tool_name = name->valuestring;
    for (int i = 0; i < NUM_TOOLS; i++) {
        if (strcmp(tool_name, TOOLS[i].name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", tool_name);
            cJSON* tool_result = TOOL_HANDLERS[i](args);
            return make_jsonrpc_response(id, tool_result);
        }
    }
    
    // Unknown tool
    char errmsg[128];
    snprintf(errmsg, sizeof(errmsg), "Unknown tool: %s", tool_name);
    return make_jsonrpc_error(id, -32602, errmsg);
}

static cJSON* handle_resources_list(cJSON* id, cJSON* /*params*/)
{
    cJSON* result = cJSON_CreateObject();
    cJSON* resources = cJSON_AddArrayToObject(result, "resources");
    
    for (int i = 0; i < NUM_RESOURCES; i++) {
        cJSON* res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "uri", RESOURCES[i].uri);
        cJSON_AddStringToObject(res, "name", RESOURCES[i].name);
        cJSON_AddStringToObject(res, "description", RESOURCES[i].description);
        cJSON_AddStringToObject(res, "mimeType", RESOURCES[i].mime_type);
        cJSON_AddItemToArray(resources, res);
    }
    
    return make_jsonrpc_response(id, result);
}

static cJSON* handle_resources_read(cJSON* id, cJSON* params)
{
    cJSON* uri_item = cJSON_GetObjectItem(params, "uri");
    if (!uri_item || !cJSON_IsString(uri_item)) {
        return make_jsonrpc_error(id, -32602, "Missing 'uri' in resources/read params");
    }
    
    const char* uri = uri_item->valuestring;
    cJSON* data = NULL;
    
    if (strcmp(uri, "arctic://capabilities") == 0) {
        data = read_resource_capabilities();
    } else if (strcmp(uri, "arctic://status") == 0) {
        // Reuse the status tool handler — extract the text from the result
        cJSON* tool_result = handle_get_heatpump_status(NULL);
        // The tool_result contains {content: [{type: "text", text: "..."}]}
        cJSON* content = cJSON_GetObjectItem(tool_result, "content");
        cJSON* first = cJSON_GetArrayItem(content, 0);
        cJSON* text = cJSON_GetObjectItem(first, "text");
        
        // Build resource read result
        cJSON* result = cJSON_CreateObject();
        cJSON* contents = cJSON_AddArrayToObject(result, "contents");
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "uri", uri);
        cJSON_AddStringToObject(item, "mimeType", "application/json");
        cJSON_AddStringToObject(item, "text", text->valuestring);
        cJSON_AddItemToArray(contents, item);
        
        cJSON_Delete(tool_result);
        return make_jsonrpc_response(id, result);
    } else {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg), "Unknown resource: %s", uri);
        return make_jsonrpc_error(id, -32602, errmsg);
    }
    
    // Build resource read result for capabilities
    char* text = cJSON_Print(data);
    cJSON_Delete(data);
    
    cJSON* result = cJSON_CreateObject();
    cJSON* contents = cJSON_AddArrayToObject(result, "contents");
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "uri", uri);
    cJSON_AddStringToObject(item, "mimeType", "application/json");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(contents, item);
    free(text);
    
    return make_jsonrpc_response(id, result);
}

// ============================================================================
// Main JSON-RPC Dispatcher
// ============================================================================

static cJSON* dispatch_jsonrpc(cJSON* request)
{
    cJSON* method = cJSON_GetObjectItem(request, "method");
    cJSON* id = cJSON_GetObjectItem(request, "id");
    cJSON* params = cJSON_GetObjectItem(request, "params");
    
    if (!method || !cJSON_IsString(method)) {
        return make_jsonrpc_error(id, -32600, "Invalid Request: missing 'method'");
    }
    
    const char* m = method->valuestring;
    
    // Notifications (no id) — acknowledge but don't return a response
    if (!id) {
        if (strcmp(m, "notifications/initialized") == 0) {
            ESP_LOGI(TAG, "Client initialized notification received");
            return NULL;  // No response for notifications
        }
        if (strcmp(m, "notifications/cancelled") == 0) {
            return NULL;  // Acknowledge cancellation
        }
        // Unknown notification — ignore
        return NULL;
    }
    
    // Methods that require a response
    if (strcmp(m, "initialize") == 0) {
        return handle_initialize(id, params);
    }
    if (strcmp(m, "ping") == 0) {
        return handle_ping(id);
    }
    if (strcmp(m, "tools/list") == 0) {
        return handle_tools_list(id, params);
    }
    if (strcmp(m, "tools/call") == 0) {
        if (!params) {
            return make_jsonrpc_error(id, -32602, "tools/call requires params");
        }
        return handle_tools_call(id, params);
    }
    if (strcmp(m, "resources/list") == 0) {
        return handle_resources_list(id, params);
    }
    if (strcmp(m, "resources/read") == 0) {
        if (!params) {
            return make_jsonrpc_error(id, -32602, "resources/read requires params");
        }
        return handle_resources_read(id, params);
    }
    
    // Method not found
    char errmsg[128];
    snprintf(errmsg, sizeof(errmsg), "Method not found: %s", m);
    return make_jsonrpc_error(id, -32601, errmsg);
}

// ============================================================================
// HTTP Handlers
// ============================================================================

static esp_err_t mcp_post_handler(httpd_req_t* req)
{
    // Validate Content-Type
    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        if (strstr(content_type, "application/json") == NULL) {
            httpd_resp_set_status(req, "415 Unsupported Media Type");
            httpd_resp_sendstr(req, "");
            return ESP_OK;
        }
    }
    
    // Read request body
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > MCP_MAX_REQUEST_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }
    
    char* body = (char*)malloc(content_len + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }
    
    int received = httpd_req_recv(req, body, content_len);
    if (received <= 0) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }
    body[received] = '\0';
    
    // Parse JSON-RPC request
    cJSON* request = cJSON_Parse(body);
    free(body);
    
    if (!request) {
        httpd_resp_set_type(req, "application/json");
        cJSON* error = make_jsonrpc_error(NULL, -32700, "Parse error");
        char* error_str = cJSON_PrintUnformatted(error);
        httpd_resp_sendstr(req, error_str);
        free(error_str);
        cJSON_Delete(error);
        return ESP_OK;
    }
    
    // Handle batch or single request
    cJSON* response = NULL;
    
    if (cJSON_IsArray(request)) {
        // Batch request — process each and collect responses
        cJSON* batch_response = cJSON_CreateArray();
        int size = cJSON_GetArraySize(request);
        bool has_responses = false;
        
        for (int i = 0; i < size; i++) {
            cJSON* item = cJSON_GetArrayItem(request, i);
            cJSON* item_response = dispatch_jsonrpc(item);
            if (item_response) {
                cJSON_AddItemToArray(batch_response, item_response);
                has_responses = true;
            }
        }
        
        if (has_responses) {
            response = batch_response;
        } else {
            // All notifications — return 202 Accepted
            cJSON_Delete(batch_response);
            cJSON_Delete(request);
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_sendstr(req, "");
            return ESP_OK;
        }
    } else {
        // Single request
        response = dispatch_jsonrpc(request);
        
        if (!response) {
            // Notification — return 202 Accepted
            cJSON_Delete(request);
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_sendstr(req, "");
            return ESP_OK;
        }
    }
    
    // Send JSON response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char* response_str = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, response_str);
    
    free(response_str);
    cJSON_Delete(response);
    cJSON_Delete(request);
    
    return ESP_OK;
}

// GET /mcp — return 405 (we don't support SSE streaming)
static esp_err_t mcp_get_handler(httpd_req_t* req)
{
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_type(req, "application/json");
    
    cJSON* error = make_jsonrpc_error(NULL, -32600, 
        "SSE streaming not supported. Use POST for all MCP requests.");
    char* error_str = cJSON_PrintUnformatted(error);
    httpd_resp_sendstr(req, error_str);
    free(error_str);
    cJSON_Delete(error);
    
    return ESP_OK;
}

// DELETE /mcp — session termination (we're stateless, just acknowledge)
static esp_err_t mcp_delete_handler(httpd_req_t* req)
{
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// OPTIONS /mcp — CORS preflight
static esp_err_t mcp_options_handler(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", 
        "Content-Type, Accept, Mcp-Session-Id");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// ============================================================================
// Well-known MCP discovery endpoint (RFC 8615)
// ============================================================================

static esp_err_t mcp_well_known_handler(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Location", "/mcp");
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// ============================================================================
// Registration
// ============================================================================

bool mcp_server_register(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Registering MCP server endpoints on /mcp");
    
    httpd_uri_t post_uri = {
        .uri = "/mcp",
        .method = HTTP_POST,
        .handler = mcp_post_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t get_uri = {
        .uri = "/mcp",
        .method = HTTP_GET,
        .handler = mcp_get_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t delete_uri = {
        .uri = "/mcp",
        .method = HTTP_DELETE,
        .handler = mcp_delete_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t options_uri = {
        .uri = "/mcp",
        .method = HTTP_OPTIONS,
        .handler = mcp_options_handler,
        .user_ctx = NULL
    };
    
    esp_err_t err;
    
    err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register POST /mcp: %s", esp_err_to_name(err));
        return false;
    }
    
    err = httpd_register_uri_handler(server, &get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /mcp: %s", esp_err_to_name(err));
        return false;
    }
    
    err = httpd_register_uri_handler(server, &delete_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register DELETE /mcp: %s", esp_err_to_name(err));
        return false;
    }
    
    err = httpd_register_uri_handler(server, &options_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS /mcp: %s", esp_err_to_name(err));
        return false;
    }
    
    // Well-known MCP discovery endpoint (RFC 8615, MCP spec 2025-03-26)
    httpd_uri_t well_known_uri = {
        .uri = "/.well-known/mcp",
        .method = HTTP_GET,
        .handler = mcp_well_known_handler,
        .user_ctx = NULL
    };
    
    err = httpd_register_uri_handler(server, &well_known_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GET /.well-known/mcp: %s", esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "MCP server registered: POST/GET/DELETE/OPTIONS /mcp, GET /.well-known/mcp (%d tools, %d resources)",
             NUM_TOOLS, NUM_RESOURCES);
    
    return true;
}
