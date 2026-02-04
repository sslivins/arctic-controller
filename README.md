# Arctic Controller

Controller for Arctic heat pump with LVGL-based UI on M5Stack Tab5.

## Hardware

- **Platform:** M5Stack Tab5 (ESP32-S3)
- **Display:** LVGL graphics library

## Project Structure

```
├── main/               # Application source code
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

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) (v5.x recommended)
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
