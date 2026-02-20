# Arctic Controller – TODO

## COP / Energy Monitoring

- [ ] Add configurable water flow rate setting (default ~20 L/min, user adjusts to match their circulator pump)
- [ ] Store flow rate in NVS so it persists across reboots
- [ ] Start polling saturation temp registers (2111-2113) — currently skipped in `pollTemperatures()`
- [ ] Calculate thermal output: `flow_rate (L/s) × 4186 (J/kg·K) × ΔT (outlet - inlet)`
- [ ] Calculate COP: `thermal_output_W / electrical_input_W` (only when compressor running)
- [ ] Expose COP, thermal output, electrical input in `HeatPumpState` struct
- [ ] Add COP/energy fields to REST API and demo mode injection
- [ ] Add flow rate configuration to the Advanced/Params screen

## Screen Render Performance

Baseline numbers measured Feb 2026 (ESP32-P4, ESP-IDF 5.4.3, LVGL 9.2).
Budget: 300 ms normal / 500 ms heavy-state. All currently passing.

| Screen | Render (ms) | Budget (ms) | Usage | Notes |
|--------|-------------|-------------|-------|-------|
| temps | 10 | 300 | 3% | |
| system | 16 | 300 | 5% | |
| settings | 20 | 300 | 7% | |
| errors (empty) | 28 | 300 | 9% | |
| control | 56 | 300 | 19% | |
| errors (history) | 187 | 500 | 37% | active + cleared errors |
| event_log | 274 | 300 | 91% | ⚠️ tightest margin |
| event_log (full) | 275 | 300 | 92% | ⚠️ 50 cards, near budget |
| errors (heavy) | 311 | 500 | 62% | 16 active errors, capped |

Future optimizations:
- [ ] **Event log screen**: reduce per-card widget count (eliminate nested `top_row`
      container — use absolute alignment on the card instead of inner flex). Target: < 200 ms
- [ ] **Event log screen**: lazy-load scroll (see below) — only create visible cards,
      append on scroll. Would bring initial render to ~50 ms regardless of event count
- [ ] **Errors screen (heavy)**: evaluate if the error card structure can be simplified
      (currently ~9 widgets per card). Less urgent since the 16-error cap keeps it well
      under the 500 ms heavy budget
- [ ] **Control screen**: profile why 56 ms — relatively high for a single-screen with
      sliders/toggles. May have unnecessary nested containers

## Event Log Enhancements

- [ ] **Lazy-load scroll**: Replace the fixed display cap (`MAX_DISPLAYED_EVENTS`)
      with incremental loading — render the first ~10 events, then append more on
      `LV_EVENT_SCROLL_END` as the user scrolls toward the bottom. LVGL scroll
      callbacks run in the display task context (no `bsp_display_lock()` needed),
      so appending 3-5 widgets per scroll event is safe and avoids the O(n²) flex
      layout issue that caused the original hang at 128 items.
- [ ] Consider logging compressor frequency changes (e.g. significant jumps or thresholds)
- [ ] Consider logging fan speed changes (RPM thresholds or level transitions)

## Modbus Heat Pump Simulator (M5Stack AtomS3 / AtomS3R + RS485)

Standalone ESP32-S3 project that acts as a Modbus RTU slave, emulating the ECO-600 heat pump register map. Connects to the Tab5 controller via RS485 for end-to-end Modbus testing without a real heat pump.

Two separate physical devices:
1. **Sniffer** — spliced onto the real controller↔heat pump bus, captures traffic, streams via MQTT to a broker on the VM
2. **Simulator** — connected to the Tab5 on the test rig, serves Modbus registers, controlled via REST + subscribes to MQTT for recording playback

Both run the same ESP-IDF project (`arctic-heatpump-simulator/`) with a build-time or NVS config flag selecting the mode.

