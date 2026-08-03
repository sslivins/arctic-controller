/*
 * Arctic Heat Pump State and Control
 */

#include "arctic_heatpump.h"
#include "modbus_manager.h"
#include "macon_master.h"
#include "heatpump_errors.h"
#include "event_log.h"
#include "macon_state.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "arctic";

namespace arctic {

// State protected by mutex
static HeatPumpState s_state;
static SemaphoreHandle_t s_state_mutex = nullptr;
static TaskHandle_t s_poll_task = nullptr;
static bool s_polling_enabled = false;
static bool s_demo_mode = false;
static bool s_feed_mode = false;  // Passive Tuya external-feed mode

// Connection state tracking
static const uint8_t MAX_CONSECUTIVE_FAILURES = 5;
static bool s_was_connected = false;  // For logging state changes

// Previous state for event detection (compared each poll cycle)
static bool s_prev_unit_on = false;
static WorkingMode s_prev_mode = WorkingMode::COOLING;
static bool s_prev_compressor = false;
static bool s_prev_fan = false;
static bool s_prev_pump = false;
static bool s_prev_aux_heater = false;
static bool s_prev_defrosting = false;
static uint16_t s_prev_error1 = 0;
static uint16_t s_prev_error2 = 0;
static int16_t s_prev_cooling_sp = 0;
static int16_t s_prev_heating_sp = 0;
static int16_t s_prev_hotwater_sp = 0;
static bool s_prev_state_valid = false;  // False until first successful poll

// Get current time in milliseconds
static uint32_t getTimeMs() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================================
// Demo Register Array
// ============================================================================
// In demo mode, poll functions read from this array instead of Modbus.
// Setters write here instead of via Modbus. Covers addresses 2000-2138.
static const uint16_t DEMO_REG_BASE = 2000;
static const uint16_t DEMO_REG_COUNT = 143; // 2000..2142 inclusive (telemetry window reaches reg2142)
static uint16_t s_demo_regs[DEMO_REG_COUNT];

// Read multiple registers - from demo array in demo/feed mode, Modbus otherwise
static esp_err_t readRegisters(uint16_t address, uint16_t count, uint16_t* data) {
    if (s_demo_mode || s_feed_mode) {
        uint16_t offset = address - DEMO_REG_BASE;
        memcpy(data, &s_demo_regs[offset], count * sizeof(uint16_t));
        return ESP_OK;
    }
    return modbus::readHoldingRegisters(SLAVE_ADDRESS, address, count, data);
}

// Write a single register - to demo array in demo mode, Modbus otherwise
static esp_err_t writeSingleReg(uint16_t address, uint16_t value) {
    if (s_demo_mode) {
        uint16_t offset = address - DEMO_REG_BASE;
        s_demo_regs[offset] = value;
        // Simulate heat pump acking power command via STATUS_1 bit 0
        if (address == reg::UNIT_ON_OFF) {
            uint16_t& st1 = s_demo_regs[reg::STATUS_1 - DEMO_REG_BASE];
            if (value) st1 |= status1::UNIT_ON;
            else       st1 &= ~status1::UNIT_ON;
        }
        return ESP_OK;
    }
    return modbus::writeSingleRegister(SLAVE_ADDRESS, address, value);
}

// Poll holding registers (settings)
static bool pollHoldingRegisters() {
    uint16_t data[8];
    esp_err_t err = readRegisters(reg::UNIT_ON_OFF, 8, data);
    
    if (err != ESP_OK) {
        return false;
    }
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    // unit_on is derived from STATUS_1 bit 0 in pollStatus(), not from the command register
    s_state.working_mode = static_cast<WorkingMode>(data[1]);
    s_state.cooling_setpoint = static_cast<int16_t>(data[2]);
    s_state.heating_setpoint = static_cast<int16_t>(data[3]);
    s_state.hot_water_setpoint = static_cast<int16_t>(data[4]);
    xSemaphoreGive(s_state_mutex);
    
    return true;
}

// Poll temperature registers (2100-2117)
static bool pollTemperatures() {
    uint16_t data[18];  // 2100-2117
    esp_err_t err = readRegisters(reg::WATER_TANK_TEMP, 18, data);
    
    if (err != ESP_OK) {
        return false;
    }
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.water_tank_temp = static_cast<int16_t>(data[0]);       // 2100
    // data[1] is reserved (2101)
    s_state.outlet_water_temp = static_cast<int16_t>(data[2]);     // 2102
    s_state.inlet_water_temp = static_cast<int16_t>(data[3]);      // 2103
    s_state.discharge_temp = static_cast<int16_t>(data[4]);        // 2104
    s_state.suction_temp = static_cast<int16_t>(data[5]);          // 2105
    // data[6] is EVI suction (2106)
    s_state.outdoor_coil_temp = static_cast<int16_t>(data[7]);     // 2107
    s_state.indoor_coil_temp = static_cast<int16_t>(data[8]);      // 2108
    // data[9] is indoor ambient (2109)
    s_state.outdoor_ambient_temp = static_cast<int16_t>(data[10]); // 2110
    // data[11-13] are saturation temps (2111-2113)
    s_state.ipm_temp = static_cast<int16_t>(data[14]);             // 2114
    // data[15-17] are reserved temps (2115-2117)
    xSemaphoreGive(s_state_mutex);
    
    return true;
}

// Poll system readings (2118-2127)
static bool pollSystemReadings() {
    uint16_t data[10];  // 2118-2127
    esp_err_t err = readRegisters(reg::COMPRESSOR_FREQ, 10, data);
    
    if (err != ESP_OK) {
        return false;
    }
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.compressor_freq = data[0];        // 2118
    s_state.fan_speed = data[1];              // 2119
    s_state.ac_voltage = data[2];             // 2120
    s_state.ac_current = data[3];             // 2121
    s_state.dc_voltage = data[4] / 10;        // 2122 (raw tenths-of-volts -> volts; legacy path)
    s_state.dc_current = data[5];             // 2123
    s_state.primary_eev_opening = data[6];    // 2124
    s_state.secondary_eev_opening = data[7];  // 2125
    s_state.high_pressure = data[8];          // 2126
    s_state.low_pressure = data[9];           // 2127
    xSemaphoreGive(s_state_mutex);
    
    return true;
}

// Poll status and error registers (2135-2138)
static bool pollStatus() {
    uint16_t data[4];
    esp_err_t err = readRegisters(reg::STATUS_1, 4, data);
    
    if (err != ESP_OK) {
        return false;
    }
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.status1 = data[0];  // 2135
    s_state.status2 = data[1];  // 2136
    s_state.error1 = data[2];   // 2137
    s_state.error2 = data[3];   // 2138
    s_state.unit_on = (s_state.status1 & status1::UNIT_ON) != 0;
    xSemaphoreGive(s_state_mutex);
    
    return true;
}

// Compare the freshly-updated s_state against the previous snapshot and record
// operational events (power/mode/setpoint/component/defrost/error transitions).
// The caller MUST hold s_state_mutex. Used by both the Modbus polling task and
// the passive/active Tuya external-feed path so events are logged regardless of
// which data source is driving the controller.
static void detectAndLogStateEvents() {
    // ---- Event detection: compare current vs previous state ----
    if (s_prev_state_valid) {
        // Power on/off
        if (s_state.unit_on != s_prev_unit_on) {
            event_log_record(s_state.unit_on ? EVENT_POWER_ON : EVENT_POWER_OFF, 0);
        }
        // Mode changed
        if (s_state.working_mode != s_prev_mode) {
            uint32_t payload = ((uint32_t)s_prev_mode << 8) | (uint32_t)s_state.working_mode;
            event_log_record(EVENT_MODE_CHANGED, payload);
        }
        // Setpoint changes
        if (s_state.cooling_setpoint != s_prev_cooling_sp) {
            uint32_t payload = (0 << 16) | ((uint16_t)s_prev_cooling_sp << 8) | (uint16_t)s_state.cooling_setpoint;
            event_log_record(EVENT_SETPOINT_CHANGED, payload);
        }
        if (s_state.heating_setpoint != s_prev_heating_sp) {
            uint32_t payload = (1 << 16) | ((uint16_t)s_prev_heating_sp << 8) | (uint16_t)s_state.heating_setpoint;
            event_log_record(EVENT_SETPOINT_CHANGED, payload);
        }
        if (s_state.hot_water_setpoint != s_prev_hotwater_sp) {
            uint32_t payload = (2 << 16) | ((uint16_t)s_prev_hotwater_sp << 8) | (uint16_t)s_state.hot_water_setpoint;
            event_log_record(EVENT_SETPOINT_CHANGED, payload);
        }
        // Component state changes
        bool cur_comp = s_state.isCompressorRunning();
        if (cur_comp != s_prev_compressor) {
            event_log_record(cur_comp ? EVENT_COMPRESSOR_ON : EVENT_COMPRESSOR_OFF, 0);
        }
        bool cur_fan = s_state.isFanRunning();
        if (cur_fan != s_prev_fan) {
            event_log_record(cur_fan ? EVENT_FAN_ON : EVENT_FAN_OFF, 0);
        }
        bool cur_pump = s_state.isWaterPumpRunning();
        if (cur_pump != s_prev_pump) {
            event_log_record(cur_pump ? EVENT_PUMP_ON : EVENT_PUMP_OFF, 0);
        }
        bool cur_aux = s_state.isBackupHeaterOn();
        if (cur_aux != s_prev_aux_heater) {
            event_log_record(cur_aux ? EVENT_AUX_HEATER_ON : EVENT_AUX_HEATER_OFF, 0);
        }
        // Defrost
        bool cur_defrost = s_state.isDefrosting();
        if (cur_defrost != s_prev_defrosting) {
            event_log_record(cur_defrost ? EVENT_DEFROST_START : EVENT_DEFROST_END, 0);
        }
        // Error changes (check individual bits)
        uint16_t new_err1 = s_state.error1 & ~s_prev_error1;
        uint16_t clr_err1 = s_prev_error1 & ~s_state.error1;
        uint16_t new_err2 = s_state.error2 & ~s_prev_error2;
        uint16_t clr_err2 = s_prev_error2 & ~s_state.error2;
        for (int b = 0; b < 16; b++) {
            if (new_err1 & (1 << b)) event_log_record(EVENT_ERROR_APPEARED, (1 << 16) | (1 << b));
            if (clr_err1 & (1 << b)) event_log_record(EVENT_ERROR_CLEARED, (1 << 16) | (1 << b));
            if (new_err2 & (1 << b)) event_log_record(EVENT_ERROR_APPEARED, (2 << 16) | (1 << b));
            if (clr_err2 & (1 << b)) event_log_record(EVENT_ERROR_CLEARED, (2 << 16) | (1 << b));
        }
    }

    // Update previous state
    s_prev_unit_on = s_state.unit_on;
    s_prev_mode = s_state.working_mode;
    s_prev_cooling_sp = s_state.cooling_setpoint;
    s_prev_heating_sp = s_state.heating_setpoint;
    s_prev_hotwater_sp = s_state.hot_water_setpoint;
    s_prev_compressor = s_state.isCompressorRunning();
    s_prev_fan = s_state.isFanRunning();
    s_prev_pump = s_state.isWaterPumpRunning();
    s_prev_aux_heater = s_state.isBackupHeaterOn();
    s_prev_defrosting = s_state.isDefrosting();
    s_prev_error1 = s_state.error1;
    s_prev_error2 = s_state.error2;
    s_prev_state_valid = true;
}

// Main polling task
static void pollTask(void* param) {
    ESP_LOGI(TAG, "Polling task started");
    
    uint32_t poll_interval = POLL_INTERVAL_NORMAL_MS;
    
    while (s_polling_enabled) {
        uint32_t now = getTimeMs();
        
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.last_attempt_ms = now;
        xSemaphoreGive(s_state_mutex);
        
        // Try to poll all data
        bool success = true;
        
        // Poll in order, stop on first failure to avoid flooding a disconnected device
        if (success) success = pollStatus();          // Most important - status/errors
        if (success) success = pollTemperatures();    // Temperatures
        if (success) success = pollSystemReadings();  // Compressor, voltage, etc.
        if (success) success = pollHoldingRegisters();// Settings (less frequent would be fine)
        
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        
        if (success) {
            s_state.connected = true;
            s_state.last_successful_read_ms = now;
            s_state.consecutive_failures = 0;
            poll_interval = POLL_INTERVAL_NORMAL_MS;
            
            // Update error history tracking
            updateErrorHistory(s_state.error1, s_state.error2);
            
            // Log connection state change
            if (!s_was_connected) {
                ESP_LOGI(TAG, "Heat pump connected");
                s_was_connected = true;
                event_log_record(EVENT_CONNECTED, 0);
            }
            
            // ---- Event detection: compare current vs previous state ----
            detectAndLogStateEvents();
        } else {
            s_state.consecutive_failures++;
            
            if (s_state.consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                s_state.connected = false;
                poll_interval = POLL_INTERVAL_DISCONNECTED_MS;
                
                // Log disconnection once
                if (s_was_connected) {
                    ESP_LOGW(TAG, "Heat pump disconnected: %s", modbus::getLastError());
                    s_was_connected = false;
                    event_log_record(EVENT_DISCONNECTED, 0);
                }
            }
        }
        
        xSemaphoreGive(s_state_mutex);
        
        // Wait for next poll
        vTaskDelay(pdMS_TO_TICKS(poll_interval));
    }
    
    ESP_LOGI(TAG, "Polling task stopped");
    s_poll_task = nullptr;
    vTaskDelete(nullptr);
}

void init() {
    if (s_state_mutex == nullptr) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    
    // Reset state
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = HeatPumpState();  // Reset to defaults
    xSemaphoreGive(s_state_mutex);
    
    s_was_connected = false;
    
    ESP_LOGI(TAG, "Heat pump controller initialized");
}

void initDemoState() {
    if (s_state_mutex == nullptr) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    
    s_demo_mode = true;
    
    // Populate demo registers with realistic initial values
    memset(s_demo_regs, 0, sizeof(s_demo_regs));
    
    // Settings (2000-2007)
    s_demo_regs[reg::UNIT_ON_OFF - DEMO_REG_BASE]       = 1;  // ON
    s_demo_regs[reg::WORKING_MODE - DEMO_REG_BASE]      = static_cast<uint16_t>(WorkingMode::FLOOR_HEATING);
    s_demo_regs[reg::COOLING_SETPOINT - DEMO_REG_BASE]  = 18;
    s_demo_regs[reg::HEATING_SETPOINT - DEMO_REG_BASE]  = 45;
    s_demo_regs[reg::HOT_WATER_SETPOINT - DEMO_REG_BASE] = 50;
    
    // Temperatures (°C)
    s_demo_regs[reg::WATER_TANK_TEMP - DEMO_REG_BASE]    = 42;
    s_demo_regs[reg::OUTLET_WATER_TEMP - DEMO_REG_BASE]  = 45;
    s_demo_regs[reg::INLET_WATER_TEMP - DEMO_REG_BASE]   = 38;
    s_demo_regs[reg::DISCHARGE_TEMP - DEMO_REG_BASE]     = 85;
    s_demo_regs[reg::SUCTION_TEMP - DEMO_REG_BASE]       = 12;
    s_demo_regs[reg::OUTDOOR_COIL_TEMP - DEMO_REG_BASE]  = 35;
    s_demo_regs[reg::INDOOR_COIL_TEMP - DEMO_REG_BASE]   = 40;
    s_demo_regs[reg::OUTDOOR_AMBIENT_TEMP - DEMO_REG_BASE] = 22;
    s_demo_regs[reg::IPM_TEMP - DEMO_REG_BASE]           = 55;
    
    // System readings
    s_demo_regs[reg::COMPRESSOR_FREQ - DEMO_REG_BASE]      = 60;
    s_demo_regs[reg::FAN_SPEED - DEMO_REG_BASE]            = 850;
    s_demo_regs[reg::AC_VOLTAGE - DEMO_REG_BASE]           = 230;
    s_demo_regs[reg::AC_CURRENT - DEMO_REG_BASE]           = 52;    // tenths of amps → 5.2A
    s_demo_regs[reg::DC_VOLTAGE - DEMO_REG_BASE]           = 3800;  // ÷10 = 380V
    s_demo_regs[reg::DC_CURRENT - DEMO_REG_BASE]           = 3;
    s_demo_regs[reg::PRIMARY_EEV_OPENING - DEMO_REG_BASE]  = 320;
    s_demo_regs[reg::SECONDARY_EEV_OPENING - DEMO_REG_BASE] = 0;
    s_demo_regs[reg::HIGH_PRESSURE - DEMO_REG_BASE]        = 280;   // ÷100 = 2.80 MPa
    s_demo_regs[reg::LOW_PRESSURE - DEMO_REG_BASE]         = 85;    // ÷100 = 0.85 MPa
    
    // Status - compressor, fan medium, water pump running
    s_demo_regs[reg::STATUS_1 - DEMO_REG_BASE] = status1::UNIT_ON | status1::COMPRESSOR | status1::FAN_MED | status1::WATER_PUMP;
    s_demo_regs[reg::STATUS_2 - DEMO_REG_BASE] = 0;
    
    // Errors - P02 high pressure active
    s_demo_regs[reg::ERROR_1 - DEMO_REG_BASE] = 0;
    s_demo_regs[reg::ERROR_2 - DEMO_REG_BASE] = error2::HIGH_PRESSURE;
    
    // Sync demo registers into s_state via the poll functions
    pollStatus();
    pollTemperatures();
    pollSystemReadings();
    pollHoldingRegisters();
    
    // Mark as connected
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.connected = true;
    s_state.last_successful_read_ms = getTimeMs();
    xSemaphoreGive(s_state_mutex);
    
    // Seed error history with the current error state
    updateErrorHistory(s_state.error1, s_state.error2);
    
    // Also seed some cleared historical errors
    populateDemoErrorHistory();
    
    ESP_LOGI(TAG, "Demo state initialized");
}

bool isDemoMode() {
    return s_demo_mode;
}

// ============================================================================
// External Feed (passive Tuya listen mode)
// ============================================================================

void initExternalFeed() {
    if (s_state_mutex == nullptr) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    s_feed_mode = true;

    memset(s_demo_regs, 0, sizeof(s_demo_regs));

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = HeatPumpState();  // Reset to defaults (connected=false until first feed)
    xSemaphoreGive(s_state_mutex);

    s_was_connected = false;
    ESP_LOGI(TAG, "External feed mode initialized (passive Tuya)");
}

bool isExternalFeed() {
    return s_feed_mode;
}

// Empirically-derived Macon (OEM) Tuya register->field mapping (index = reg-2000).
// Confirmed against the unit's official o/A parameter-code legend cross-checked
// with live ground truth (idle + running pump->fan->compressor staged states):
//   TEMPERATURES (signed int8, whole °C):
//     reg2008 = o1 water tank
//     reg2132 = o3 water outlet/supply    (idle 28 -> running 40)
//     reg2133 = o2 water inlet/return     (idle 28 -> running 36)
//     reg2134 = o4 ambient/outdoor
//     reg2135 = A6 cool coil
//     reg2136 = A2 coil
//     reg2137 = A3 suction
//     reg2138 = A1 discharge
//     reg2113 = A8 IPM module
//   SETPOINT: reg2012 = hot-water setpoint
//   STATUS:   reg2007 run/fault bitfield (0x20 = hot-water ON; bits0-3 = ΔT/temp faults),
//             reg2130 icon bits #1 (0x01 heating, 0x04 compressor, 0x08 pump, 0x20 hours),
//             reg2129 icon bits #2 (0x02 defrost, 0x10 fan)
//   ELECTRICAL (register value == A-code menu value, 1:1):
//     reg2000 = A4 AC input current    reg2101 = A13 AC input voltage
//     reg2001 = A7 DC bus voltage(*10) reg2140 = A5 main EEV degree
//     reg2003 = A10 DC motor (fan) speed
//     reg2141 = A14 compressor frequency (Hz)   [telemetry window reaches 2142]
//   real-time power comes from the macon library (reg2114/A9), in watts.
// High/low pressure (A11/A12) read static nonsense values (-6 / 3), i.e.
// uninstalled sensors on this DHW unit, so left cleared. The fault/protection
// registers are reg2007 (holding) + the INPUT cluster reg2125-2128, all mapped
// live 2026-07-05; their bit ordering differs from the legacy Arctic error
// tables, so each confirmed bit is translated to its semantic legacy mask.
// Adapt the library's native MaconMode to the controller's legacy WorkingMode
// enum. NEVER cast: the raw wire values (0=heating, 4=cooling) differ from the
// legacy enum values (COOLING=0, FLOOR_HEATING=1). reg2049 only exposes the
// operating direction, so a confirmed heating direction is reported as
// FLOOR_HEATING (this unit's heating application). When the reg2049 reading is
// absent or untrusted, fall back to the generic HEATING label rather than
// falsely claiming FLOOR_HEATING.
static WorkingMode to_working_mode(MaconMode m) {
    switch (m) {
        case MaconMode::Cooling: return WorkingMode::COOLING;
        case MaconMode::Heating: return WorkingMode::FLOOR_HEATING;
        default:                 return WorkingMode::HEATING;
    }
}

static void applyMaconMapping() {
    // The arctic-macon library owns the register->field mapping: it knows which
    // wire register carries which field and how to interpret it. This function
    // now only (a) drives that decode over the fed register cache and (b) adapts
    // the native MaconState into the controller's legacy HeatPumpState (status
    // bitfields, WorkingMode enum, error masks).
    MaconState ms;
    decode_state(DEMO_REG_BASE, s_demo_regs, DEMO_REG_COUNT, &ms);

    // Synthesize the legacy status1 bitfield the HeatPumpState helpers expect
    // (isCompressorRunning()/isWaterPumpRunning()/isFanRunning()/getFanSpeedLevel()).
    uint16_t st1 = 0;
    if (ms.running)       st1 |= status1::UNIT_ON;
    if (ms.compressor_on) st1 |= status1::COMPRESSOR;
    if (ms.pump_on)       st1 |= status1::WATER_PUMP;
    if (ms.fan_on) {
        if (ms.fan_level >= 60)      st1 |= status1::FAN_HIGH;
        else if (ms.fan_level >= 30) st1 |= status1::FAN_MED;
        else                         st1 |= status1::FAN_LOW;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    // Temperatures (signed whole °C).
    s_state.water_tank_temp      = ms.water_tank_c;
    s_state.outlet_water_temp    = ms.outlet_c;
    s_state.inlet_water_temp     = ms.inlet_c;
    s_state.outdoor_ambient_temp = ms.outdoor_ambient_c;
    s_state.indoor_coil_temp     = ms.indoor_coil_c;
    s_state.ipm_temp             = ms.ipm_c;
    s_state.discharge_temp       = ms.discharge_c;
    s_state.suction_temp         = ms.suction_c;
    s_state.outdoor_coil_temp    = ms.outdoor_coil_c;

    // Setpoints (whole °C). cooling_setpoint = reg2093 byte0, decoded by the
    // macon library; previously left unmapped so the API reported 0.
    s_state.hot_water_setpoint   = ms.hot_water_setpoint;
    s_state.cooling_setpoint     = ms.cooling_setpoint;

    // Running state + readings.
    s_state.status1         = st1;
    s_state.status2         = ms.defrost_on ? status2::DEFROSTING : 0;
    s_state.unit_on         = ms.running;
    // Operating direction now derives from reg2049 via the library (previously a
    // static FLOOR_HEATING). Translate — never cast — the native mode.
    s_state.working_mode    = to_working_mode(ms.mode);
    s_state.fan_speed       = ms.fan_level;

    // Electrical readings.
    s_state.ac_current          = ms.ac_current;
    s_state.ac_voltage          = ms.ac_voltage;
    // The macon library owns raw->unit conversion and reports dc_voltage in
    // VOLTS. Store it as-is; no re-scaling in the consumer.
    s_state.dc_voltage          = ms.dc_voltage;
    s_state.primary_eev_opening = ms.primary_eev;
    s_state.compressor_freq     = ms.compressor_freq;
    // Real-time power in watts, decoded by the macon library (reg2114/A9).
    // Preferred over the old V*I/10 estimate, which is now 10x low because the
    // library normalises ac_current to whole amps.
    s_state.realtime_power_w    = ms.realtime_power_w;

    // Estimated performance (thermal output + COP). Water flow is NOT reported
    // by the mainboard (only a flow switch), so it is an outside estimate: 40
    // L/min of water matches the arctic-sniffer's assumption so both agree. The
    // macon library owns the physics; we only supply the estimated inputs.
    static constexpr arctic::PerformanceInputs kPerfInputs = {
        /*water_flow_lpm=*/40.0f, /*fluid_cp_j_per_kgK=*/4186.0f,
        /*fluid_density_kg_per_l=*/1.00f };
    const arctic::PerformanceEstimate perf = arctic::estimate_performance(ms, kPerfInputs);
    s_state.thermal_w = perf.thermal_w;
    s_state.cop_x100  = perf.cop_x100;
    s_state.cop_valid = perf.valid;


    // Macon fault/protection registers = reg2007 (holding run/fault bits) plus
    // the INPUT fault cluster reg2125-2128, all mapped live 2026-07-05 one bit at
    // a time against the OEM LCD + Smart Life app and cross-referenced to the
    // official Arctic fault catalog. The Macon bit ordering does NOT match the
    // legacy Arctic error1/error2 tables, so each confirmed Macon bit is
    // translated to its *semantic* legacy mask, letting the existing P-code/UI
    // pipeline report the correct fault. Device-only codes with no legacy slot
    // (P10, P30, E03) are intentionally left undecoded. The library
    // address-encapsulates the five raw fault registers (MaconState.fault_*);
    // the bit->mask translation below stays here (native fault decode = Phase 2).
    const uint8_t f_run  = ms.fault_run;   // reg2007
    const uint8_t f_ee   = ms.fault_ee;    // reg2125 sensor/EE/comm
    const uint8_t f_comp = ms.fault_comp;  // reg2126 sensor/comm/compressor
    const uint8_t f_elec = ms.fault_elec;  // reg2127 electrical/power-stage
    const uint8_t f_ref  = ms.fault_ref;   // reg2128 refrigerant/protection
    uint16_t e1 = 0;
    uint16_t e2 = 0;

    // reg2007 (holding) — differential/temp-diff faults (bit5=0x20 is the RUN
    // indicator, decoded above as `running`, and is NOT a fault).
    if (f_run & 0x01) e2 |= error2::WATER_TEMP_DIFF;   // P15 inlet/outlet ΔT large
    if (f_run & 0x02) e2 |= error2::LOW_OUTLET_TEMP;   // P16 outlet water temp low
    if (f_run & 0x04) e2 |= error2::COMP_PRESS_DIFF;   // FE start diff-pressure prot
    if (f_run & 0x08) e2 |= error2::COMP_PRESS_DIFF;   // FF run diff-pressure prot

    // reg2128 — refrigerant / P-codes.
    if (f_ref & 0x01) e2 |= error2::LOW_PRESSURE;      // P06 low pressure
    if (f_ref & 0x02) e2 |= error2::COOLING_HIGH_COIL; // P27 coil overheat
    if (f_ref & 0x04) e2 |= error2::LOW_AMBIENT_TEMP;  // PC ambient protection
    // bit3 (0x08) = P10 — device code, no legacy slot.
    // bit4 (0x10) = P30 antifreeze — no clean legacy slot.
    if (f_ref & 0x20) e1 |= error1::OUTDOOR_COIL_SENS; // E05 coil sensor
    if (f_ref & 0x80) e2 |= error2::WATER_FLOW;        // P01 water flow (confirmed live)

    // reg2127 — electrical / r-codes + P02/P11.
    if (f_elec & 0x02) e2 |= error2::AC_CURRENT_PROT;    // P19 AC current
    if (f_elec & 0x04) e2 |= error2::COMP_CURRENT_PROT;  // r06 comp phase current
    if (f_elec & 0x08) e1 |= error1::AC_VOLTAGE_PROT;    // r10 AC voltage
    if (f_elec & 0x10) e2 |= error2::BUS_VOLTAGE_PROT;   // r11 DC bus voltage
    if (f_elec & 0x20) e2 |= error2::IPM_HIGH_TEMP;      // r05 IPM temp
    if (f_elec & 0x40) e2 |= error2::HIGH_DISCHARGE_TEMP;// P11 high discharge temp
    if (f_elec & 0x80) e2 |= error2::HIGH_PRESSURE;      // P02 high pressure

    // reg2126 — sensor / comm / compressor.
    if (f_comp & 0x01) e1 |= error1::COMP_START;         // r02 compressor start
    if (f_comp & 0x02) e1 |= error1::INDOOR_OUTDOOR_COMM;// E26 in/out comm
    if (f_comp & 0x04) e1 |= error1::IPM_ERROR;          // r01 IPM
    if (f_comp & 0x10) e1 |= error1::DISCHARGE_SENS;     // E01 discharge sensor
    if (f_comp & 0x20) e1 |= error1::SUCTION_SENS;       // E09 suction sensor
    if (f_comp & 0x40) e1 |= error1::OUTDOOR_COIL_SENS;  // E05 coil sensor
    if (f_comp & 0x80) e1 |= error1::OUTDOOR_TEMP_SENS;  // E22 ambient sensor

    // reg2125 — sensor / EE / comm E-codes.
    if (f_ee & 0x01) e1 |= error1::OUTDOOR_EE;           // E28 outdoor EE
    if (f_ee & 0x02) e1 |= error1::INLET_TEMP_SENS;      // E19 inlet sensor
    if (f_ee & 0x04) e1 |= error1::OUTLET_TEMP_SENS;     // E18 outlet sensor
    if (f_ee & 0x08) e1 |= error1::INDOOR_COIL_SENS;     // E13 cool-coil sensor
    // bit4 (0x10) = E03 — device code, no legacy slot.
    if (f_ee & 0x20) e1 |= error1::INDOOR_EE;            // E28 indoor EE
    if (f_ee & 0x40) e1 |= error1::COMP_DRIVE;           // E27 driver comm
    if (f_ee & 0x80) e1 |= error1::WIRED_CTRL_COMM;      // E21 controller comm

    s_state.error1 = e1;
    s_state.error2 = e2;

    xSemaphoreGive(s_state_mutex);
}

uint16_t getRawRegisters(uint16_t* out, uint16_t max_count, uint16_t* base_out) {
    if (base_out) {
        *base_out = DEMO_REG_BASE;
    }
    if (out == nullptr || max_count == 0) {
        return 0;
    }
    uint16_t n = (max_count < DEMO_REG_COUNT) ? max_count : DEMO_REG_COUNT;
    for (uint16_t i = 0; i < n; ++i) {
        out[i] = s_demo_regs[i];
    }
    return n;
}

// ---------------------------------------------------------------------------
// Observed-window diagnostic catalog
// ---------------------------------------------------------------------------
static constexpr uint16_t OBS_WIN_MAX = 64;
static ObservedWindow s_obs_windows[OBS_WIN_MAX] = {};
static uint16_t       s_obs_window_count = 0;
static portMUX_TYPE   s_obs_mux = portMUX_INITIALIZER_UNLOCKED;

void recordObservedWindow(uint16_t field_a, uint16_t field_b, uint8_t known,
                          const uint8_t* payload, size_t len) {
    const uint32_t now = getTimeMs();
    const uint8_t cap = (uint8_t)sizeof(s_obs_windows[0].payload);
    const uint8_t n = (uint8_t)((len < cap) ? len : cap);

    portENTER_CRITICAL(&s_obs_mux);
    ObservedWindow* slot = nullptr;
    for (uint16_t i = 0; i < s_obs_window_count; ++i) {
        if (s_obs_windows[i].field_a == field_a &&
            s_obs_windows[i].field_b == field_b) {
            slot = &s_obs_windows[i];
            break;
        }
    }
    if (slot == nullptr && s_obs_window_count < OBS_WIN_MAX) {
        slot = &s_obs_windows[s_obs_window_count++];
        slot->field_a = field_a;
        slot->field_b = field_b;
        slot->hits    = 0;
    }
    if (slot != nullptr) {
        slot->known       = known;
        slot->hits       += 1;
        slot->last_ms     = now;
        slot->payload_len = n;
        for (uint8_t i = 0; i < n && payload != nullptr; ++i) {
            slot->payload[i] = payload[i];
        }
    }
    portEXIT_CRITICAL(&s_obs_mux);
}

uint16_t getObservedWindows(ObservedWindow* out, uint16_t max_count) {
    if (out == nullptr || max_count == 0) return 0;
    portENTER_CRITICAL(&s_obs_mux);
    uint16_t n = (s_obs_window_count < max_count) ? s_obs_window_count : max_count;
    for (uint16_t i = 0; i < n; ++i) {
        out[i] = s_obs_windows[i];
    }
    portEXIT_CRITICAL(&s_obs_mux);
    return n;
}

void feedRegisterWindow(uint16_t reg_base, const uint8_t* regs, size_t count) {
    if (!s_feed_mode || regs == nullptr || count == 0) {
        return;
    }

    // Copy the window's 1-byte registers into the register cache (bounds-checked).
    for (size_t i = 0; i < count; ++i) {
        int32_t idx = (int32_t)reg_base + (int32_t)i - (int32_t)DEMO_REG_BASE;
        if (idx >= 0 && idx < (int32_t)DEMO_REG_COUNT) {
            s_demo_regs[idx] = regs[i];
        }
    }

    // Map the cache into HeatPumpState using the empirically-derived ECO-600
    // Tuya layout (see applyMaconMapping). The Arctic/ECO-600 doc-based poll parsers do
    // NOT apply here: the real byte offsets differ and the doc's status/error
    // registers (2135-2138) are actually live temperature bytes on this unit.
    applyMaconMapping();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.connected = true;
    s_state.last_successful_read_ms = getTimeMs();
    s_state.last_attempt_ms = s_state.last_successful_read_ms;
    s_state.consecutive_failures = 0;
    // Log operational events (compressor/pump/fan/defrost/mode/setpoint/error
    // transitions) for the Tuya feed path, same as the Modbus poll loop.
    detectAndLogStateEvents();
    uint16_t err1 = s_state.error1;
    uint16_t err2 = s_state.error2;
    xSemaphoreGive(s_state_mutex);

    updateErrorHistory(err1, err2);

    if (!s_was_connected) {
        ESP_LOGI(TAG, "Heat pump connected (passive feed)");
        s_was_connected = true;
        event_log_record(EVENT_CONNECTED, 0);
    }
}

void startPolling() {
    if (s_poll_task != nullptr) {
        ESP_LOGW(TAG, "Polling already running");
        return;
    }
    
    if (!s_demo_mode && !modbus::isInitialized()) {
        ESP_LOGE(TAG, "Cannot start polling: Modbus not initialized");
        return;
    }
    
    s_polling_enabled = true;
    
    BaseType_t ret = xTaskCreate(
        pollTask,
        "arctic_poll",
        4096,
        nullptr,
        5,  // Priority
        &s_poll_task
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create polling task");
        s_polling_enabled = false;
    }
}

void stopPolling() {
    s_polling_enabled = false;
    
    // Wait for task to finish
    int timeout = 50;  // 500ms max
    while (s_poll_task != nullptr && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout--;
    }
}

HeatPumpState getState() {
    HeatPumpState copy;
    
    if (s_state_mutex != nullptr) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        copy = s_state;
        xSemaphoreGive(s_state_mutex);
    }
    
    return copy;
}

bool isConnected() {
    bool connected = false;
    
    if (s_state_mutex != nullptr) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        connected = s_state.connected;
        xSemaphoreGive(s_state_mutex);
    }
    
