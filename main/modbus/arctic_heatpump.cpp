/*
 * Arctic Heat Pump State and Control
 */

#include "arctic_heatpump.h"
#include "modbus_manager.h"
#include "heatpump_errors.h"
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

// Connection state tracking
static const uint8_t MAX_CONSECUTIVE_FAILURES = 5;
static bool s_was_connected = false;  // For logging state changes

// Get current time in milliseconds
static uint32_t getTimeMs() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Poll holding registers (settings)
static bool pollHoldingRegisters() {
    uint16_t data[8];
    esp_err_t err = modbus::readHoldingRegisters(SLAVE_ADDRESS, reg::UNIT_ON_OFF, 8, data);
    
    if (err != ESP_OK) {
        return false;
    }
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.unit_on = (data[0] != 0);
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
    esp_err_t err = modbus::readHoldingRegisters(SLAVE_ADDRESS, reg::WATER_TANK_TEMP, 18, data);
    
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
    esp_err_t err = modbus::readHoldingRegisters(SLAVE_ADDRESS, reg::COMPRESSOR_FREQ, 10, data);
    
    if (err != ESP_OK) {
        return false;
    }
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.compressor_freq = data[0];        // 2118
    s_state.fan_speed = data[1];              // 2119
    s_state.ac_voltage = data[2];             // 2120
    s_state.ac_current = data[3];             // 2121
    s_state.dc_voltage = data[4];             // 2122
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
    // Read registers 2135-2138 (but they're not contiguous from previous reads)
    // We need to read starting from 2135, which is 2135-2100=35 registers after 2100
    // Or we can read just 4 registers starting at 2135
    uint16_t data[4];
    esp_err_t err = modbus::readHoldingRegisters(SLAVE_ADDRESS, reg::STATUS_1, 4, data);
    
    if (err != ESP_OK) {
        return false;
    }
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.status1 = data[0];  // 2135
    s_state.status2 = data[1];  // 2136
    s_state.error1 = data[2];   // 2137
    s_state.error2 = data[3];   // 2138
    xSemaphoreGive(s_state_mutex);
    
    return true;
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
            }
        } else {
            s_state.consecutive_failures++;
            
            if (s_state.consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                s_state.connected = false;
                poll_interval = POLL_INTERVAL_DISCONNECTED_MS;
                
                // Log disconnection once
                if (s_was_connected) {
                    ESP_LOGW(TAG, "Heat pump disconnected: %s", modbus::getLastError());
                    s_was_connected = false;
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
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = HeatPumpState();
    
    // Connection
    s_state.connected = true;
    s_state.last_successful_read_ms = getTimeMs();
    
    // Settings
    s_state.unit_on = true;
    s_state.working_mode = WorkingMode::FLOOR_HEATING;
    s_state.cooling_setpoint = 18;
    s_state.heating_setpoint = 45;
    s_state.hot_water_setpoint = 50;
    
    // Temperatures (°C)
    s_state.water_tank_temp = 42;
    s_state.outlet_water_temp = 45;
    s_state.inlet_water_temp = 38;
    s_state.discharge_temp = 85;
    s_state.suction_temp = 12;
    s_state.outdoor_coil_temp = 35;
    s_state.indoor_coil_temp = 40;
    s_state.outdoor_ambient_temp = 22;
    s_state.ipm_temp = 55;
    
    // System readings
    s_state.compressor_freq = 60;
    s_state.fan_speed = 850;
    s_state.ac_voltage = 230;
    s_state.ac_current = 52;     // tenths of amps → 5.2A → ~1200W
    s_state.dc_voltage = 3800;   // ÷10 = 380V
    s_state.dc_current = 3;
    s_state.primary_eev_opening = 320;
    s_state.secondary_eev_opening = 0;
    s_state.high_pressure = 280; // ÷100 = 2.80 MPa
    s_state.low_pressure = 85;   // ÷100 = 0.85 MPa
    
    // Status - compressor, fan medium, water pump running
    s_state.status1 = status1::UNIT_ON | status1::COMPRESSOR | status1::FAN_MED | status1::WATER_PUMP;
    s_state.status2 = 0;
    
    // Errors - P02 high pressure active
    s_state.error1 = 0;
    s_state.error2 = error2::HIGH_PRESSURE;
    
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

void startPolling() {
    if (s_demo_mode) {
        ESP_LOGI(TAG, "Demo mode - polling disabled");
        return;
    }
    
    if (s_poll_task != nullptr) {
        ESP_LOGW(TAG, "Polling already running");
        return;
    }
    
    if (!modbus::isInitialized()) {
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
    if (s_demo_mode) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.unit_on = on;
        if (on) {
            s_state.status1 = status1::UNIT_ON | status1::COMPRESSOR | status1::FAN_MED | status1::WATER_PUMP;
        } else {
            s_state.status1 = 0;
        }
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "[DEMO] Unit power set to %s", on ? "ON" : "OFF");
        return true;
    }
    
    esp_err_t err = modbus::writeSingleRegister(SLAVE_ADDRESS, reg::UNIT_ON_OFF, on ? 1 : 0);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.unit_on = on;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Unit power set to %s", on ? "ON" : "OFF");
        return true;
    }
    ESP_LOGE(TAG, "Failed to set unit power: %s", modbus::getLastError());
    return false;
}

bool setWorkingMode(WorkingMode mode) {
    if (s_demo_mode) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.working_mode = mode;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "[DEMO] Working mode set to %s", workingModeToString(mode));
        return true;
    }
    
    esp_err_t err = modbus::writeSingleRegister(SLAVE_ADDRESS, reg::WORKING_MODE, static_cast<uint16_t>(mode));
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
    if (s_demo_mode) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.cooling_setpoint = temp;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "[DEMO] Cooling setpoint set to %d", temp);
        return true;
    }
    
    esp_err_t err = modbus::writeSingleRegister(SLAVE_ADDRESS, reg::COOLING_SETPOINT, static_cast<uint16_t>(temp));
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
    if (s_demo_mode) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.heating_setpoint = temp;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "[DEMO] Heating setpoint set to %d", temp);
        return true;
    }
    
    esp_err_t err = modbus::writeSingleRegister(SLAVE_ADDRESS, reg::HEATING_SETPOINT, static_cast<uint16_t>(temp));
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
    if (s_demo_mode) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.hot_water_setpoint = temp;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "[DEMO] Hot water setpoint set to %d", temp);
        return true;
    }
    
    esp_err_t err = modbus::writeSingleRegister(SLAVE_ADDRESS, reg::HOT_WATER_SETPOINT, static_cast<uint16_t>(temp));
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
    if (s_demo_mode) {
        ESP_LOGI(TAG, "[DEMO] Register %d set to %d (ignored)", address, value);
        return true;
    }
    
    // Validate address is in writable range (2000-2057)
    if (address < 2000 || address > 2057) {
        ESP_LOGE(TAG, "Invalid register address %d (must be 2000-2057)", address);
        return false;
    }
    
    esp_err_t err = modbus::writeSingleRegister(SLAVE_ADDRESS, address, value);
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
    
    if (s_demo_mode) {
        *value_out = 0;
        ESP_LOGD(TAG, "[DEMO] Register %d read = 0", address);
        return true;
    }
    
    esp_err_t err = modbus::readHoldingRegisters(SLAVE_ADDRESS, address, 1, value_out);
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
    
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool found = true;
    
    // Temperatures
    if (strcmp(field, "water_tank_temp") == 0)       s_state.water_tank_temp = (int16_t)value;
    else if (strcmp(field, "outlet_water_temp") == 0) s_state.outlet_water_temp = (int16_t)value;
    else if (strcmp(field, "inlet_water_temp") == 0)  s_state.inlet_water_temp = (int16_t)value;
    else if (strcmp(field, "discharge_temp") == 0)    s_state.discharge_temp = (int16_t)value;
    else if (strcmp(field, "suction_temp") == 0)      s_state.suction_temp = (int16_t)value;
    else if (strcmp(field, "outdoor_coil_temp") == 0) s_state.outdoor_coil_temp = (int16_t)value;
    else if (strcmp(field, "indoor_coil_temp") == 0)  s_state.indoor_coil_temp = (int16_t)value;
    else if (strcmp(field, "outdoor_ambient_temp") == 0) s_state.outdoor_ambient_temp = (int16_t)value;
    else if (strcmp(field, "ipm_temp") == 0)          s_state.ipm_temp = (int16_t)value;
    // System readings
    else if (strcmp(field, "compressor_freq") == 0)   s_state.compressor_freq = (uint16_t)value;
    else if (strcmp(field, "fan_speed") == 0)         s_state.fan_speed = (uint16_t)value;
    else if (strcmp(field, "ac_voltage") == 0)        s_state.ac_voltage = (uint16_t)value;
    else if (strcmp(field, "ac_current") == 0)        s_state.ac_current = (uint16_t)value;
    else if (strcmp(field, "dc_voltage") == 0)        s_state.dc_voltage = (uint16_t)value;
    else if (strcmp(field, "dc_current") == 0)        s_state.dc_current = (uint16_t)value;
    else if (strcmp(field, "primary_eev_opening") == 0) s_state.primary_eev_opening = (uint16_t)value;
    else if (strcmp(field, "secondary_eev_opening") == 0) s_state.secondary_eev_opening = (uint16_t)value;
    else if (strcmp(field, "high_pressure") == 0)     s_state.high_pressure = (uint16_t)value;
    else if (strcmp(field, "low_pressure") == 0)      s_state.low_pressure = (uint16_t)value;
    // Status/error registers
    else if (strcmp(field, "status1") == 0)           s_state.status1 = (uint16_t)value;
    else if (strcmp(field, "status2") == 0)           s_state.status2 = (uint16_t)value;
    else if (strcmp(field, "error1") == 0)            s_state.error1 = (uint16_t)value;
    else if (strcmp(field, "error2") == 0)            s_state.error2 = (uint16_t)value;
    // Settings (writable, but also accessible here)
    else if (strcmp(field, "unit_on") == 0)           s_state.unit_on = (value != 0);
    else if (strcmp(field, "working_mode") == 0)      s_state.working_mode = static_cast<WorkingMode>(value);
    else if (strcmp(field, "cooling_setpoint") == 0)  s_state.cooling_setpoint = (int16_t)value;
    else if (strcmp(field, "heating_setpoint") == 0)  s_state.heating_setpoint = (int16_t)value;
    else if (strcmp(field, "hot_water_setpoint") == 0) s_state.hot_water_setpoint = (int16_t)value;
    else found = false;
    
    xSemaphoreGive(s_state_mutex);
    
    if (found) {
        ESP_LOGI(TAG, "[DEMO] Field '%s' set to %ld", field, (long)value);
    }
    return found;
}

}  // namespace arctic
