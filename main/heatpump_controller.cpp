/*
 * Arctic Heat Pump State and Control
 */

#include "heatpump_controller.h"
#include "macon_master.h"
#include "heatpump_errors.h"
#include "event_log.h"
#include "macon_state.h"
#include "macon_registers.h"
#include "macon_faults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "arctic";

namespace arctic {

// Forward declaration: decode the real-Tuya register cache into HeatPumpState.
static void applyMaconMapping();

// State protected by mutex
static HeatPumpState s_state;
static SemaphoreHandle_t s_state_mutex = nullptr;
static TaskHandle_t s_demo_sync_task = nullptr;
static bool s_demo_sync_enabled = false;
static bool s_demo_mode = false;
static bool s_feed_mode = false;  // Passive Tuya external-feed mode
static uint32_t s_holding_window_ms = 0;
static uint32_t s_telemetry_window_ms = 0;
static bool s_inlet_valid = false;
static bool s_outlet_valid = false;
static bool s_cooling_setpoint_valid = false;
static bool s_heating_setpoint_valid = false;
static bool s_hot_water_setpoint_valid = false;
static bool s_mode_valid = false;
static bool s_compressor_valid = false;

static constexpr uint32_t TELEMETRY_FRESHNESS_MS = 90000;

// Connection state tracking
static bool s_was_connected = false;  // For logging state changes

// Previous state for event detection (compared each poll cycle)
static bool s_prev_unit_on = false;
static WorkingMode s_prev_mode = WorkingMode::COOLING;
static bool s_prev_compressor = false;
static bool s_prev_fan = false;
static bool s_prev_pump = false;
static bool s_prev_aux_heater = false;
static bool s_prev_defrosting = false;
static uint8_t s_prev_fault_bytes[5] = {0};  // reg 2007,2125,2126,2127,2128
static int16_t s_prev_cooling_sp = 0;
static int16_t s_prev_heating_sp = 0;
static int16_t s_prev_hotwater_sp = 0;
static bool s_prev_state_valid = false;  // False until first successful poll

// Get current time in milliseconds
static uint32_t getTimeMs() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool elapsed_within(uint32_t now, uint32_t then, uint32_t limit) {
    return then != 0 && (uint32_t)(now - then) <= limit;
}

// ============================================================================
// Register Cache
// ============================================================================
// The real Tuya wire registers are cached here (index = reg - DEMO_REG_BASE),
// exactly as the passive listener / active master / demo simulator populate
// them. The arctic-macon library (decode_state) owns interpreting this image
// into a MaconState; the controller only adapts that into HeatPumpState.
static const uint16_t DEMO_REG_BASE = HOLDING_START;  // 2000
// Spans both the holding (2000..2057) and telemetry (2093..2142) windows as a
// single flat cache; bounds come from the macon library, not magic numbers.
static const uint16_t DEMO_REG_COUNT = INPUT_START + INPUT_COUNT - HOLDING_START;  // 2000..2142
static uint16_t s_demo_regs[DEMO_REG_COUNT];

// Read a single register from the cache.
static esp_err_t readCacheReg(uint16_t address, uint16_t* out) {
    if (out == nullptr || address < DEMO_REG_BASE ||
        (uint16_t)(address - DEMO_REG_BASE) >= DEMO_REG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_demo_regs[address - DEMO_REG_BASE];
    return ESP_OK;
}

// Write a single register into the cache (real Tuya layout).
static esp_err_t writeCacheReg(uint16_t address, uint16_t value) {
    if (address < DEMO_REG_BASE ||
        (uint16_t)(address - DEMO_REG_BASE) >= DEMO_REG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_demo_regs[address - DEMO_REG_BASE] = value;
    return ESP_OK;
}


// Compare the freshly-updated s_state against the previous snapshot and record
// operational events (power/mode/setpoint/component/defrost/error transitions).
// The caller MUST hold s_state_mutex. Used by demo synchronization and the
// passive/active Tuya feed path.
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
        // Fault changes (per Macon fault register/bit; skip the RUN indicator).
        const uint8_t cur_faults[5] = {
            s_state.fault_run, s_state.fault_ee, s_state.fault_comp,
            s_state.fault_elec, s_state.fault_ref };
        static const uint16_t kFaultRegs[5] = {
            REG_FAULT_RUNSTATE, REG_FAULT_SENSOR_EE, REG_FAULT_SENSOR_COMP,
            REG_FAULT_ELEC, REG_FAULT };
        for (int r = 0; r < 5; r++) {
            uint8_t appeared = cur_faults[r] & ~s_prev_fault_bytes[r];
            uint8_t cleared  = s_prev_fault_bytes[r] & ~cur_faults[r];
            for (int b = 0; b < 8; b++) {
                const MaconFaultBit* fb = macon_fault_bit(kFaultRegs[r], b);
                if (fb == nullptr || fb->severity == FaultSeverity::INFO) continue;
                uint32_t payload = ((uint32_t)kFaultRegs[r] << 8) | (uint32_t)b;
                if (appeared & (1 << b)) event_log_record(EVENT_ERROR_APPEARED, payload);
                if (cleared & (1 << b))  event_log_record(EVENT_ERROR_CLEARED, payload);
            }
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
    s_prev_fault_bytes[0] = s_state.fault_run;
    s_prev_fault_bytes[1] = s_state.fault_ee;
    s_prev_fault_bytes[2] = s_state.fault_comp;
    s_prev_fault_bytes[3] = s_state.fault_elec;
    s_prev_fault_bytes[4] = s_state.fault_ref;
    s_prev_state_valid = true;
}

// Periodically decode the register cache into HeatPumpState and emit events.
static void demoSyncTask(void*) {
    ESP_LOGI(TAG, "Demo synchronization task started");

    while (s_demo_sync_enabled) {
        uint32_t now = getTimeMs();

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.last_attempt_ms = now;
        xSemaphoreGive(s_state_mutex);

        // Decode the real-Tuya register cache into HeatPumpState (locks internally).
        applyMaconMapping();

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.connected = true;
        s_state.last_successful_read_ms = now;
        s_state.consecutive_failures = 0;

        if (!s_was_connected) {
            ESP_LOGI(TAG, "Demo heat pump connected");
            s_was_connected = true;
            event_log_record(EVENT_CONNECTED, 0);
        }

        detectAndLogStateEvents();

        const uint8_t f_run  = s_state.fault_run;
        const uint8_t f_ee   = s_state.fault_ee;
        const uint8_t f_comp = s_state.fault_comp;
        const uint8_t f_elec = s_state.fault_elec;
        const uint8_t f_ref  = s_state.fault_ref;
        xSemaphoreGive(s_state_mutex);

        updateErrorHistory(f_run, f_ee, f_comp, f_elec, f_ref);

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "Demo synchronization task stopped");
    s_demo_sync_task = nullptr;
    vTaskDelete(nullptr);
}

void initDemoState() {
    if (s_state_mutex == nullptr) {
        s_state_mutex = xSemaphoreCreateMutex();
    }

    s_demo_mode = true;

    // Seed the register cache with a realistic RUNNING image in the REAL Tuya
    // wire layout (index = reg - DEMO_REG_BASE). The arctic-macon library
    // (decode_state) interprets these exactly as it does live device registers.
    memset(s_demo_regs, 0, sizeof(s_demo_regs));
    auto seed = [&](uint16_t r, uint16_t v) { s_demo_regs[r - DEMO_REG_BASE] = v; };

    // Run-state / mode.
    seed(REG_FAULT_RUNSTATE, 0x20);   // reg2007 bit5 = running
    seed(REG_OPERATING_MODE, 0x00);   // reg2049 = heating (reversing valve)
    seed(REG_WORKING_MODE, static_cast<uint16_t>(MaconWorkingMode::FloorHeating)); // reg2096 = 1
    // Icon bits: compressor + pump (reg2130 bit2/bit3), fan (reg2129 bit4).
    seed(REG_STATUS_BYTE, 0x04 | 0x08);
    seed(REG_ICON_BITS2, 0x10);

    // Setpoints (whole °C).
    seed(REG_COOLING_SETPOINT, 18);   // reg2093
    seed(REG_AUX_HEAT_SETPOINT, 45);  // reg2094 (aux/heating, demo only)
    seed(REG_HOT_WATER_SETPOINT, 50); // reg2095
    seed(REG_HOT_WATER_CEILING, 50);  // reg2012 AP13 ceiling

    // Temperatures (signed whole °C).
    seed(REG_WATER_TANK_TEMP, 42);       // reg2008 o1
    seed(REG_OUTLET_WATER_TEMP, 45);     // reg2132 o3
    seed(REG_INLET_WATER_TEMP, 38);      // reg2133 o2
    seed(REG_OUTDOOR_AMBIENT_TEMP, 22);  // reg2134 o4
    seed(REG_COOL_COIL_TEMP, 40);        // reg2135 A6 (indoor coil)
    seed(REG_COIL_TEMP, 35);             // reg2136 A2 (outdoor coil)
    seed(REG_SUCTION_TEMP, 12);          // reg2137 A3
    seed(REG_DISCHARGE_TEMP, 85);        // reg2138 A1
    seed(REG_IPM_TEMP, 55);              // reg2113 A8

    // Electrical / system readings (raw register values; decode owns scaling).
    seed(REG_AC_CURRENT, 5);        // reg2000 A4 (whole A)
    seed(REG_AC_VOLTAGE, 23);       // reg2101 A13 (x10 => 230 V)
    seed(REG_DC_BUS_VOLTAGE, 38);   // reg2001 A7 (x10 => 380 V)
    seed(REG_DC_MOTOR_SPEED, 40);   // reg2003 A10 fan level
    seed(REG_MAIN_EEV, 200);        // reg2104 A5 EEV steps
    seed(REG_COMPRESSOR_FREQ, 60);  // reg2141 A14 Hz
    seed(REG_REALTIME_POWER, 12);   // reg2114 A9 (x100 => 1200 W)

    // Seed one active fault — P02 high pressure — via the library's canonical
    // (reg,bit) encoder so no bit position is hardcoded here.
    macon_set_fault_by_code(s_demo_regs, DEMO_REG_BASE, DEMO_REG_COUNT, "P02", true);

    // Decode the seeded cache into HeatPumpState and mark connected.
    applyMaconMapping();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.connected = true;
    s_state.last_successful_read_ms = getTimeMs();
    const uint8_t f_run  = s_state.fault_run;
    const uint8_t f_ee   = s_state.fault_ee;
    const uint8_t f_comp = s_state.fault_comp;
    const uint8_t f_elec = s_state.fault_elec;
    const uint8_t f_ref  = s_state.fault_ref;
    xSemaphoreGive(s_state_mutex);

    // Seed error history with the current fault state.
    updateErrorHistory(f_run, f_ee, f_comp, f_elec, f_ref);

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
    ESP_LOGI(TAG, "Macon register feed initialized");
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
// Adapt the confirmed reg2096 working-mode enum. In Auto, expose the actual
// water-side direction while the compressor runs.
static WorkingMode to_working_mode(const MaconState& state) {
    switch (state.working_mode) {
        case MaconWorkingMode::Cooling:
            return WorkingMode::COOLING;
        case MaconWorkingMode::FloorHeating:
            return WorkingMode::FLOOR_HEATING;
        case MaconWorkingMode::FanCoilHeating:
            return WorkingMode::FAN_COIL_HEATING;
        case MaconWorkingMode::HotWater:
            return WorkingMode::HOT_WATER;
        case MaconWorkingMode::Auto:
            return WorkingMode::AUTO;
        default:
            return WorkingMode::AUTO;
    }
}

static HeatPumpOperation to_operation(const MaconState& state) {
    switch (decode_operation(state)) {
        case MaconOperation::Off:      return HeatPumpOperation::OFF;
        case MaconOperation::Idle:     return HeatPumpOperation::IDLE;
        case MaconOperation::Heating:  return HeatPumpOperation::HEATING;
        case MaconOperation::Cooling:  return HeatPumpOperation::COOLING;
        case MaconOperation::Defrost:  return HeatPumpOperation::DEFROST;
        case MaconOperation::Fault:    return HeatPumpOperation::FAULT;
        default:                       return HeatPumpOperation::UNKNOWN;
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
    if (ms.aux_heat_setpoint_valid) {
        s_state.heating_setpoint = ms.aux_heat_setpoint;
    }

    s_inlet_valid = ms.inlet_valid;
    s_outlet_valid = ms.outlet_valid;
    s_cooling_setpoint_valid = ms.cooling_setpoint_valid;
    // reg2094 is still not confirmed as the active heating target, so expose
    // its value in the diagnostic UI but do not persist it as a valid target.
    s_heating_setpoint_valid = false;
    s_hot_water_setpoint_valid = ms.hot_water_setpoint_valid;
    s_mode_valid = ms.working_mode_valid;
    s_compressor_valid = ms.compressor_freq_valid;

    // Component run-state (derived by the macon library from the native
    // MaconState) + readings. No fictional status bitfields any more.
    s_state.compressor_running      = ms.compressor_freq_valid && ms.compressor_freq > 0;
    s_state.pump_running            = ms.pump_on;
    s_state.fan_running             = ms.fan_on;
    s_state.defrosting              = ms.defrost_on;
    s_state.backup_heater           = false;  // no confirmed Macon register
    s_state.reversing_valve_cooling = ms.cooling_on;
    s_state.unit_on         = ms.running;
    s_state.working_mode    = to_working_mode(ms);
    s_state.operation       = to_operation(ms);
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


    // Raw Macon fault-register bytes, stored exactly as the mainboard reports
    // them. Decoding into P/E codes is done natively by macon_decode_faults()
    // (see heatpump_errors.cpp) — no fictional error1/error2 mask translation.
    s_state.fault_run  = ms.fault_run;
    s_state.fault_ee   = ms.fault_ee;
    s_state.fault_comp = ms.fault_comp;
    s_state.fault_elec = ms.fault_elec;
    s_state.fault_ref  = ms.fault_ref;
    s_state.any_fault  = macon_has_fault(ms.fault_run, ms.fault_ee, ms.fault_comp,
                                         ms.fault_elec, ms.fault_ref);

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

    const uint32_t now = getTimeMs();
    const uint32_t window_end = (uint32_t)reg_base + (uint32_t)count;
    const bool has_holding_state = reg_base <= REG_OPERATING_MODE && window_end > REG_OPERATING_MODE;
    const bool has_telemetry_state = reg_base <= REG_COMPRESSOR_FREQ && window_end > REG_COMPRESSOR_FREQ;

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
    if (has_holding_state) s_holding_window_ms = now;
    if (has_telemetry_state) s_telemetry_window_ms = now;
    s_state.connected = true;
    s_state.last_successful_read_ms = now;
    s_state.last_attempt_ms = s_state.last_successful_read_ms;
    s_state.consecutive_failures = 0;
    // Log operational transitions for the Tuya feed path.
    detectAndLogStateEvents();
    const uint8_t f_run  = s_state.fault_run;
    const uint8_t f_ee   = s_state.fault_ee;
    const uint8_t f_comp = s_state.fault_comp;
    const uint8_t f_elec = s_state.fault_elec;
    const uint8_t f_ref  = s_state.fault_ref;
    xSemaphoreGive(s_state_mutex);

    updateErrorHistory(f_run, f_ee, f_comp, f_elec, f_ref);

    if (!s_was_connected) {
        ESP_LOGI(TAG, "Heat pump connected (passive feed)");
        s_was_connected = true;
        event_log_record(EVENT_CONNECTED, 0);
    }
}

TelemetrySnapshot getTelemetrySnapshot() {
        TelemetrySnapshot snapshot;
        const uint32_t now = getTimeMs();

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_demo_mode) {
            snapshot.connected = s_state.connected;
            snapshot.inlet_valid = snapshot.connected;
            snapshot.outlet_valid = snapshot.connected;
            snapshot.compressor_valid = snapshot.connected;
            snapshot.compressor_running = s_state.isCompressorRunning();
            snapshot.inlet_c = s_state.inlet_water_temp;
            snapshot.outlet_c = s_state.outlet_water_temp;
            if (s_state.operation == HeatPumpOperation::COOLING) {
                snapshot.operation = TelemetryOperation::COOLING;
            } else if (s_state.operation == HeatPumpOperation::HEATING) {
                snapshot.operation = TelemetryOperation::HEATING;
            }
            if (s_state.working_mode == WorkingMode::COOLING) {
                snapshot.active_setpoint_c = s_state.cooling_setpoint;
                snapshot.setpoint_valid = snapshot.connected;
            } else if (s_state.working_mode == WorkingMode::AUTO &&
                       s_state.operation == HeatPumpOperation::COOLING) {
                snapshot.active_setpoint_c = s_state.cooling_setpoint;
                snapshot.setpoint_valid = snapshot.connected;
            } else if (s_state.working_mode == WorkingMode::HOT_WATER) {
                snapshot.active_setpoint_c = s_state.hot_water_setpoint;
                snapshot.setpoint_valid = snapshot.connected;
            } else if (s_state.working_mode == WorkingMode::FLOOR_HEATING ||
                       s_state.working_mode == WorkingMode::FAN_COIL_HEATING ||
                       s_state.working_mode == WorkingMode::HEATING) {
                snapshot.active_setpoint_c = s_state.heating_setpoint;
                snapshot.setpoint_valid = snapshot.connected;
            }
            xSemaphoreGive(s_state_mutex);
            return snapshot;
        }

        const bool telemetry_fresh =
            elapsed_within(now, s_telemetry_window_ms, TELEMETRY_FRESHNESS_MS);
        snapshot.connected = telemetry_fresh;
        snapshot.inlet_valid = telemetry_fresh && s_inlet_valid &&
            !hasActiveFaultCode(s_state.fault_run, s_state.fault_ee,
                                s_state.fault_comp, s_state.fault_elec,
                                s_state.fault_ref, "E19") &&  // inlet-water sensor
            s_state.inlet_water_temp >= -50 && s_state.inlet_water_temp <= 150;
        snapshot.outlet_valid = telemetry_fresh && s_outlet_valid &&
            !hasActiveFaultCode(s_state.fault_run, s_state.fault_ee,
                                s_state.fault_comp, s_state.fault_elec,
                                s_state.fault_ref, "E18") &&  // outlet-water sensor
            s_state.outlet_water_temp >= -50 && s_state.outlet_water_temp <= 150;
        snapshot.compressor_valid = telemetry_fresh && s_compressor_valid;
        snapshot.compressor_running =
            snapshot.compressor_valid && s_state.compressor_freq > 0;
        snapshot.inlet_c = s_state.inlet_water_temp;
        snapshot.outlet_c = s_state.outlet_water_temp;

        if (telemetry_fresh) {
            if (s_state.operation == HeatPumpOperation::COOLING) {
                snapshot.operation = TelemetryOperation::COOLING;
            } else if (s_state.operation == HeatPumpOperation::HEATING) {
                snapshot.operation = TelemetryOperation::HEATING;
            }
        }

        if (telemetry_fresh && s_mode_valid) {
            switch (s_state.working_mode) {
                case WorkingMode::COOLING:
                    snapshot.active_setpoint_c = s_state.cooling_setpoint;
                    snapshot.setpoint_valid =
                        telemetry_fresh && s_cooling_setpoint_valid;
                    break;
                case WorkingMode::AUTO:
                    if (s_state.operation == HeatPumpOperation::COOLING) {
                        snapshot.active_setpoint_c = s_state.cooling_setpoint;
                        snapshot.setpoint_valid =
                            telemetry_fresh && s_cooling_setpoint_valid;
                    }
                    break;
                case WorkingMode::HOT_WATER:
                    snapshot.active_setpoint_c = s_state.hot_water_setpoint;
                    snapshot.setpoint_valid =
                        telemetry_fresh && s_hot_water_setpoint_valid;
                    break;
                case WorkingMode::FLOOR_HEATING:
                case WorkingMode::FAN_COIL_HEATING:
                case WorkingMode::HEATING:
                    snapshot.active_setpoint_c = s_state.heating_setpoint;
                    snapshot.setpoint_valid =
                        telemetry_fresh && s_heating_setpoint_valid;
                    break;
                default:
                    break;
            }
        }
        xSemaphoreGive(s_state_mutex);
        return snapshot;
}

void startDemoSync() {
    if (!s_demo_mode) {
        ESP_LOGE(TAG, "Cannot start demo synchronization outside demo mode");
        return;
    }
    if (s_demo_sync_task != nullptr) {
        ESP_LOGW(TAG, "Demo synchronization already running");
        return;
    }

    s_demo_sync_enabled = true;

    BaseType_t ret = xTaskCreate(
        demoSyncTask,
        "arctic_demo_sync",
        4096,
        nullptr,
        5,  // Priority
        &s_demo_sync_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create demo synchronization task");
        s_demo_sync_enabled = false;
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
    // Toggle only the run-state bit (reg2007 bit5); other bits carry faults.
    uint16_t rs = 0;
    readCacheReg(REG_FAULT_RUNSTATE, &rs);
    if (on) rs |= 0x20; else rs &= ~0x20;
    esp_err_t err = writeCacheReg(REG_FAULT_RUNSTATE, rs);
    if (err == ESP_OK) {
        applyMaconMapping();
        ESP_LOGI(TAG, "Unit power set to %s", on ? "ON" : "OFF");
        return true;
    }
    ESP_LOGW(TAG, "Unit power write rejected outside demo mode");
    return false;
}

bool setWorkingMode(WorkingMode mode) {
    if (macon_master::is_active()) {
        ESP_LOGW(TAG, "Working-mode write unsupported in Tuya master mode (no verified fc06 mapping)");
        return false;
    }
    esp_err_t err = writeCacheReg(REG_WORKING_MODE, static_cast<uint16_t>(mode));
    if (err == ESP_OK) {
        applyMaconMapping();
        ESP_LOGI(TAG, "Working mode set to %s", workingModeToString(mode));
        return true;
    }
    ESP_LOGW(TAG, "Working-mode write rejected outside demo mode");
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
    esp_err_t err = writeCacheReg(REG_COOLING_SETPOINT, static_cast<uint16_t>(temp));
    if (err == ESP_OK) {
        applyMaconMapping();
        ESP_LOGI(TAG, "Cooling setpoint set to %d", temp);
        return true;
    }
    ESP_LOGW(TAG, "Cooling setpoint write rejected in passive-listen mode");
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
    esp_err_t err = writeCacheReg(REG_AUX_HEAT_SETPOINT, static_cast<uint16_t>(temp));
    if (err == ESP_OK) {
        applyMaconMapping();
        ESP_LOGI(TAG, "Heating setpoint set to %d", temp);
        return true;
    }
    ESP_LOGW(TAG, "Heating setpoint write rejected outside demo mode");
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
    esp_err_t err = writeCacheReg(REG_HOT_WATER_SETPOINT, static_cast<uint16_t>(temp));
    if (err == ESP_OK) {
        applyMaconMapping();
        ESP_LOGI(TAG, "Hot water setpoint set to %d", temp);
        return true;
    }
    ESP_LOGW(TAG, "Hot-water setpoint write rejected in passive-listen mode");
    return false;
}

bool writeRegister(uint16_t address, uint16_t value) {
    const bool in_holding_window =
        address >= HOLDING_START && address < HOLDING_START + HOLDING_COUNT;
    const bool in_telemetry_window =
        address >= INPUT_START && address < INPUT_START + INPUT_COUNT;
    if (!in_holding_window && !in_telemetry_window) {
        ESP_LOGE(TAG, "Register %u is outside known Macon windows",
                 (unsigned)address);
        return false;
    }
    if (value > UINT8_MAX) {
        ESP_LOGE(TAG, "Invalid register value %u (Macon registers are one byte)",
                 (unsigned)value);
        return false;
    }
    if (macon_master::is_active()) {
        return macon_master::write_register(address, static_cast<uint8_t>(value));
    }
    if (writeCacheReg(address, value) == ESP_OK) {
        applyMaconMapping();
        ESP_LOGI(TAG, "Register %d set to %d", address, value);
        return true;
    }
    ESP_LOGW(TAG, "Register write rejected in passive-listen mode");
    return false;
}

bool readRegister(uint16_t address, uint16_t* value_out) {
    if (value_out == nullptr) {
        return false;
    }
    // Demo, passive-listen, and active-master polling all populate this cache.
    if (s_demo_mode || s_feed_mode) {
        return readCacheReg(address, value_out) == ESP_OK;
    }
    ESP_LOGW(TAG, "Register cache is not initialized");
    return false;
}

// ============================================================================
// Diagnostic Functions
// ============================================================================

int getErrorDescriptions(char* buffer, size_t buffer_size) {
    HeatPumpState state = getState();
    int error_count = 0;
    size_t offset = 0;

    // Iterate the natively-decoded active faults (macon library owns the
    // canonical (reg,bit) -> code/label table); no fictional error1/error2 masks.
    arctic::ActiveError active[32];
    int n = arctic::getActiveErrors(active, 32);
    for (int i = 0; i < n && offset < buffer_size - 1; ++i) {
        const char* label = active[i].name ? active[i].name : active[i].code;
        int written = snprintf(buffer + offset, buffer_size - offset, "%s%s",
                               error_count > 0 ? ", " : "", label);
        if (written > 0) offset += written;
        error_count++;
    }

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
             heatPumpOperationToString(state.operation),
             state.isCompressorRunning() ? "Y" : "N",
             state.isWaterPumpRunning() ? "Y" : "N",
             state.getFanSpeedLevel());
}

bool setDemoField(const char* field, int32_t value) {
    if (!s_demo_mode) return false;
    
    // Map the demo field name to its REAL Tuya register address (index =
    // reg - DEMO_REG_BASE). Fault injection is done by writing the raw fault
    // register bytes directly (fault_run/ee/comp/elec/ref) — the macon library
    // decodes them into P/E codes. There are no fictional status/error regs.
    uint16_t addr = 0;
    // For bit-level (read-modify-write) fields we set rmw_reg/rmw_mask instead
    // of writing a whole register, so unrelated bits in shared icon/run
    // registers are preserved.
    uint16_t rmw_reg = 0;
    uint16_t rmw_mask = 0;

    // Temperatures
    if (strcmp(field, "water_tank_temp") == 0)            addr = REG_WATER_TANK_TEMP;
    else if (strcmp(field, "outlet_water_temp") == 0)     addr = REG_OUTLET_WATER_TEMP;
    else if (strcmp(field, "inlet_water_temp") == 0)      addr = REG_INLET_WATER_TEMP;
    else if (strcmp(field, "discharge_temp") == 0)        addr = REG_DISCHARGE_TEMP;
    else if (strcmp(field, "suction_temp") == 0)          addr = REG_SUCTION_TEMP;
    else if (strcmp(field, "outdoor_coil_temp") == 0)     addr = REG_COIL_TEMP;
    else if (strcmp(field, "indoor_coil_temp") == 0)      addr = REG_COOL_COIL_TEMP;
    else if (strcmp(field, "outdoor_ambient_temp") == 0)  addr = REG_OUTDOOR_AMBIENT_TEMP;
    else if (strcmp(field, "ipm_temp") == 0)              addr = REG_IPM_TEMP;
    // System readings
    else if (strcmp(field, "compressor_freq") == 0)       addr = REG_COMPRESSOR_FREQ;
    else if (strcmp(field, "fan_speed") == 0)             addr = REG_DC_MOTOR_SPEED;
    else if (strcmp(field, "ac_voltage") == 0)            addr = REG_AC_VOLTAGE;
    else if (strcmp(field, "ac_current") == 0)            addr = REG_AC_CURRENT;
    else if (strcmp(field, "dc_voltage") == 0)            addr = REG_DC_BUS_VOLTAGE;
    else if (strcmp(field, "main_eev") == 0 ||
             strcmp(field, "primary_eev_opening") == 0)   addr = REG_MAIN_EEV;
    else if (strcmp(field, "realtime_power") == 0)        addr = REG_REALTIME_POWER;
    // Raw fault-register bytes (fault injection). Decoded natively.
    else if (strcmp(field, "fault_run") == 0)             addr = REG_FAULT_RUNSTATE;
    else if (strcmp(field, "fault_ee") == 0)              addr = REG_FAULT_SENSOR_EE;
    else if (strcmp(field, "fault_comp") == 0)            addr = REG_FAULT_SENSOR_COMP;
    else if (strcmp(field, "fault_elec") == 0)            addr = REG_FAULT_ELEC;
    else if (strcmp(field, "fault_ref") == 0)             addr = REG_FAULT;
    // Component run-state (bit-level, decoded live). Compressor state is driven
    // by compressor_freq (reg2141), not an icon bit.
    else if (strcmp(field, "fan_on") == 0)                { rmw_reg = REG_ICON_BITS2; rmw_mask = 0x10; }  // bit4
    else if (strcmp(field, "cooling_on") == 0)            { rmw_reg = REG_ICON_BITS2; rmw_mask = 0x04; }  // bit2 (reversing valve = cooling)
    else if (strcmp(field, "pump_on") == 0)               { rmw_reg = REG_STATUS_BYTE; rmw_mask = 0x08; } // bit3
    // Settings
    else if (strcmp(field, "unit_on") == 0)               { rmw_reg = REG_FAULT_RUNSTATE; rmw_mask = 0x20; } // bit5
    else if (strcmp(field, "working_mode") == 0)          addr = REG_WORKING_MODE;
    else if (strcmp(field, "cooling_setpoint") == 0)      addr = REG_COOLING_SETPOINT;
    else if (strcmp(field, "heating_setpoint") == 0)      addr = REG_AUX_HEAT_SETPOINT;
    else if (strcmp(field, "hot_water_setpoint") == 0)    addr = REG_HOT_WATER_SETPOINT;
    else return false;

    if (rmw_mask != 0) {
        // Read-modify-write a single bit, preserving the rest of the register.
        uint16_t& r = s_demo_regs[rmw_reg - DEMO_REG_BASE];
        if (value) r |= rmw_mask; else r &= ~rmw_mask;
        addr = rmw_reg;  // for the log line below
    } else {
        s_demo_regs[addr - DEMO_REG_BASE] = (uint16_t)value;
    }

    // Re-decode the cache into HeatPumpState so the change is reflected live.
    applyMaconMapping();

    ESP_LOGI(TAG, "[DEMO] Field '%s' (reg %d) set to %ld", field, addr, (long)value);
    return true;
}

int injectDemoFault(const char* code, bool active) {
    if (!s_demo_mode || code == nullptr) return 0;
    int sites = macon_set_fault_by_code(s_demo_regs, DEMO_REG_BASE,
                                        DEMO_REG_COUNT, code, active);
    if (sites > 0) {
        applyMaconMapping();
        ESP_LOGI(TAG, "[DEMO] Fault '%s' %s (%d site%s)",
                 code, active ? "set" : "cleared", sites, sites == 1 ? "" : "s");
    }
    return sites;
}

void clearDemoFaults() {
    if (!s_demo_mode) return;
    // Clear the four telemetry fault registers entirely; on the run/state
    // register keep bit5 (RUN indicator) and clear only the fault bits.
    s_demo_regs[REG_FAULT_RUNSTATE - DEMO_REG_BASE]   &= 0x20;
    s_demo_regs[REG_FAULT_SENSOR_EE - DEMO_REG_BASE]   = 0;
    s_demo_regs[REG_FAULT_SENSOR_COMP - DEMO_REG_BASE] = 0;
    s_demo_regs[REG_FAULT_ELEC - DEMO_REG_BASE]        = 0;
    s_demo_regs[REG_FAULT - DEMO_REG_BASE]             = 0;
    applyMaconMapping();
    ESP_LOGI(TAG, "[DEMO] All faults cleared");
}

}  // namespace arctic