    return connected;
}

// ============================================================================
// Control Functions
// ============================================================================

bool setUnitPower(bool on) {
    if (macon_master::is_active()) {
        ESP_LOGW(TAG, "Unit power write unsupported in Tuya master mode (no verified fc06 mapping)");
        return false;
    }
    esp_err_t err = writeSingleReg(reg::UNIT_ON_OFF, on ? 1 : 0);
    if (err == ESP_OK) {
        // unit_on will update from STATUS_1 on next pollStatus() cycle
        ESP_LOGI(TAG, "Unit power set to %s", on ? "ON" : "OFF");
        return true;
    }
    ESP_LOGE(TAG, "Failed to set unit power: %s", modbus::getLastError());
    return false;
}

bool setWorkingMode(WorkingMode mode) {
    if (macon_master::is_active()) {
        ESP_LOGW(TAG, "Working-mode write unsupported in Tuya master mode (no verified fc06 mapping)");
        return false;
    }
    esp_err_t err = writeSingleReg(reg::WORKING_MODE, static_cast<uint16_t>(mode));
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.working_mode = mode;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Working mode set to %s", workingModeToString(mode));
        return true;
    }
    ESP_LOGE(TAG, "Failed to set working mode: %s", modbus::getLastError());
    return false;
}

bool setCoolingSetpoint(int16_t temp) {
    // Enforce the library-owned range (the mainboard enforces none of its own).
    temp = static_cast<int16_t>(clamp_setpoint(SetpointKind::Cooling, temp));
    if (macon_master::is_active()) {
        // Route through the shared-library MaconLink (fc06 write + ACK).
        if (macon_master::set_cooling_setpoint((int)temp)) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_state.cooling_setpoint = temp;
            xSemaphoreGive(s_state_mutex);
            return true;
        }
        return false;
    }
    esp_err_t err = writeSingleReg(reg::COOLING_SETPOINT, static_cast<uint16_t>(temp));
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.cooling_setpoint = temp;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Cooling setpoint set to %d", temp);
        return true;
    }
    ESP_LOGE(TAG, "Failed to set cooling setpoint: %s", modbus::getLastError());
    return false;
}

