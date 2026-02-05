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
| `/health` | GET | Simple health check |
| `/api/status` | GET | System status (WiFi, time, uptime) |
| `/api/time` | GET | Current time in multiple formats |
| `/api/wifi` | GET | WiFi connection status |
| `/api/info` | GET | Device information |

**Example:**
```bash
curl http://arctic.local/api/status
```

## Project Structure

```
├── main/               # Application source code
│   ├── main.cpp        # Entry point
│   ├── status_bar.*    # Top status bar (time, WiFi indicator)
│   ├── time_screen.*   # Time/timezone settings screen
│   ├── time_manager.*  # Time and NTP management
│   ├── wifi_screen.*   # WiFi configuration screen
│   ├── wifi_manager.*  # WiFi connection management
│   └── api_server.*    # REST API and mDNS server
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

## License

[Add your license here]
