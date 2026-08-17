/*
 * Arctic Heat Pump State and Control
 */

#include "heatpump_controller.h"
#include "macon_master_iface.h"
#include "heatpump_errors.h"
#include "event_log.h"
#include "macon_state.h"
#include "macon_image.h"
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
// Register Image
// ============================================================================
// The heat pump's register state is held OPAQUELY in a MaconImage. The
// controller never references a register address, bit position, or scaling
// factor: the arctic-macon library owns all of that. The image is mutated
// through semantic operations (set_flag/set_value/set_fault/ingest/…) under
// s_image_mutex; applyMaconMapping() copies it under that lock, releases it,
// then decodes the copy — so the image lock and the state lock are never held
// at the same time.
static MaconImage s_image;
static SemaphoreHandle_t s_image_mutex = nullptr;

static void ensureImageMutex() {
    if (s_image_mutex == nullptr) {
        s_image_mutex = xSemaphoreCreateMutex();
    }
}

static inline void imageLock()   { if (s_image_mutex) xSemaphoreTake(s_image_mutex, portMAX_DELAY); }
static inline void imageUnlock() { if (s_image_mutex) xSemaphoreGive(s_image_mutex); }


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
        // Fault changes: decode the previous and current fault bytes into
        // semantic sites (the library skips the RUN indicator) and diff by
        // opaque site id. The event-log payload is that site id — no register
        // or bit position is handled here.
        MaconFault prev_f[arctic::MAX_ACTIVE_FAULTS];
        MaconFault cur_f[arctic::MAX_ACTIVE_FAULTS];
        const size_t np = macon_decode_faults(
            s_prev_fault_bytes[0], s_prev_fault_bytes[1], s_prev_fault_bytes[2],
            s_prev_fault_bytes[3], s_prev_fault_bytes[4], prev_f, arctic::MAX_ACTIVE_FAULTS);
        const size_t nc = macon_decode_faults(
            s_state.fault_run, s_state.fault_ee, s_state.fault_comp,
            s_state.fault_elec, s_state.fault_ref, cur_f, arctic::MAX_ACTIVE_FAULTS);
        for (size_t i = 0; i < nc; ++i) {
            bool was_present = false;
            for (size_t j = 0; j < np; ++j) {
                if (prev_f[j].site == cur_f[i].site) { was_present = true; break; }
            }
            if (!was_present) event_log_record(EVENT_ERROR_APPEARED, cur_f[i].site);
        }
        for (size_t j = 0; j < np; ++j) {
            bool still_present = false;
            for (size_t i = 0; i < nc; ++i) {
                if (cur_f[i].site == prev_f[j].site) { still_present = true; break; }
            }
            if (!still_present) event_log_record(EVENT_ERROR_CLEARED, prev_f[j].site);
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
    ensureImageMutex();

    s_demo_mode = true;

    // Seed a realistic RUNNING image entirely through the opaque semantic API:
    // no register address, bit, or scaling factor appears here. The arctic-macon
    // library applies the wire encoding (e.g. AcVoltage 230 V -> raw 23).
    imageLock();
    s_image.clear();

    // Run-state / mode. (Compressor run-state is derived from compressor_freq,
    // and the operating-mode register is unused by the mapping, so neither is
    // seeded.)
    s_image.set_working_mode(MaconWorkingMode::FloorHeating);
    s_image.set_flag(MaconFlag::Pump, true);
    s_image.set_flag(MaconFlag::Fan, true);

    // Setpoints (whole °C).
    s_image.set_temp(MaconField::CoolingSetpoint, 18);
    s_image.set_temp(MaconField::HeatingSetpoint, 45);   // aux/heating, demo only
    s_image.set_temp(MaconField::HotWaterSetpoint, 50);
    s_image.set_value(MaconField::HotWaterCeiling, 50);   // AP13 ceiling

    // Temperatures (signed whole °C).
    s_image.set_temp(MaconField::WaterTankTemp, 42);
    s_image.set_temp(MaconField::OutletWaterTemp, 45);
    s_image.set_temp(MaconField::InletWaterTemp, 38);
    s_image.set_temp(MaconField::OutdoorAmbientTemp, 22);
    s_image.set_temp(MaconField::IndoorCoilTemp, 40);
    s_image.set_temp(MaconField::OutdoorCoilTemp, 35);
    s_image.set_temp(MaconField::SuctionTemp, 12);
    s_image.set_temp(MaconField::DischargeTemp, 85);
    s_image.set_temp(MaconField::IpmTemp, 55);

    // Electrical / system readings (natural units; the library owns scaling).
    s_image.set_value(MaconField::AcCurrent, 5);        // A
    s_image.set_value(MaconField::AcVoltage, 230);      // V
    s_image.set_value(MaconField::DcVoltage, 380);      // V
    s_image.set_value(MaconField::FanLevel, 40);        // raw DC motor level
    s_image.set_value(MaconField::PrimaryEev, 200);     // EEV steps
    s_image.set_value(MaconField::CompressorFreq, 60);  // Hz
    s_image.set_value(MaconField::RealtimePower, 1200); // W

    // Mark all five fault registers present (so faults decode as valid), keep
    // the unit running, then light the demo's default high-pressure protection
    // fault by semantic identity — no OEM code string here.
    s_image.clear_faults();
    s_image.set_flag(MaconFlag::UnitOn, true);
    s_image.set_fault(MaconFaultId::HighPressureProtection, true);
    imageUnlock();

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
    ensureImageMutex();
    s_feed_mode = true;

    imageLock();
    s_image.clear();
    imageUnlock();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = HeatPumpState();  // Reset to defaults (connected=false until first feed)
    xSemaphoreGive(s_state_mutex);

    s_was_connected = false;
    ESP_LOGI(TAG, "Macon register feed initialized");
}

