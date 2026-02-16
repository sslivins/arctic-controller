# Arctic Controller

Controller for Arctic heat pump with LVGL-based UI on M5Stack Tab5 and web-based management interface.

## Hardware

- **Platform:** M5Stack Tab5 (ESP32-P4 main processor, ESP32-C6 WiFi co-processor)
- **Display:** 7" touch display with LVGL graphics library
- **RTC:** Battery-backed real-time clock
- **RS-485:** SIT3088 transceiver for Modbus RTU communication with heat pump

## Features

- **Heat Pump Communication** - Modbus RTU over RS-485 for real-time monitoring and control
- **LVGL-based Touch UI** - Status bar, heat pump dashboard, settings screens
- **Web Interface** - Responsive web UI at `http://arctic.local` for remote management
- **WiFi Management** - Connect to networks, credentials saved to NVS
- **Time Synchronization** - NTP sync with configurable timezone (saved to NVS)
- **REST API** - HTTP API for external control and monitoring
- **OTA Updates** - Over-the-air firmware updates via web UI or API
- **Security** - Optional web authentication and API key protection
- **Multi-Language** - Web interface in English, French, and Spanish
- **mDNS Discovery** - Access via `arctic.local` (auto-increments for multiple controllers)

## Heat Pump Communication

The controller communicates with the Arctic heat pump via Modbus RTU over RS-485:

- **Protocol:** Modbus RTU, 2400 baud, 8E1 (8 data bits, even parity, 1 stop bit)
- **Slave Address:** 1
- **Polling Interval:** 1 second (connected), 5 seconds (disconnected)

### Monitored Data
- Operating mode (Cooling, Floor Heating, Fan Coil Heating, Hot Water, Auto)
- Component status (Compressor, Fan with speed, Water Pump, Backup Heater)
- Temperatures (Tank, Outlet, Inlet, Outdoor Ambient)
- Setpoints (Cooling, Heating, Hot Water)
- System readings (Compressor frequency, Voltages, Currents, Pressures)
- Error codes with descriptions

### RS-485 Pinout
| Signal | GPIO |
|--------|------|
| TX     | 20   |
| RX     | 21   |
| DIR    | 34   |

## Web Interface

Access the web interface at `http://arctic.local` after connecting to WiFi.

### Dashboard
- Real-time heat pump status with mode indicator
- Temperature display (Tank, Setpoint, Outdoor)
- Component status (Compressor, Fan, Pump, Aux Heater)
- Error display
- System overview (uptime, time, timezone)
- Auto-refresh every 5 seconds

### Settings
- **Device Info** - Firmware version, platform, memory usage
- **WiFi Status** - Network, signal strength, IP address
- **Time Settings** - Timezone selection, 24h format, NTP sync
- **Firmware Updates** - Drag-and-drop .bin file upload with progress
- **Security** - Enable/disable authentication, manage API keys
- **System** - Reboot controller

### Security Options
- **Web Authentication** - Require login to access web interface (default credentials: `arctic`/`arctic`)
- **API Key Authentication** - Require `X-API-Key` header for programmatic API access

### Multi-Language Support
- English, Français, Español
- Language selector in header
- Preference saved in browser localStorage

## REST API

Full API documentation available in [docs/openapi.yaml](docs/openapi.yaml).

### Public Endpoints (no auth required)
| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/health` | GET | Simple health check |
| `/api/status` | GET | System status (WiFi, time, uptime) |
| `/api/time` | GET | Current time in multiple formats |
| `/api/wifi` | GET | WiFi connection status |
| `/api/auth/status` | GET | Check if auth is required |

### Protected Endpoints (auth optional, configurable)
| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/info` | GET | Device information |
| `/api/time/config` | GET/POST | Timezone and format settings |
| `/api/time/sync` | POST | Force NTP synchronization |
| `/api/ota/status` | GET | OTA update status |
| `/api/ota/update` | POST | Start OTA from URL (GitHub only) |
| `/api/ota/upload` | POST | Upload firmware binary |
| `/api/ota/reboot` | POST | Reboot device |
| `/api/heatpump/status` | GET | Heat pump status, temps, setpoints |
| `/api/auth/config` | GET/POST | Authentication settings |
| `/api/auth/credentials` | POST | Update username/password |
| `/api/auth/apikey` | GET | Retrieve API key |
| `/api/auth/apikey/regenerate` | POST | Generate new API key |

