/*
 * Arctic Heat Pump Controller
 * Shared Demo State - Implementation
 */
#include "heatpump_demo_state.h"

// Static demo state - shared between UI and API
static bool s_demo_power = true;
static arctic::WorkingMode s_demo_mode = arctic::WorkingMode::FLOOR_HEATING;
static int16_t s_demo_cooling = 18;
static int16_t s_demo_heating = 45;
static int16_t s_demo_hotwater = 50;

bool heatpump_demo_get_power() {
    return s_demo_power;
}

void heatpump_demo_set_power(bool on) {
    s_demo_power = on;
}

arctic::WorkingMode heatpump_demo_get_mode() {
    return s_demo_mode;
}

void heatpump_demo_set_mode(arctic::WorkingMode mode) {
    s_demo_mode = mode;
}

int16_t heatpump_demo_get_cooling_setpoint() {
    return s_demo_cooling;
}

int16_t heatpump_demo_get_heating_setpoint() {
    return s_demo_heating;
}

int16_t heatpump_demo_get_hotwater_setpoint() {
    return s_demo_hotwater;
}

void heatpump_demo_set_cooling_setpoint(int16_t temp) {
    s_demo_cooling = temp;
}

void heatpump_demo_set_heating_setpoint(int16_t temp) {
    s_demo_heating = temp;
}

void heatpump_demo_set_hotwater_setpoint(int16_t temp) {
    s_demo_hotwater = temp;
}