bool isExternalFeed() {
    return s_feed_mode;
}

// The arctic-macon library is the single source of truth for the Macon Tuya
// register layout, scaling, and fault/icon bit positions. This adapter consumes
// the already-decoded MaconState; it does NOT re-interpret raw registers.
// High/low pressure (A11/A12) are uninstalled sensors on this DHW
// unit and are not reported.
//
// In Auto working mode, expose the actual water-side direction while the
// compressor runs (rather than the raw menu enum).
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
    // The arctic-macon library owns the register->field mapping. Copy the opaque
    // image under its own lock, release it, then decode the copy — so the image
    // lock and the state lock are never held simultaneously. This adapter only
    // (a) drives that decode and (b) adapts the native MaconState into the
    // controller's legacy HeatPumpState (status bitfields, WorkingMode enum).
    imageLock();
    MaconImage snapshot = s_image;
    imageUnlock();
    MaconState ms;
    snapshot.decode(&ms);

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
    // The image relays its bytes without interpretation; it owns the window base
    // so the controller needs no window-bound constants of its own.
    imageLock();
    uint16_t n = s_image.raw(base_out, out, max_count);
    imageUnlock();
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

    // Feed the window into the opaque image; it reports which decode-relevant
    // windows were covered (status / telemetry) without exposing any register
    // number to the controller.
    imageLock();
    const MaconCoverage cov = s_image.ingest_bytes(reg_base, regs, count);
    imageUnlock();

    // Map the image into HeatPumpState using the library-owned decode.
    applyMaconMapping();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (cov.status_updated) s_holding_window_ms = now;
    if (cov.telemetry_updated) s_telemetry_window_ms = now;
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
            !macon_has_fault_id(s_state.fault_run, s_state.fault_ee,
                                s_state.fault_comp, s_state.fault_elec,
                                s_state.fault_ref, MaconFaultId::InletWaterSensor) &&
            s_state.inlet_water_temp >= -50 && s_state.inlet_water_temp <= 150;
        snapshot.outlet_valid = telemetry_fresh && s_outlet_valid &&
            !macon_has_fault_id(s_state.fault_run, s_state.fault_ee,
                                s_state.fault_comp, s_state.fault_elec,
                                s_state.fault_ref, MaconFaultId::OutletWaterSensor) &&
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
    // Set the run-state flag by identity; the library owns the bit position and
    // preserves the fault bits sharing that register.
    imageLock();
    s_image.set_flag(MaconFlag::UnitOn, on);
    imageUnlock();
    applyMaconMapping();
    ESP_LOGI(TAG, "Unit power set to %s", on ? "ON" : "OFF");
    return true;
}

bool setWorkingMode(WorkingMode mode) {
    if (macon_master::is_active()) {
        ESP_LOGW(TAG, "Working-mode write unsupported in Tuya master mode (no verified fc06 mapping)");
        return false;
    }
    // The controller's WorkingMode and the library's MaconWorkingMode share the
    // same semantic encoding by construction; convert between the two enums
    // without touching any register.
    imageLock();
    s_image.set_working_mode(static_cast<MaconWorkingMode>(static_cast<uint8_t>(mode)));
    imageUnlock();
    applyMaconMapping();
    ESP_LOGI(TAG, "Working mode set to %s", workingModeToString(mode));
    return true;
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
    imageLock();
    s_image.set_temp(MaconField::CoolingSetpoint, temp);
    imageUnlock();
    applyMaconMapping();
    ESP_LOGI(TAG, "Cooling setpoint set to %d", temp);
    return true;
}

bool setHeatingSetpoint(int16_t temp) {
    temp = static_cast<int16_t>(clamp_setpoint(SetpointKind::Heating, temp));
    if (macon_master::is_active()) {
        // MaconLink deliberately has no set_heating_setpoint: reg2094 is
        // unverified on this unit. Fail explicitly rather than guess.
        ESP_LOGW(TAG, "Heating setpoint write unsupported in Tuya master mode (reg2094 unverified)");
        return false;
    }
    imageLock();
    s_image.set_temp(MaconField::HeatingSetpoint, temp);
    imageUnlock();
    applyMaconMapping();
    ESP_LOGI(TAG, "Heating setpoint set to %d", temp);
    return true;
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
    imageLock();
    s_image.set_temp(MaconField::HotWaterSetpoint, temp);
    imageUnlock();
    applyMaconMapping();
    ESP_LOGI(TAG, "Hot water setpoint set to %d", temp);
    return true;
}