### Authentication
When API authentication is enabled, include the API key in requests:
```bash
curl -H "X-API-Key: your-api-key" http://arctic.local/api/info
```

### Examples
```bash
# Health check
curl http://arctic.local/api/health

# Get device status
curl http://arctic.local/api/status

# Upload firmware (with API key)
curl -X POST http://arctic.local/api/ota/upload \
     -H "X-API-Key: your-api-key" \
     -H "Content-Type: application/octet-stream" \
     --data-binary @arctic_controller.bin
```

## OTA Updates

The device supports Over-The-Air (OTA) firmware updates via the web interface or API.

### Via Web Interface (Recommended)
1. Open `http://arctic.local` in your browser
2. Go to Settings → Firmware Updates
3. Drag and drop your `.bin` file or click to browse
4. Monitor upload progress and wait for automatic reboot

### Via API
```bash
# Upload firmware binary
curl -X POST http://arctic.local/api/ota/upload \
     -H "Content-Type: application/octet-stream" \
     --data-binary @arctic_controller.bin

# Or from GitHub releases (URL must be from official repo)
curl -X POST http://arctic.local/api/ota/update \
     -H "Content-Type: application/json" \
     -d '{"url":"https://github.com/sslivins/arctic-controller/releases/download/v1.3.0/arctic_controller.bin"}'
```

**Safety Features:**
- Dual OTA partitions for safe updates with automatic rollback
- Firmware validation (ESP32 magic byte check)
- Concurrent update protection
- URL restriction to official GitHub repository

## Project Structure

```
├── main/                  # Application source code
│   ├── main.cpp           # Entry point
│   ├── api_server.*       # REST API and mDNS server
│   ├── auth_manager.*     # Session and API key authentication
│   ├── ota_manager.*      # OTA firmware update management
│   ├── time_manager.*     # Time and NTP management
│   ├── wifi_manager.*     # WiFi connection management
│   ├── status_bar.*       # Top status bar (time, WiFi indicator)
│   ├── time_screen.*      # Time/timezone settings screen
│   ├── wifi_screen.*      # WiFi configuration screen
│   └── web/
│       └── index.html     # Web interface (embedded in firmware)
├── docs/
│   └── openapi.yaml       # OpenAPI 3.0 specification
├── components/            # ESP-IDF components (BSP, etc.)
├── dependencies/          # External libraries (fetched via fetch_repos.py)
│   ├── lvgl/             # LVGL graphics library
│   ├── mooncake/         # Mooncake framework
│   ├── mooncake_log/     # Logging utilities
│   └── smooth_ui_toolkit/
├── CMakeLists.txt        # ESP-IDF project configuration
├── sdkconfig             # ESP-IDF settings
├── partitions.csv        # Partition table
├── lv_conf.h             # LVGL configuration
├── fetch_repos.py        # Dependency fetcher script
└── repos.json            # Dependency definitions
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

## Default Credentials

| Type | Username | Password/Key |
|------|----------|--------------|
| Web Login | `arctic` | `arctic` |
| API Key | N/A | Auto-generated (view in Settings) |

## Development

### Branch Protection

The `main` branch is protected to ensure code quality. All changes must go through pull requests, and the Device Tests workflow (build + device tests) must pass before merging.

To set up branch protection, see [.github/BRANCH_PROTECTION.md](.github/BRANCH_PROTECTION.md) or run:

```bash
./scripts/setup-branch-protection.sh
```

## TODO

- [ ] Add automatic update option checkbox - allow users to enable auto-updates so firmware installs automatically when available
- [ ] Temperature and sensor readings on dashboard
- [ ] Heat pump control commands

## License

[Add your license here]
