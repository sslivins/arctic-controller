/*
 * Arctic Heat Pump Controller
 * P-Parameter Definitions
 * 
 * Shared between UI and API server
 */
#pragma once

#include <stdint.h>
#include "modbus/arctic_registers.h"
#include "i18n/strings.h"

// Unit types for parameters
enum class ParamUnit : uint8_t {
    NONE,           // Unitless (mode selectors, etc.)
    STEPS,          // EEV steps
    MINUTES,        // Time in minutes
    SECONDS,        // Time in seconds
    TEMP_ABSOLUTE,  // Temperature (convert with +32 for F)
    TEMP_OFFSET     // Temperature difference (no +32 for F)
};

// Parameter definition structure
struct HeatPumpParam {
    const char* key;            // API key: "eev_opening", "defrost_cycle", etc.
    const char* p_code;         // Arctic P-code: "P1", "P41", etc.
    const char* name;           // English display name (also used in API)
    string_id_t name_id;        // Translated display name ID
    const char* description;    // Full description for OpenAPI docs
    uint16_t reg_addr;          // Modbus register address
    int16_t min_val;            // Minimum value (Celsius for temps)
    int16_t max_val;            // Maximum value (Celsius for temps)
    ParamUnit unit_type;        // Unit type for conversion
    const char* category;       // Category for grouping in UI
};