### Hardware
- [ ] 2× M5Stack AtomS3 Lite or AtomS3R (ESP32-S3; R variant has 8 MB PSRAM if needed) + RS485 units
- [ ] Sniffer: T-splice A/B/GND from existing controller↔heat pump RS485 bus (keep stub < 30 cm)
- [ ] Simulator: Wire RS485 A/B to Tab5 RS485 port (half-duplex, shared bus)

### MQTT Broker (VM)

Central message bus for streaming sniffer data and feeding recordings back to the simulator. Runs on a Linux VM on the local network.

- [ ] Install Mosquitto: `sudo apt install mosquitto mosquitto-clients`
- [ ] Default config (no TLS, port 1883) — LAN-only, no auth needed
- [ ] Topics:
  - `arctic/sniffer/frames` — sniffer publishes captured frames here
  - `arctic/simulator/playback` — simulator subscribes here for recording playback
  - `arctic/simulator/command` — optional: remote control commands to simulator
- [ ] Test connectivity: `mosquitto_sub -h <vm-ip> -t "#"` to see all traffic

### Firmware (shared project)
- [ ] New ESP-IDF project: `arctic-heatpump-simulator/`
- [ ] Mode selection: `CONFIG_DEVICE_MODE` Kconfig option (`SNIFFER` or `SIMULATOR`), or runtime NVS flag switchable via serial console
- [ ] WiFi STA mode with configurable SSID/password (stored in NVS, set via serial console on first boot)
- [ ] MQTT client: `esp-mqtt` component, connects to broker IP (configurable via NVS/Kconfig), no TLS (~15 KB RAM)
- [ ] Common: UART driver, CRC-16 helper, register definitions (reuse `arctic_registers.h`)

### Sniffer Mode

Passively captures all RS485 bus traffic between the real Arctic controller and heat pump. Publishes frames via MQTT to the broker on the VM for long-term storage and later analysis.

**Intended workflow**: Splice sniffer onto existing controller↔heat pump RS485 bus. Run for 24 hours. Frames stream via MQTT → VM writes to disk (JSONL file). Analyze recording to extract real register values, poll patterns, timing. Load data into simulator as realistic presets.

#### Firmware (Sniffer)
- [ ] Hold RS485 DIR pin LOW at boot (permanent receive mode — electrically invisible on the bus)
- [ ] Configure UART: 2400 baud, 8E1, receive-only
- [ ] Frame detection via inter-character timeout: UART read with ~20 ms timeout naturally segments frames (3.5-char gap at 2400 baud ≈ 16 ms)
- [ ] For each captured frame, record: timestamp (µs since boot), raw bytes, frame length
- [ ] Infer direction (master vs slave) from frame alternation — master always speaks first after an idle gap, slave responds immediately
- [ ] Parse Modbus fields: slave address, function code, register start/count (requests), byte count + data (responses)
- [ ] Validate CRC-16 on each frame; flag CRC errors but still log the frame
- [ ] Publish each frame to MQTT topic `arctic/sniffer/frames` as JSON
- [ ] Frame JSON format: `{"t_ms": 0, "dir": "M"|"S", "raw_hex": "0103 07D0 0027 C5FC", "fn": 3, "addr": 2000, "count": 39, "crc_ok": true}`

#### On-Device RAM Buffer
- [ ] Ring buffer in RAM: 32–64 KB (~5–10 minutes of traffic at 2400 baud / 500 ms polls)
- [ ] Safety net if MQTT connection momentarily drops — re-publishes buffered frames on reconnect
- [ ] Frame struct: `{ uint64_t timestamp_us; uint16_t length; uint8_t direction; uint8_t data[256]; bool crc_ok; }`

#### Sniffer REST Endpoints (status/diagnostics only)
- [ ] mDNS: `sniffer.local`
- [ ] `GET /api/status` — uptime, frame count, frames/sec, CRC error count, MQTT connected, buffer fill %
- [ ] `GET /api/buffer` — dump current ring buffer contents as JSON array (last ~5–10 min)
- [ ] `POST /api/reset` — clear counters

