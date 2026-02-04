# ESP32-C6 WiFi Setup for M5Stack Tab5

The M5Stack Tab5 uses a dual-chip architecture:
- **ESP32-P4**: Main processor (runs your application code)
- **ESP32-C6**: Co-processor that handles WiFi/BLE via SDIO interface

The ESP32-P4 does NOT have built-in WiFi. All WiFi operations go through the ESP32-C6 using the **ESP-Hosted** protocol over SDIO.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                        M5Stack Tab5                          │
│                                                              │
│  ┌──────────────┐      SDIO Bus       ┌──────────────┐     │
│  │   ESP32-P4   │◄──────────────────►│   ESP32-C6   │     │
│  │  (Main App)  │                     │   (WiFi)     │     │
│  │              │                     │              │     │
│  │ - Display    │                     │ - WiFi STA   │     │
│  │ - Touch      │                     │ - WiFi AP    │     │
│  │ - UI (LVGL)  │                     │ - BLE        │     │
│  │ - Business   │                     │              │     │
│  │   Logic      │                     │              │     │
│  └──────────────┘                     └──────────────┘     │
│         │                                    │              │
│         │ I2C                               │ RF           │
│         ▼                                   ▼              │
│  ┌──────────────┐                    ┌──────────────┐     │
│  │  IO Expander │                    │   Antenna    │     │
│  │   (PI4IOE)   │                    │  (Int/Ext)   │     │
│  └──────────────┘                    └──────────────┘     │
└─────────────────────────────────────────────────────────────┘
```

## ESP32-C6 Firmware

The ESP32-C6 needs to be flashed with ESP-Hosted firmware **separately** from the P4 firmware. This firmware runs on the C6 and exposes WiFi/BLE to the P4 over SDIO.

### Firmware Location
M5Stack provides the firmware binary:
- Repository: `m5stack/M5Tab5-UserDemo`
- Path: `platforms/tab5/wifi_c6_fw/ESP32C6-WiFi-SDIO-Interface-V1.4.1-96bea3a_0x0.bin`

### Flashing the ESP32-C6 Firmware

**Important**: The ESP32-C6 is flashed at address `0x0`, which is different from the P4.

1. Download the firmware binary from M5Stack's repository
2. Use esptool.py to flash directly to the C6:
   ```bash
   esptool.py --port <C6_PORT> write_flash 0x0 ESP32C6-WiFi-SDIO-Interface-V1.4.1-96bea3a_0x0.bin
   ```

**Note**: You need to identify which USB port corresponds to the C6. The Tab5 may have multiple USB endpoints.

## Enabling WiFi on ESP32-P4

### Step 1: Enable ESP-Hosted in sdkconfig

Add to `sdkconfig.defaults`:
```
CONFIG_ESP_HOSTED_ENABLED=y
```

### Step 2: Power on the C6 Module

Before using WiFi, you must enable power to the C6:
```c
#include <bsp/m5stack_tab5.h>

// Enable power to ESP32-C6 WiFi module
bsp_set_wifi_power_enable(true);
```

### Step 3: Initialize ESP-Hosted

```c
#include "esp_hosted.h"
#include "esp_wifi.h"

void wifi_init(void)
{
    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
}
```

### Step 4: Scan for Networks

```c
void wifi_scan(void)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));
    
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    
    for (int i = 0; i < ap_count; i++) {
        printf("SSID: %s, RSSI: %d, Auth: %d\n", 
               ap_records[i].ssid, 
               ap_records[i].rssi, 
               ap_records[i].authmode);
    }
    
    free(ap_records);
}
```

### Step 5: Connect to a Network

```c
void wifi_connect(const char* ssid, const char* password)
{
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}
```

## GPIO Connections (P4 to C6 via SDIO)

Based on the M5Tab5-UserDemo code:
```
GPIO_NUM_8   - SDIO
GPIO_NUM_9   - SDIO
GPIO_NUM_10  - SDIO
GPIO_NUM_11  - SDIO
GPIO_NUM_12  - SDIO
GPIO_NUM_13  - SDIO
```

## External Antenna Support

The Tab5 supports external antenna via the BSP:
```c
// Switch between internal and external antenna
bsp_set_ext_antenna_enable(true);   // Use external antenna
bsp_set_ext_antenna_enable(false);  // Use internal antenna
```

## Troubleshooting

### C6 Not Responding
1. Check that `bsp_set_wifi_power_enable(true)` is called
2. Verify the C6 firmware is flashed correctly
3. Check SDIO connections

### WiFi Scan Returns Empty
1. Ensure `CONFIG_ESP_HOSTED_ENABLED=y` is set
2. Check that WiFi is started with `esp_wifi_start()`
3. Add delay after power-on for C6 to initialize

### Connection Failures
1. Verify SSID and password are correct
2. Check if network is in range (RSSI > -80 dBm preferred)
3. Ensure network supports the authentication mode

## References

- [ESP-Hosted GitHub](https://github.com/espressif/esp-hosted)
- [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo)
- [ESP-IDF WiFi Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html)
