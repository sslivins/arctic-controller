# Arctic Controller – TODO

## Macon Protocol Opacity (completed 2026-08)

The controller (`main/`) is now opaque to the Tuya/Macon wire protocol and
register map — all wire/register/fault/scaling/bus knowledge lives behind
`arctic-macon`. Enforced by `tests/api/test_opaque_macon_controller.py` (7/7).

- [x] Register-map header, `REG_*`, OEM fault codes, and register numbers scrubbed
      from controller code/comments/strings (#119)
- [x] WorkingMode write-path uses an explicit opaque map, not raw wire casts (#120)
- [x] Advanced-param wire metadata quarantined behind a library opaque-option-id
      API; `advanced_params.{cpp,h}` is the sole id↔wire wrapper (#118)
- [x] Production diagnostic endpoints (`/raw`, `/windows`, `/diagnostic` CSV)
      register addresses compile-gated behind `CONFIG_TEST_ENDPOINTS`; the
      register-map headers are no longer compiled into a production build (#121, #129)
- [x] Tuya master/listener ingest relocated into `arctic-macon`; gate's
      `main/tuya/` exclusion dropped (#123)
- [x] Fan tier derived from library-provided full-scale instead of magic
      thresholds (#122 — opacity portion; real fan-max calibration still pending)
- [x] UART bus parameters (4800 8-E-1) owned by the library as data
      (`MACON_BUS_PARAMS`); transports consume `arctic::macon_uart_config()` (#129)

## COP / Energy Monitoring

- [ ] Add configurable water flow rate setting (default ~20 L/min, user adjusts to match their circulator pump)
- [ ] Store flow rate in NVS so it persists across reboots
- [ ] Start polling saturation temp registers (2111-2113) — currently skipped in `pollTemperatures()`
- [ ] Calculate thermal output: `flow_rate (L/s) × 4186 (J/kg·K) × ΔT (outlet - inlet)`
- [ ] Calculate COP: `thermal_output_W / electrical_input_W` (only when compressor running)
- [ ] Expose COP, thermal output, electrical input in `HeatPumpState` struct
- [ ] Add COP/energy fields to REST API and demo mode injection
- [ ] Add flow rate configuration to the Advanced/Params screen

## Tuya Active-Master Mode (`CONFIG_ARCTIC_TUYA_MASTER`)

Active bus-master control landed 2026-08 (poll loop + `MaconLink` setpoint
writes over an RS485 half-duplex transport). Follow-ups:

- [ ] **Hardware validation**: with the OEM controller disconnected, confirm
      the bus-idle preflight passes, telemetry populates, and a cooling/hot-water
      setpoint write is ACKed by the unit. Watch for RS485 turnaround/echo issues
      on a logic analyzer.
- [ ] **Move the streaming block-read into `arctic-macon`** (`MaconLink`): the
      poll loop in `macon_master.cpp` duplicates the frame-accumulate/match logic
      that already exists privately in `MaconLink::read_matching_frame`. Expose a
      generic `read_registers(addr, count, out, …)` so the firmware transport
      only owns UART/DIR behaviour.
- [ ] Capture OEM traffic across cold-boot / steady-state / setpoint change /
      fault / defrost to confirm periodic fc=0x03 polling fully replaces the OEM
      controller (no missed handshake / keepalive / unsolicited frames).
- [ ] Verify `reg2094` before adding `set_heating_setpoint` (currently
      unsupported in master mode).

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
and alerting. The device already has sensor data from the Macon bus — this adds
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

## Networking / DNS

- [ ] **Dynamic DNS for device hostnames** — `arctic-0001.mennlabs.com` currently
      has a static A record pointing to the device's LAN IP. If the IP changes
      (DHCP lease expiry, router reset), the hostname goes stale. Options:
  - DHCP reservation on the router (simplest, no code changes)
  - Device-side DDNS updater: call Cloudflare API after WiFi connect to update
    the A record automatically (~50 lines C++, needs `CLOUDFLARE_API_TOKEN` + zone ID)
  - Could reuse the same Cloudflare token already stored for cert renewal