### Simulator Mode

Responds to Modbus RTU master requests from the Tab5. Register values controlled via REST API and MQTT playback.

#### Firmware (Simulator)
- [ ] Modbus RTU slave, address 1, 2400 baud, 8E1 (matching `arctic_registers.h`)
- [ ] Holding register map: reuse `arctic_registers.h` definitions (registers 2000-2138)
- [ ] Respond to function code 0x03 (read holding registers) — return current register values
- [ ] Respond to function code 0x06 (write single register) — update register and log change
- [ ] All registers pre-populated with realistic defaults (same as `initDemoState()`)
- [ ] Subscribe to MQTT topic `arctic/simulator/playback` — when frames arrive, parse slave responses and load register values into the register map (enables recording playback)

### WiFi + REST API (simulator — for test automation)

The simulator exposes an HTTP API so pytest scripts (running on the Pi runner or dev machine) can remotely control the Modbus registers. This enables fully automated end-to-end integration tests: script sets registers on the Atom → Tab5 polls via RS485 → script reads Tab5 UI state via its test API → asserts correctness.

- [ ] mDNS: `simulator.local`
- [ ] Lightweight HTTP server (same approach as Tab5: `httpd_start()` with URI handlers)

#### Endpoints
- [ ] `GET /api/status` — health check: uptime, WiFi RSSI, Modbus stats (frames served, CRC errors), current scenario name, MQTT connected
- [ ] `GET /api/registers` — dump all register values as JSON (keyed by name from `arctic_registers.h`)
- [ ] `POST /api/registers` — bulk set registers by name or address: `{"error2": "0x0040", "water_tank_temp": 450, "compressor_freq": 55}`
- [ ] `POST /api/error` — set a single error by code: `{"code": "P02"}` (maps code → register/bit automatically)
- [ ] `DELETE /api/errors` — clear all error1/error2 bits
- [ ] `POST /api/scenario` — activate a named scenario: `{"name": "defrost", "duration_s": 30}` (runs the auto-behavior state machine)
- [ ] `POST /api/reset` — reset all registers to power-on defaults (same as `initDemoState()` on Tab5)
- [ ] `POST /api/comms` — control Modbus responsiveness: `{"mode": "normal"}`, `{"mode": "silent"}` (stop responding to simulate disconnect), `{"mode": "delayed", "delay_ms": 500}` (simulate slow responses)
- [ ] `POST /api/playback` — load a recording file (JSON array of frames) and begin replaying register values on a schedule

#### Python Client (`SimulatorClient`)
- [ ] Add `tests/device/simulator_client.py` wrapping the above endpoints (mirrors `DeviceClient` pattern)
- [ ] Add `simulator` pytest fixture in `conftest.py` (reads `SIMULATOR_URL` env var, defaults to `http://simulator.local`)
- [ ] Integration tests import both `device` and `simulator` fixtures

### Host-Side Scripts (VM / Pi)

#### Recording (MQTT → disk)
- [ ] `scripts/record_bus.py` — subscribes to `arctic/sniffer/frames` on the MQTT broker, writes each frame to a JSONL file (one JSON object per line)
- [ ] Uses `paho-mqtt` (standard Python MQTT client): `pip install paho-mqtt`
- [ ] Output filename: `recordings/bus_capture_YYYYMMDD_HHMMSS.jsonl`
- [ ] Print live summary to console: frames/sec, last register read, any CRC errors
- [ ] Graceful shutdown on Ctrl+C — flush file, print total stats
- [ ] Optional duration flag: `--duration 24h`
- [ ] Alternative one-liner (no script needed): `mosquitto_sub -h <broker> -t arctic/sniffer/frames >> capture.jsonl`