bool setHeatingSetpoint(int16_t temp) {
    temp = static_cast<int16_t>(clamp_setpoint(SetpointKind::Heating, temp));
    if (macon_master::is_active()) {
        // MaconLink deliberately has no set_heating_setpoint: reg2094 is
        // unverified on this unit. Fail explicitly rather than guess.
        ESP_LOGW(TAG, "Heating setpoint write unsupported in Tuya master mode (reg2094 unverified)");
        return false;
    }
    esp_err_t err = writeSingleReg(reg::HEATING_SETPOINT, static_cast<uint16_t>(temp));
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.heating_setpoint = temp;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Heating setpoint set to %d", temp);
        return true;
    }
    ESP_LOGE(TAG, "Failed to set heating setpoint: %s", modbus::getLastError());
    return false;
}

bool setHotWaterSetpoint(int16_t temp) {
    temp = static_cast<int16_t>(clamp_setpoint(SetpointKind::HotWater, temp));
    if (macon_master::is_active()) {
        if (macon_master::set_hot_water_setpoint((int)temp)) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_state.hot_water_setpoint = temp;
            xSemaphoreGive(s_state_mutex);
            return true;
        }
        return false;
    }
    esp_err_t err = writeSingleReg(reg::HOT_WATER_SETPOINT, static_cast<uint16_t>(temp));
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.hot_water_setpoint = temp;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Hot water setpoint set to %d", temp);
        return true;
    }
    ESP_LOGE(TAG, "Failed to set hot water setpoint: %s", modbus::getLastError());
    return false;
}