// All P-parameter definitions
static const HeatPumpParam HEATPUMP_PARAMS[] = {
    // EEV Settings
    {
        .key = "eev_opening",
        .p_code = "P1",
        .name = "EEV Opening",
        .name_id = STR_HP_PARAM_EEV_OPENING,
        .description = "Initial EEV opening position when compressor starts. Higher values allow more refrigerant flow.",
        .reg_addr = arctic::reg::P1_EEV_INITIAL_OPENING,
        .min_val = 0, .max_val = 500,
        .unit_type = ParamUnit::STEPS,
        .category = "EEV"
    },
    {
        .key = "eev_mode",
        .p_code = "P41",
        .name = "EEV Mode",
        .name_id = STR_HP_PARAM_EEV_MODE,
        .description = "EEV control mode. 0=Superheat control (automatic), 1=Fixed position.",
        .reg_addr = arctic::reg::P41_EEV_SUPERHEAT_MODE,
        .min_val = 0, .max_val = 1,
        .unit_type = ParamUnit::NONE,
        .category = "EEV"
    },
    {
        .key = "target_superheat",
        .p_code = "P42",
        .name = "Target Superheat",
        .name_id = STR_HP_PARAM_TARGET_SUPERHEAT,
        .description = "Target superheat temperature for EEV superheat control mode.",
        .reg_addr = arctic::reg::P42_EEV_TARGET_SUPERHEAT,
        .min_val = 0, .max_val = 20,
        .unit_type = ParamUnit::TEMP_OFFSET,
        .category = "EEV"
    },
    
    // Defrost Settings
    {
        .key = "defrost_cycle",
        .p_code = "P29",
        .name = "Defrost Cycle",
        .name_id = STR_HP_PARAM_DEFROST_CYCLE,
        .description = "Minimum time between defrost cycles during heating operation.",
        .reg_addr = arctic::reg::P29_DEFROST_CYCLE,
        .min_val = 30, .max_val = 120,
        .unit_type = ParamUnit::MINUTES,
        .category = "Defrost"
    },
    {
        .key = "defrost_enter_temp",
        .p_code = "P30",
        .name = "Defrost Enter Temp",
        .name_id = STR_HP_PARAM_DEFROST_ENTER_TEMP,
        .description = "Outdoor coil temperature to initiate defrost cycle.",
        .reg_addr = arctic::reg::P30_DEFROST_ENTER_TEMP,
        .min_val = -20, .max_val = 5,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Defrost"
    },
    {
        .key = "defrost_extend_temp",
        .p_code = "P31",
        .name = "Defrost Extend Temp",
        .name_id = STR_HP_PARAM_DEFROST_EXTEND_TEMP,
        .description = "Ambient temperature below which defrost time is extended.",
        .reg_addr = arctic::reg::P31_DEFROST_EXTEND_TEMP,
        .min_val = -20, .max_val = 10,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Defrost"
    },
    {
        .key = "defrost_temp_diff",
        .p_code = "P32",
        .name = "Defrost Temp Diff",
        .name_id = STR_HP_PARAM_DEFROST_TEMP_DIFF,
        .description = "Temperature difference between ambient and coil to trigger defrost.",
        .reg_addr = arctic::reg::P32_DEFROST_TEMP_DIFF,
        .min_val = 0, .max_val = 30,
        .unit_type = ParamUnit::TEMP_OFFSET,
        .category = "Defrost"
    },
    {
        .key = "defrost_extend_time",
        .p_code = "P33",
        .name = "Defrost Extend Time",
        .name_id = STR_HP_PARAM_DEFROST_EXTEND_TIME,
        .description = "Additional defrost cycle time when ambient is below P31.",
        .reg_addr = arctic::reg::P33_DEFROST_EXTEND_TIME,
        .min_val = 0, .max_val = 60,
        .unit_type = ParamUnit::MINUTES,
        .category = "Defrost"
    },
    {
        .key = "max_defrost_time",
        .p_code = "P34",
        .name = "Max Defrost Time",
        .name_id = STR_HP_PARAM_MAX_DEFROST_TIME,
        .description = "Maximum duration of a single defrost cycle.",
        .reg_addr = arctic::reg::P34_MAX_DEFROST_TIME,
        .min_val = 1, .max_val = 30,
        .unit_type = ParamUnit::MINUTES,
        .category = "Defrost"
    },
    {
        .key = "defrost_exit_temp",
        .p_code = "P35",
        .name = "Defrost Exit Temp",
        .name_id = STR_HP_PARAM_DEFROST_EXIT_TEMP,
        .description = "Outdoor coil temperature to end defrost cycle early.",
        .reg_addr = arctic::reg::P35_DEFROST_EXIT_TEMP,
        .min_val = 5, .max_val = 30,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Defrost"
    },
    
    // Protection Settings
    {
        .key = "low_ambient_protect",
        .p_code = "P38",
        .name = "Low Ambient Protect",
        .name_id = STR_HP_PARAM_LOW_AMBIENT_PROTECT,
        .description = "Minimum ambient temperature for heating operation. Unit stops below this.",
        .reg_addr = arctic::reg::P38_LOW_AMBIENT_PROTECT,
        .min_val = -30, .max_val = 10,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Protection"
    },
    {
        .key = "freq_reduction",
        .p_code = "P39",
        .name = "Freq Reduction",
        .name_id = STR_HP_PARAM_FREQ_REDUCTION,
        .description = "Temperature difference from setpoint to start reducing compressor frequency.",
        .reg_addr = arctic::reg::P39_FREQ_REDUCTION,
        .min_val = 0, .max_val = 10,
        .unit_type = ParamUnit::TEMP_OFFSET,
        .category = "Protection"
    },
    {
        .key = "cooling_low_ambient",
        .p_code = "P40",
        .name = "Cooling Low Ambient",
        .name_id = STR_HP_PARAM_COOLING_LOW_AMBIENT,
        .description = "Minimum ambient temperature for cooling operation.",
        .reg_addr = arctic::reg::P40_COOLING_LOW_AMBIENT,
        .min_val = -10, .max_val = 20,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Protection"
    },
    
    // Auto Mode Settings
    {
        .key = "max_setting_temp",
        .p_code = "P13",
        .name = "Max Setting Temp",
        .name_id = STR_HP_PARAM_MAX_SETTING_TEMP,
        .description = "Maximum allowed temperature setpoint for hot water.",
        .reg_addr = arctic::reg::P13_MAX_TEMP_SETTING,
        .min_val = 40, .max_val = 65,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Auto Mode"
    },
    {
        .key = "cooling_auto_temp",
        .p_code = "P23",
        .name = "Cooling Auto Temp",
        .name_id = STR_HP_PARAM_COOLING_AUTO_TEMP,
        .description = "Ambient temperature above which auto mode switches to cooling.",
        .reg_addr = arctic::reg::P23_COOLING_AUTO_TEMP,
        .min_val = 15, .max_val = 35,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Auto Mode"
    },
    {
        .key = "heating_auto_temp",
        .p_code = "P24",
        .name = "Heating Auto Temp",
        .name_id = STR_HP_PARAM_HEATING_AUTO_TEMP,
        .description = "Ambient temperature below which auto mode switches to heating.",
        .reg_addr = arctic::reg::P24_HEATING_AUTO_TEMP,
        .min_val = 5, .max_val = 25,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Auto Mode"
    },
    {
        .key = "mode_switch_delay",
        .p_code = "P28",
        .name = "Mode Switch Delay",
        .name_id = STR_HP_PARAM_MODE_SWITCH_DELAY,
        .description = "Minimum time before auto mode can switch between heating and cooling.",
        .reg_addr = arctic::reg::P28_MODE_SWITCH_DELAY,
        .min_val = 0, .max_val = 60,
        .unit_type = ParamUnit::MINUTES,
        .category = "Auto Mode"
    },
    
    // Pump & Valve Settings
    {
        .key = "sterilize_time",
        .p_code = "P5",
        .name = "Sterilize Time",
        .name_id = STR_HP_PARAM_STERILIZE_TIME,
        .description = "Duration of high-temperature sterilization cycle for hot water tank.",
        .reg_addr = arctic::reg::P5_STERILIZING_TIME,
        .min_val = 0, .max_val = 120,
        .unit_type = ParamUnit::MINUTES,
        .category = "Pump & Valve"
    },
    {
        .key = "water_return_temp",
        .p_code = "P36",
        .name = "Water Return Temp",
        .name_id = STR_HP_PARAM_WATER_RETURN_TEMP,
        .description = "Target water temperature for return water cycle.",
        .reg_addr = arctic::reg::P36_WATER_RETURN_TEMP,
        .min_val = 20, .max_val = 50,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Pump & Valve"
    },
    {
        .key = "water_return_time",
        .p_code = "P37",
        .name = "Water Return Time",
        .name_id = STR_HP_PARAM_WATER_RETURN_TIME,
        .description = "Duration of water return circulation cycle.",
        .reg_addr = arctic::reg::P37_WATER_RETURN_TIME,
        .min_val = 0, .max_val = 60,
        .unit_type = ParamUnit::MINUTES,
        .category = "Pump & Valve"
    },
    {
        .key = "3way_valve_time",
        .p_code = "P43",
        .name = "3-Way Valve Time",
        .name_id = STR_HP_PARAM_3WAY_VALVE_TIME,
        .description = "Time for 3-way valve to fully switch positions.",
        .reg_addr = arctic::reg::P43_3WAY_VALVE_TIME,
        .min_val = 0, .max_val = 300,
        .unit_type = ParamUnit::SECONDS,
        .category = "Pump & Valve"
    },
    {
        .key = "pump_mode",
        .p_code = "P44",
        .name = "Pump Mode",
        .name_id = STR_HP_PARAM_PUMP_MODE,
        .description = "Water pump behavior at setpoint. 0=Per P45 interval, 1=OFF, 2=Always ON.",
        .reg_addr = arctic::reg::P44_PUMP_TARGET_MODE,
        .min_val = 0, .max_val = 2,
        .unit_type = ParamUnit::NONE,
        .category = "Pump & Valve"
    },
    {
        .key = "pump_interval",
        .p_code = "P45",
        .name = "Pump Interval",
        .name_id = STR_HP_PARAM_PUMP_INTERVAL,
        .description = "Pump running interval when at setpoint (if P44=0).",
        .reg_addr = arctic::reg::P45_PUMP_INTERVAL,
        .min_val = 0, .max_val = 60,
        .unit_type = ParamUnit::MINUTES,
        .category = "Pump & Valve"
    },
    {
        .key = "pump_low_ambient",
        .p_code = "P46",
        .name = "Pump Low Ambient",
        .name_id = STR_HP_PARAM_PUMP_LOW_AMBIENT,
        .description = "Ambient temperature below which pump runs in standby to prevent freezing.",
        .reg_addr = arctic::reg::P46_PUMP_LOW_AMBIENT,
        .min_val = -20, .max_val = 10,
        .unit_type = ParamUnit::TEMP_ABSOLUTE,
        .category = "Pump & Valve"
    },
    {
        .key = "waterway_clean",
        .p_code = "P47",
        .name = "Waterway Clean",
        .name_id = STR_HP_PARAM_WATERWAY_CLEAN,
        .description = "Waterway cleaning mode. 0=OFF, 1=Pump only, 2=+3WV1, 3=+3WV1+3WV2.",
        .reg_addr = arctic::reg::P47_WATERWAY_CLEANING,
        .min_val = 0, .max_val = 3,
        .unit_type = ParamUnit::NONE,
        .category = "Pump & Valve"
    },
};

static const int NUM_HEATPUMP_PARAMS = sizeof(HEATPUMP_PARAMS) / sizeof(HEATPUMP_PARAMS[0]);

// Lookup functions
const HeatPumpParam* heatpump_param_find_by_key(const char* key);
const HeatPumpParam* heatpump_param_find_by_pcode(const char* pcode);
const HeatPumpParam* heatpump_param_find(const char* id);  // Accepts either key or p_code
int heatpump_param_get_index(const HeatPumpParam* param);  // Get array index for a param

// Unit string helpers
const char* param_unit_to_string(ParamUnit unit);

// Value read/write (handles demo mode internally)
// Returns value on success, or default on failure. Sets *success if provided.
int16_t heatpump_param_read(const HeatPumpParam* param, bool* success = nullptr);
int16_t heatpump_param_read_by_index(int idx, bool* success = nullptr);

// Write value. Returns true on success.
bool heatpump_param_write(const HeatPumpParam* param, int16_t value);
bool heatpump_param_write_by_index(int idx, int16_t value);