#### Playback (disk → MQTT → simulator)
- [ ] `scripts/replay_recording.py` — reads a `.jsonl` file, publishes slave-response frames to `arctic/simulator/playback` with original timing
- [ ] Parses `t_ms` from each frame to reproduce inter-frame delays
- [ ] Only replays slave (dir=S) frames — the simulator uses these to populate its register map
- [ ] Speed multiplier flag: `--speed 10` to replay 24 hours in 2.4 hours
- [ ] Loop flag: `--loop` for continuous replay
- [ ] Alternative one-liner for testing: `mosquitto_pub -h <broker> -t arctic/simulator/playback -f frame.json`

#### Analysis
- [ ] `scripts/analyze_recording.py` — parse a `.jsonl` recording and produce:
  - Which registers the controller actually polls (and which it skips)
  - Poll order and cycle time (e.g., reads 2000-2038 then 2100-2138 every 500 ms)
  - Function codes used for writes (0x06 vs 0x10)
  - Register value distributions (real-world temp ranges, normal status bit patterns)
  - Timing stats: request-response gap, inter-poll interval, retries on timeout
- [ ] Extract register snapshots as simulator presets (JSON files loadable via `POST /api/registers`)
- [ ] Detect anomalies: CRC errors, unexpected function codes, out-of-range register addresses

### Simulation Features
- [ ] **Scenario presets**: idle, heating, cooling, defrost, fault — each preset sets a coherent combination of temps/status/errors
- [ ] **Auto-behavior**: when unit_on and mode set, slowly ramp temps toward setpoint, toggle compressor/fan/pump status bits, update frequency/RPM
- [ ] **Error injection**: cycle through error1/error2 bit patterns on command
- [ ] **Serial console menu**: switch presets, inject errors, adjust individual registers via UART commands
- [ ] **Temperature drift**: ambient/coil/discharge temps change gradually based on mode, making the controller's polling feel realistic

### End-to-End Test Scenarios (pytest + simulator + device)

Test flow: pytest script → sets state on simulator via REST → waits for Tab5 to poll (1-2s) → reads Tab5 UI/API state → asserts.

- [ ] Controller connects and reads valid data → verify dashboard populates correctly
- [ ] Simulator injects P02 error (`POST /api/error {"code":"P02"}`) → verify Tab5 error card shows "P02" via `/api/test/ui-state`
- [ ] Iterate all 32 error codes: set each bit individually on simulator → verify Tab5 catches each one (parameterized test, mirrors `test_error_mapping.py` but over real RS485)
- [ ] Controller sends mode change (write reg 2001) → verify simulator register updated via `GET /api/registers`
- [ ] Controller sends setpoint change → verify simulator register updated
- [ ] Controller sends power off (write reg 2000 = 0) → verify simulator stops "running"
- [ ] Simulate communication failure (`POST /api/comms {"mode":"silent"}`) → verify controller shows DISCONNECTED after timeout
- [ ] Restore communication (`POST /api/comms {"mode":"normal"}`) → verify controller reconnects and resumes polling
- [ ] Defrost cycle scenario → verify controller detects state transition and logs events
- [ ] Temperature ramp scenario → verify Tab5 dashboard temps update progressively over time

## Main Screen Layout Redesign

- [x] Hero state card with color-coded background (heating/cooling/defrost/fault/idle)
- [x] Component dots card (Comp/Fan/Pump/Aux) with status indicators
- [x] Performance strip (COP/Power/Fan) with value-on-top layout
- [x] Expandable panels for Temperatures, Compressor, Energy
- [x] Fixed footer nav buttons (scroll-independent)
- [x] Drop setpoint delta (no demand register in ECO-600 protocol)
- [ ] Fine-tune hero card sizing and spacing after hardware testing
- [ ] Consider adding tank setpoint to hero card if available

## API Enhancements

- [x] Add screenshot endpoint (`GET /api/screenshot`) — returns uncompressed PNG (DEFLATE stored blocks, ~2.77 MB for 720×1280) with minimal CPU overhead. Streamed via HTTP chunked transfer, no extra heap allocation beyond the framebuffer capture.