bool writeRegister(uint16_t address, uint16_t value) {
    if (macon_master::is_active()) {
        ESP_LOGW(TAG, "Raw register write unsupported in Tuya master mode (no verified fc06 mapping)");
        return false;
    }
    // Validate address is in writable range (2000-2057)
    if (address < 2000 || address > 2057) {
        ESP_LOGE(TAG, "Invalid register address %d (must be 2000-2057)", address);
        return false;
    }
    
    esp_err_t err = writeSingleReg(address, value);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Register %d set to %d", address, value);
        return true;
    }
    ESP_LOGE(TAG, "Failed to write register %d: %s", address, modbus::getLastError());
    return false;
}

bool readRegister(uint16_t address, uint16_t* value_out) {
    if (value_out == nullptr) {
        return false;
    }
    // In demo or passive external-feed mode the register value lives in the
    // cached window (s_demo_regs), which stays valid even when we are the
    // active Tuya master. Serve it from the cache rather than attempting a
    // synchronous bus transaction (which is unsupported in master mode) so the
    // Control-screen P-parameter and advanced (AP) rows can display values.
    if (s_demo_mode || s_feed_mode) {
        if (address < DEMO_REG_BASE ||
            (uint16_t)(address - DEMO_REG_BASE) >= DEMO_REG_COUNT) {
            return false;  // outside the cached register window
        }
        return readRegisters(address, 1, value_out) == ESP_OK;
    }
    if (macon_master::is_active()) {
        // Live values come from the poll loop into HeatPumpState; there is no
        // synchronous single-register read path on the Tuya bus.
        ESP_LOGW(TAG, "Synchronous register read unsupported in Tuya master mode");
        return false;
    }
    
    esp_err_t err = readRegisters(address, 1, value_out);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Register %d = %d", address, *value_out);
        return true;
    }
    ESP_LOGE(TAG, "Failed to read register %d: %s", address, modbus::getLastError());
    return false;
}

