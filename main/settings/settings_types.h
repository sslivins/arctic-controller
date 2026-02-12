/*
 * Arctic Heat Pump Controller
 * Settings - Shared Type Definitions
 * 
 * Types used across settings screens.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi network information
 */
typedef struct {
    char ssid[33];      // Max SSID length is 32 + null terminator
    int8_t rssi;        // Signal strength
    uint8_t authmode;   // Authentication mode (0=open, 1=WEP, 2=WPA, etc.)
} settings_wifi_network_t;

#ifdef __cplusplus
}
#endif