## Logging

- [ ] Audit and clean up serial log output (remove excessive/noisy ESP_LOGI/LOGD calls, standardize TAG usage)
- [x] Implement a circular RAM log buffer (256-entry ring buffer in `log_buffer.cpp`)
- [x] Add `GET /api/logs` endpoint to retrieve buffered logs as JSON (with `since`, `level`, `limit` params)
- [x] Add `DELETE /api/logs` endpoint to clear the log buffer
- [x] Add a live log viewer panel to the web dashboard (auto-scroll, filterable by level, 2s polling)
- [x] Hook into `esp_log_set_vprintf()` to capture logs into the ring buffer alongside serial output

## OTA Hardening

- [x] Enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` — bootloader reverts to
      previous partition if new firmware fails to call `mark_valid()`
- [x] Defer `ota_mgr_mark_valid()` to after `create_ui()` — proves display,
      LVGL, and UI stack are functional before committing to new firmware
- [x] Add `ota_mgr_is_pending_verify()` query function
- [ ] Expose `is_pending_verify` in `/api/ota/status` response for test automation
- [ ] Add Tier 1 OTA safety tests (URL allowlist, concurrent prevention, bad upload)
- [ ] Add Tier 2 real OTA round-trip test (`@pytest.mark.destructive`)
- [ ] Add Tier 3 rollback validation test (crash-before-mark-valid)
- [ ] Consider adding a watchdog timer that reboots if firmware hangs before `mark_valid()`

## Testing

> **Test backlog and coverage details have moved to [`tests/device/TODO.md`](tests/device/TODO.md).**
> See [`tests/device/README.md`](tests/device/README.md) for architecture,
> setup instructions, and how to write new tests.

## Cloud Telemetry (Azure)

Send heat pump operational data to Azure for long-term monitoring, analytics,
and alerting. The device already has all sensor data via Modbus — this adds
cloud connectivity to enable remote dashboards, historical trends, and
anomaly detection.

### Phase 1 — Device to Cloud

- [ ] **Azure IoT Hub integration** — register device, authenticate via SAS token
      or X.509 cert, send telemetry via MQTT (ESP-IDF `esp-mqtt` or Azure IoT SDK)
- [ ] **Telemetry payload** — periodic messages (every 30–60s) with: temperatures
      (water tank, outdoor, discharge, suction, condenser, evaporator), compressor
      frequency, fan speed, power state, working mode, error codes, COP metrics
- [ ] **Connection management** — reconnect on WiFi drop, buffer unsent messages
      in PSRAM, backoff on repeated failures
- [ ] **Device twin / desired properties** — allow cloud-side configuration of
      poll interval, telemetry fields, alert thresholds
- [ ] **Kconfig option** — `CONFIG_CLOUD_TELEMETRY` to enable/disable at build
      time (off by default, no impact on production binary size when disabled)

### Phase 2 — Cloud Backend

- [ ] **IoT Hub → Event Hubs / Stream Analytics** — route telemetry for processing
- [ ] **Azure Data Explorer or Cosmos DB** — store time-series data for historical
      queries (temperature trends, runtime hours, defrost cycles)
- [ ] **Azure Monitor / Alerts** — trigger alerts on error codes, unusual temps,
      compressor stalls, or extended defrost cycles
- [ ] **Power BI / Grafana dashboard** — visualize historical data, COP trends,
      energy consumption patterns

### Phase 3 — Cloud to Device

- [ ] **Remote commands** — change mode, setpoints, power on/off from cloud
      dashboard (via IoT Hub direct methods or cloud-to-device messages)
- [ ] **OTA from cloud** — trigger firmware updates via IoT Hub instead of
      direct API (useful for fleet management with multiple controllers)
- [ ] **Remote diagnostics** — pull device logs, capture screenshots, check
      health status from cloud portal