// ============================================================================
// Diagnostic Functions
// ============================================================================

int getErrorDescriptions(char* buffer, size_t buffer_size) {
    HeatPumpState state = getState();
    int error_count = 0;
    size_t offset = 0;
    
    #define CHECK_ERROR(reg, mask, desc) \
        if ((state.reg & mask) && offset < buffer_size - 1) { \
            int written = snprintf(buffer + offset, buffer_size - offset, "%s%s", \
                                   error_count > 0 ? ", " : "", desc); \
            if (written > 0) offset += written; \
            error_count++; \
        }
    
    // Error register 1 (2137)
    CHECK_ERROR(error1, error1::INDOOR_EE, "Indoor EEPROM");
    CHECK_ERROR(error1, error1::OUTDOOR_EE, "Outdoor EEPROM");
    CHECK_ERROR(error1, error1::INLET_TEMP_SENS, "Inlet temp sensor");
    CHECK_ERROR(error1, error1::OUTLET_TEMP_SENS, "Outlet temp sensor");
    CHECK_ERROR(error1, error1::INDOOR_COIL_SENS, "Indoor coil sensor");
    CHECK_ERROR(error1, error1::OUTDOOR_COIL_SENS, "Outdoor coil sensor");
    CHECK_ERROR(error1, error1::DISCHARGE_SENS, "Discharge sensor");
    CHECK_ERROR(error1, error1::SUCTION_SENS, "Suction sensor");
    CHECK_ERROR(error1, error1::OUTDOOR_TEMP_SENS, "Outdoor temp sensor");
    CHECK_ERROR(error1, error1::INDOOR_OUTDOOR_COMM, "Indoor/outdoor comm");
    CHECK_ERROR(error1, error1::WIRED_CTRL_COMM, "Controller comm");
    CHECK_ERROR(error1, error1::COMP_START, "Compressor start");
    CHECK_ERROR(error1, error1::COMP_DRIVE, "Compressor drive");
    CHECK_ERROR(error1, error1::IPM_ERROR, "IPM error");
    CHECK_ERROR(error1, error1::COMP_TOP_PROT, "Compressor overheat");
    CHECK_ERROR(error1, error1::AC_VOLTAGE_PROT, "AC voltage");
    
    // Error register 2 (2138)
    CHECK_ERROR(error2, error2::AC_CURRENT_PROT, "AC current");
    CHECK_ERROR(error2, error2::COMP_CURRENT_PROT, "Compressor current");
    CHECK_ERROR(error2, error2::FAN_MOTOR, "Fan motor");
    CHECK_ERROR(error2, error2::BUS_VOLTAGE_PROT, "Bus voltage");
    CHECK_ERROR(error2, error2::IPM_HIGH_TEMP, "IPM high temp");
    CHECK_ERROR(error2, error2::HIGH_DISCHARGE_TEMP, "High discharge temp");
    CHECK_ERROR(error2, error2::HIGH_PRESSURE, "High pressure");
    CHECK_ERROR(error2, error2::LOW_PRESSURE, "Low pressure");
    CHECK_ERROR(error2, error2::WATER_FLOW, "Water flow");
    CHECK_ERROR(error2, error2::COOLING_HIGH_COIL, "High coil temp");
    CHECK_ERROR(error2, error2::LOW_AMBIENT_TEMP, "Low ambient temp");
    CHECK_ERROR(error2, error2::EEV_LOW_PRESS, "EEV low pressure");
    CHECK_ERROR(error2, error2::EVI_LOW_PRESS, "EVI low pressure");
    CHECK_ERROR(error2, error2::WATER_TEMP_DIFF, "Water temp diff");
    CHECK_ERROR(error2, error2::LOW_OUTLET_TEMP, "Low outlet temp");
    CHECK_ERROR(error2, error2::COMP_PRESS_DIFF, "Compressor pressure");
    
    #undef CHECK_ERROR
    
    if (error_count == 0 && buffer_size > 0) {
        strncpy(buffer, "No errors", buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
    }
    
    return error_count;
}

void getStatusDescription(char* buffer, size_t buffer_size) {
    HeatPumpState state = getState();
    
    if (!state.connected) {
        snprintf(buffer, buffer_size, "Disconnected");
        return;
    }
    
    snprintf(buffer, buffer_size, "%s | %s | Comp:%s Pump:%s Fan:%d",
             state.unit_on ? "ON" : "OFF",
             workingModeToString(state.working_mode),
             state.isCompressorRunning() ? "Y" : "N",
             state.isWaterPumpRunning() ? "Y" : "N",
             state.getFanSpeedLevel());
}

void forcePoll() {
    // Could implement a flag to trigger immediate poll
    // For now, just log
    ESP_LOGI(TAG, "Force poll requested");
}

bool setDemoField(const char* field, int32_t value) {
    if (!s_demo_mode) return false;
    
    // Map field name to register address
    uint16_t addr = 0;
    
    // Temperatures
    if (strcmp(field, "water_tank_temp") == 0)            addr = reg::WATER_TANK_TEMP;
    else if (strcmp(field, "outlet_water_temp") == 0)     addr = reg::OUTLET_WATER_TEMP;
    else if (strcmp(field, "inlet_water_temp") == 0)      addr = reg::INLET_WATER_TEMP;
    else if (strcmp(field, "discharge_temp") == 0)        addr = reg::DISCHARGE_TEMP;
    else if (strcmp(field, "suction_temp") == 0)          addr = reg::SUCTION_TEMP;
    else if (strcmp(field, "outdoor_coil_temp") == 0)     addr = reg::OUTDOOR_COIL_TEMP;
    else if (strcmp(field, "indoor_coil_temp") == 0)      addr = reg::INDOOR_COIL_TEMP;
    else if (strcmp(field, "outdoor_ambient_temp") == 0)  addr = reg::OUTDOOR_AMBIENT_TEMP;
    else if (strcmp(field, "ipm_temp") == 0)              addr = reg::IPM_TEMP;
    // System readings
    else if (strcmp(field, "compressor_freq") == 0)       addr = reg::COMPRESSOR_FREQ;
    else if (strcmp(field, "fan_speed") == 0)             addr = reg::FAN_SPEED;
    else if (strcmp(field, "ac_voltage") == 0)            addr = reg::AC_VOLTAGE;
    else if (strcmp(field, "ac_current") == 0)            addr = reg::AC_CURRENT;
    else if (strcmp(field, "dc_voltage") == 0)            addr = reg::DC_VOLTAGE;
    else if (strcmp(field, "dc_current") == 0)            addr = reg::DC_CURRENT;
    else if (strcmp(field, "primary_eev_opening") == 0)   addr = reg::PRIMARY_EEV_OPENING;
    else if (strcmp(field, "secondary_eev_opening") == 0) addr = reg::SECONDARY_EEV_OPENING;
    else if (strcmp(field, "high_pressure") == 0)         addr = reg::HIGH_PRESSURE;
    else if (strcmp(field, "low_pressure") == 0)          addr = reg::LOW_PRESSURE;
    // Status/error registers
    else if (strcmp(field, "status1") == 0)               addr = reg::STATUS_1;
    else if (strcmp(field, "status2") == 0)               addr = reg::STATUS_2;
    else if (strcmp(field, "error1") == 0)                addr = reg::ERROR_1;
    else if (strcmp(field, "error2") == 0)                addr = reg::ERROR_2;
    // Settings
    else if (strcmp(field, "unit_on") == 0)               addr = reg::UNIT_ON_OFF;
    else if (strcmp(field, "working_mode") == 0)          addr = reg::WORKING_MODE;
    else if (strcmp(field, "cooling_setpoint") == 0)      addr = reg::COOLING_SETPOINT;
    else if (strcmp(field, "heating_setpoint") == 0)      addr = reg::HEATING_SETPOINT;
    else if (strcmp(field, "hot_water_setpoint") == 0)    addr = reg::HOT_WATER_SETPOINT;
    else return false;
    
    s_demo_regs[addr - DEMO_REG_BASE] = (uint16_t)value;
    
    // Mirror UNIT_ON_OFF to STATUS_1 bit 0 (same as writeSingleReg)
    if (addr == reg::UNIT_ON_OFF) {
        uint16_t& st1 = s_demo_regs[reg::STATUS_1 - DEMO_REG_BASE];
        if (value) st1 |= status1::UNIT_ON;
        else       st1 &= ~status1::UNIT_ON;
    }
    
    ESP_LOGI(TAG, "[DEMO] Field '%s' (reg %d) set to %ld", field, addr, (long)value);
    return true;
}

}  // namespace arctic

