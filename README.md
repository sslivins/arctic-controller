# Arctic Controller

Controller for Arctic heat pump with LVGL-based UI on M5Stack Tab5.

## Hardware

- **Platform:** M5Stack Tab5 (ESP32-P4 main processor, ESP32-C6 WiFi co-processor)
- **Display:** 7" touch display with LVGL graphics library
- **RTC:** Battery-backed real-time clock

## Features

- **LVGL-based Touch UI** - Status bar, time settings, WiFi configuration screens
- **WiFi Management** - Connect to networks, credentials saved to NVS
- **Time Synchronization** - NTP sync with configurable timezone (saved to NVS)
- **REST API** - HTTP API for external control and monitoring
- **mDNS Discovery** - Access via `arctic.local` (auto-increments to `arctic-2.local`, etc. for multiple controllers)

## REST API

Once connected to WiFi, the controller is accessible via mDNS:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/health` | GET | Simple health check |
| `/api/status` | GET | System status (WiFi, time, uptime) |
| `/api/time` | GET | Current time in multiple formats |
| `/api/wifi` | GET | WiFi connection status |
| `/api/info` | GET | Device information |
| `/api/ota/status` | GET | OTA update status |
| `/api/ota/update` | POST | Start OTA update (body: `{"url":"http://..."}`) |
| `/api/ota/reboot` | POST | Reboot after successful OTA |

**Example:**
```bash
curl http://arctic.local/api/status
```

## OTA Updates

The device supports Over-The-Air (OTA) firmware updates. The partition table uses dual OTA partitions (ota_0 and ota_1) for safe updates with automatic rollback.

**To perform an OTA update:**

1. Host your firmware binary on an HTTP/HTTPS server
2. Start the update:
   ```bash
   curl -X POST http://arctic.local/api/ota/update \
        -H "Content-Type: application/json" \
        -d '{"url":"http://your-server/arctic_controller.bin"}'
   ```
3. Monitor progress:
   ```bash
   curl http://arctic.local/api/ota/status
   ```
4. Reboot to apply:
   ```bash
   curl -X POST http://arctic.local/api/ota/reboot
   ```

**Rollback Protection:** If the new firmware fails to boot properly, the device will automatically rollback to the previous working version after a few failed attempts.

## Project Structure

```
├── main/               # Application source code
│   ├── main.cpp        # Entry point
│   ├── status_bar.*    # Top status bar (time, WiFi indicator)
│   ├── time_screen.*   # Time/timezone settings screen
│   ├── time_manager.*  # Time and NTP management
│   ├── wifi_screen.*   # WiFi configuration screen
│   ├── wifi_manager.*  # WiFi connection management
│   ├── api_server.*    # REST API and mDNS server
│   └── ota_manager.*   # OTA firmware update management
├── components/         # ESP-IDF components (BSP, etc.)
├── dependencies/       # External libraries (fetched via fetch_repos.py)
│   ├── lvgl/          # LVGL graphics library
│   ├── mooncake/      # Mooncake framework
│   ├── mooncake_log/  # Logging utilities
│   └── smooth_ui_toolkit/
├── CMakeLists.txt     # ESP-IDF project configuration
├── sdkconfig          # ESP-IDF settings
├── partitions.csv     # Partition table
├── lv_conf.h          # LVGL configuration
├── fetch_repos.py     # Dependency fetcher script
└── repos.json         # Dependency definitions
```

## Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/) (v5.4.x recommended)
- Python 3.x

## Getting Started

### 1. Fetch Dependencies

```bash
python fetch_repos.py
```

### 2. Build the Project

```bash
idf.py build
```

### 3. Flash to Device

```bash
idf.py -p [PORT] flash
```

### 4. Monitor Output

```bash
idf.py -p [PORT] monitor
```

Or build, flash, and monitor in one command:

```bash
idf.py -p [PORT] flash monitor
```

## Configuration

- **LVGL settings:** `lv_conf.h`
- **ESP-IDF settings:** `sdkconfig`

Run `idf.py menuconfig` to configure ESP-IDF options.

## TODO

- [ ] Add automatic update option checkbox - allow users to enable auto-updates so firmware installs automatically when available
- [ ] Web interface HTML for settings
- [ ] API authentication (API key, session cookies)
- [ ] Localization (i18n) support

## License

[Add your license here]