bool writeRegister(uint16_t address, uint16_t value) {
    // The image owns the window bounds, so the controller keeps no window
    // constants of its own.
    if (!s_image.in_window(address)) {
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
    imageLock();
    s_image.set_register(address, value);
    imageUnlock();
    applyMaconMapping();
    ESP_LOGI(TAG, "Register %d set to %d", address, value);
    return true;
}

bool readRegister(uint16_t address, uint16_t* value_out) {
    if (value_out == nullptr) {
        return false;
    }
    // Demo, passive-listen, and active-master polling all populate this image.
    if (s_demo_mode || s_feed_mode) {
        imageLock();
        bool ok = s_image.get_register(address, value_out);
        imageUnlock();
        return ok;
    }
    ESP_LOGW(TAG, "Register image is not initialized");
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
    arctic::ActiveError active[arctic::MAX_ACTIVE_FAULTS];
    int n = arctic::getActiveErrors(active, arctic::MAX_ACTIVE_FAULTS);
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
    if (!s_demo_mode || field == nullptr) return false;

    // Map the demo field name to an opaque MaconImage operation. No register
    // address, bit position, or scaling factor appears here — the arctic-macon
    // library owns all of that. Fault injection is NOT a field write: use
    // injectDemoFault()/clearDemoFaults() (which reference faults by identity).
    struct FieldEntry { const char* name; MaconField field; };
    static const FieldEntry kFields[] = {
        { "water_tank_temp",      MaconField::WaterTankTemp },
        { "outlet_water_temp",    MaconField::OutletWaterTemp },
        { "inlet_water_temp",     MaconField::InletWaterTemp },
        { "discharge_temp",       MaconField::DischargeTemp },
        { "suction_temp",         MaconField::SuctionTemp },
        { "outdoor_coil_temp",    MaconField::OutdoorCoilTemp },
        { "indoor_coil_temp",     MaconField::IndoorCoilTemp },
        { "outdoor_ambient_temp", MaconField::OutdoorAmbientTemp },
        { "ipm_temp",             MaconField::IpmTemp },
        { "compressor_freq",      MaconField::CompressorFreq },
        { "fan_speed",            MaconField::FanLevel },
        { "ac_voltage",           MaconField::AcVoltage },
        { "ac_current",           MaconField::AcCurrent },
        { "dc_voltage",           MaconField::DcVoltage },
        { "main_eev",             MaconField::PrimaryEev },
        { "primary_eev_opening",  MaconField::PrimaryEev },
        { "realtime_power",       MaconField::RealtimePower },
        { "cooling_setpoint",     MaconField::CoolingSetpoint },
        { "heating_setpoint",     MaconField::HeatingSetpoint },
        { "hot_water_setpoint",   MaconField::HotWaterSetpoint },
    };
    struct FlagEntry { const char* name; MaconFlag flag; };
    static const FlagEntry kFlags[] = {
        { "fan_on",     MaconFlag::Fan },
        { "cooling_on", MaconFlag::Cooling },
        { "pump_on",    MaconFlag::Pump },
        { "unit_on",    MaconFlag::UnitOn },
    };

    bool matched = false;
    imageLock();
    for (const FieldEntry& e : kFields) {
        if (strcmp(field, e.name) == 0) {
            s_image.set_value(e.field, value);
            matched = true;
            break;
        }
    }
    if (!matched) {
        for (const FlagEntry& e : kFlags) {
            if (strcmp(field, e.name) == 0) {
                s_image.set_flag(e.flag, value != 0);
                matched = true;
                break;
            }
        }
    }
    if (!matched && strcmp(field, "working_mode") == 0) {
        s_image.set_working_mode(static_cast<MaconWorkingMode>(static_cast<uint8_t>(value)));
        matched = true;
    }
    imageUnlock();

    if (!matched) return false;

    // Re-decode the image into HeatPumpState so the change is reflected live.
    applyMaconMapping();

    ESP_LOGI(TAG, "[DEMO] Field '%s' set to %ld", field, (long)value);
    return true;
}

int injectDemoFault(const char* code, bool active) {
    if (!s_demo_mode || code == nullptr) return 0;
    imageLock();
    int sites = s_image.set_fault_by_code(code, active);
    imageUnlock();
    if (sites > 0) {
        applyMaconMapping();
        ESP_LOGI(TAG, "[DEMO] Fault '%s' %s (%d site%s)",
                 code, active ? "set" : "cleared", sites, sites == 1 ? "" : "s");
    }
    return sites;
}

void clearDemoFaults() {
    if (!s_demo_mode) return;
    imageLock();
    s_image.clear_faults();
    imageUnlock();
    applyMaconMapping();
    ESP_LOGI(TAG, "[DEMO] All faults cleared");
}

}  // namespace arctic
