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

## Event Log Enhancements

- [ ] Consider logging compressor frequency changes (e.g. significant jumps or thresholds)
- [ ] Consider logging fan speed changes (RPM thresholds or level transitions)

## Modbus Heat Pump Simulator (M5Stack AtomS3 Lite + RS485)

Standalone ESP32-S3 project that acts as a Modbus RTU slave, emulating the ECO-600 heat pump register map. Connects to the Tab5 controller via RS485 for end-to-end Modbus testing without a real heat pump.

### Hardware
- [ ] M5Stack AtomS3 Lite (ESP32-S3) + RS485 unit
- [ ] Wire RS485 A/B to Tab5 RS485 port (half-duplex, shared bus)

### Firmware
- [ ] New ESP-IDF project: `arctic-heatpump-simulator/`
- [ ] Modbus RTU slave, address 1, 2400 baud, 8E1 (matching `arctic_registers.h`)
- [ ] Holding register map: reuse `arctic_registers.h` definitions (registers 2000-2138)
- [ ] Respond to function code 0x03 (read holding registers) — return current register values
- [ ] Respond to function code 0x06 (write single register) — update register and log change
- [ ] All registers pre-populated with realistic defaults (same as `initDemoState()`)

### Simulation Features
- [ ] **Scenario presets**: idle, heating, cooling, defrost, fault — each preset sets a coherent combination of temps/status/errors
- [ ] **Auto-behavior**: when unit_on and mode set, slowly ramp temps toward setpoint, toggle compressor/fan/pump status bits, update frequency/RPM
- [ ] **Error injection**: cycle through error1/error2 bit patterns on command
- [ ] **Serial console menu**: switch presets, inject errors, adjust individual registers via UART commands
- [ ] **Temperature drift**: ambient/coil/discharge temps change gradually based on mode, making the controller's polling feel realistic

### Test Scenarios
- [ ] Controller connects and reads valid data → verify dashboard populates correctly
- [ ] Simulator injects P02 error → verify controller shows error on device + web
- [ ] Controller sends mode change (write reg 2001) → verify simulator receives and updates
- [ ] Controller sends setpoint change → verify simulator register updated
- [ ] Controller sends power off (write reg 2000 = 0) → verify simulator stops "running"
- [ ] Simulate communication failure (stop responding) → verify controller shows DISCONNECTED after timeout
- [ ] Defrost cycle simulation → verify controller detects state transition and logs events

## Main Screen Layout Redesign

- [x] Hero state card with color-coded background (heating/cooling/defrost/fault/idle)
- [x] Component dots card (Comp/Fan/Pump/Aux) with status indicators
- [x] Performance strip (COP/Power/Fan) with value-on-top layout
- [x] Expandable panels for Temperatures, Compressor, Energy
- [x] Fixed footer nav buttons (scroll-independent)
- [x] Drop setpoint delta (no demand register in ECO-600 protocol)
- [ ] Fine-tune hero card sizing and spacing after hardware testing
- [ ] Consider adding tank setpoint to hero card if available

## Logging

- [ ] Audit and clean up serial log output (remove excessive/noisy ESP_LOGI/LOGD calls, standardize TAG usage)
- [ ] Implement a circular RAM log buffer (e.g. 16–32 KB ring buffer) for recent log messages
- [ ] Add `GET /api/logs` endpoint to retrieve buffered logs as JSON or plain text
- [ ] Add a live log viewer panel to the web dashboard (auto-scroll, filterable by level)
- [ ] Hook into `esp_log_set_vprintf()` to capture logs into the ring buffer alongside serial output

## Testing

### CI / Automated Testing (no device required)
- [ ] **Unit tests**: Extract pure logic (event ring buffer, °C↔°F conversion, error lookups, demo register mapping, i18n string resolution, setpoint validation) into host-compilable modules; test with Google Test or Catch2 on GitHub Actions runner
- [ ] **API contract tests**: Build Python mock server (FastAPI) implementing the same REST API; validate against `openapi.yaml` with schemathesis; run pytest suite for all endpoints; add to CI workflow
- [ ] **OpenAPI spec validation**: Add spectral or openapi-generator lint step to CI to catch spec drift and malformed schemas
- [ ] **Web dashboard tests**: Run Playwright against mock server serving `index.html`; test page load, real-time updates, mode/setpoint controls, language switching, mobile viewport
- [ ] Add unit test and API contract test stages to `.github/workflows/build.yml`

### Device / Hardware Testing

#### Test Instrumentation Endpoints (behind `#ifdef CONFIG_TEST_ENDPOINTS`)
- [ ] `GET /api/test/ui-state` — Walk LVGL object tree, return all visible labels/buttons/states as JSON (type, text, visible, position)
- [ ] `POST /api/test/click` — Find widget by label text (`{"label": "Errors"}` or `{"label_contains": "P02"}`) and fire `lv_obj_send_event(LV_EVENT_CLICKED)` from LVGL thread; supports `user_data` tag matching for tagged widgets
- [ ] `GET /api/test/screenshot` — Capture framebuffer via `lv_snapshot_take()` and return as PNG
- [ ] Queue mechanism: HTTP handler posts action to LVGL task queue, LVGL tick processes it (same pattern as existing screen navigation callbacks)
- [ ] Add `CONFIG_TEST_ENDPOINTS` Kconfig option so endpoints are stripped from production builds
- [ ] Add test endpoints to OpenAPI spec (under a `Test` tag)

#### On-Device Test Runner (pytest on PC → device over HTTP)
- [ ] Test flow: boot device in demo mode with test endpoints → Python test runner injects state via demo API → clicks buttons by label → reads `ui-state` → asserts expected labels/values
- [ ] Example: inject P02 error → click "Errors" button → verify label containing "P02" appears on errors screen
- [ ] Example: switch language to French → navigate to dashboard → verify hero card labels are in French
- [ ] Example: set demo temps → navigate to dashboard → verify temperature labels show correct values

#### Manual Device Testing
- [ ] Verify all screens render correctly on 720×1280 display (Tab5)
- [ ] Test scrolling behavior with multiple expandable panels open
- [ ] Confirm fixed footer stays visible during scroll
- [ ] Test hero card color transitions between all states (heating → cooling → defrost → fault → idle → standby → disconnected)
- [ ] Verify component dots update in real-time when compressor/fan/pump status changes
- [ ] Test expandable panel toggle (open/close) responsiveness
- [ ] Verify frequency bar updates smoothly in compressor panel
- [ ] Test demo mode toggle: all fields injectable, UI reflects changes immediately
- [ ] Confirm startup animation → dashboard transition works cleanly
- [ ] Test memory usage: open/close screens repeatedly, check for LVGL object leaks
- [ ] Verify touch targets are large enough (especially expandable panel headers, nav buttons)

### REST API Testing
- [ ] `GET /api/heatpump` — verify all fields returned, correct types
- [ ] `GET /api/heatpump/status` — confirm status fields match device state
- [ ] `GET /api/heatpump/errors` — test with 0 errors, active errors, error history
- [ ] `PATCH /api/heatpump/demo` — inject all supported fields, verify UI updates
- [ ] `PATCH /api/heatpump/demo` — test error injection (`error1`, `error2`) and clearing
- [ ] `PATCH /api/heatpump/demo` — test status1 bit manipulation (compressor, fan, pump on/off)
- [ ] `POST /api/heatpump/mode` — test all working modes
- [ ] `POST /api/heatpump/setpoint` — test cooling/heating/hot water setpoints
- [ ] `POST /api/heatpump/power` — test power on/off
- [ ] Verify API auth (missing/wrong API key returns 401)
- [ ] Test API responses when device is disconnected (non-demo mode)
- [ ] Verify `demo_mode` flag is present in all API responses

### Web Dashboard Testing
- [ ] Verify web dashboard loads and connects via WebSocket
- [ ] Confirm real-time updates: temperatures, status, errors refresh live
- [ ] Test web dashboard in demo mode: all values update when injected via API
- [ ] Verify mode/setpoint controls work from web interface
- [ ] Test web dashboard on mobile browsers (responsive layout)
- [ ] Verify error display matches device error screen
- [ ] Test web dashboard reconnection after WiFi dropout

### Localization Testing
- [ ] Switch to French: verify all dashboard labels (hero card, perf strip, expandable panels, nav buttons)
- [ ] Switch to Spanish: verify all dashboard labels
- [ ] Verify settings menu labels translate (Demo Mode, Temperature, WiFi, etc.)
- [ ] Check French accented characters render correctly (É, è, à, ô in ENTRÉE, EXTÉRIEUR, Énergie, etc.)
- [ ] Check Spanish accented characters render correctly (ó, í, á in SUCCIÓN, Energía, POTENCIA, etc.)
- [ ] Verify temperature unit labels (°C/°F) display correctly in all languages
- [ ] Test language switch while on dashboard — labels should update on next screen entry
- [ ] Verify abbreviated French/Spanish labels fit within card columns without truncation
- [ ] Test error screen messages in all 3 languages
- [ ] Verify params/advanced screen parameter names in all languages

### Fahrenheit / Unit Conversion Testing
- [ ] Switch to °F: verify all temperature displays convert correctly
- [ ] Verify dashboard hero card tank temp shows °F
- [ ] Verify expandable temps panel (inlet/outlet/ambient/coil) shows °F
- [ ] Verify compressor panel discharge/suction temps show °F
- [ ] Verify setpoint editing works in °F (converts back to °C for Modbus write)
- [ ] Verify web dashboard respects unit preference
- [ ] Test switching units while on temperature screens — values should update immediately
- [ ] Verify ΔT values use correct Fahrenheit differential conversion

### Error Handling Testing
- [ ] Inject error1/error2 via demo API, verify error card turns red with count
- [ ] Tap error card: verify navigation to error details screen
- [ ] Verify error history shows timestamps and durations
- [ ] Clear error registers: verify card returns to green "No Errors"
- [ ] Test multiple simultaneous errors (both error1 and error2 active)
- [ ] Verify disconnected state: hero card shows DISCONNECTED, error card shows appropriate message
- [ ] Test error history clear button

### OTA / Firmware Testing
- [ ] Verify OTA update check works from settings menu
- [ ] Test OTA download and install process
- [ ] Confirm firmware version displays correctly on settings screen
- [ ] Verify device reboots cleanly after OTA update

### OTA Install Button — Future Test Options
The automated firmware tests verify version display, GitHub check completion,
update-available UI state, and Install button visibility — but intentionally
never *click* the Install button (it triggers a real OTA download + auto-reboot
with no confirmation dialog, which would kill the test session).

Options for future coverage:
- [ ] **Dry-run mode**: Add a `CONFIG_TEST_OTA_DRY_RUN` flag that replaces the
      real `ota_mgr_start_update()` with a fake task that simulates progress
      (0→100%) and sets state to `OTA_STATE_READY_TO_REBOOT` without actually
      flashing or rebooting. Tests could then verify the progress bar, download
      status text, and completion UI.
- [ ] **Destructive marker**: Mark a dedicated test with `@pytest.mark.destructive`
      that actually clicks Install, waits for the device to reboot (polling until
      it comes back online), and verifies the new version. Only run on demand
      (`pytest -m destructive`) against a throwaway device.
- [ ] **CI with hardware-in-the-loop**: Flash a known old version, run the
      destructive update test, verify the device comes back with the new version.
      Requires a dedicated test device on the CI network.

### WiFi Testing  
- [ ] Test WiFi scan and connect from settings
- [ ] Verify mDNS discovery (`arctic.local`)
- [ ] Test WiFi reconnection after signal loss
- [ ] Verify API server starts correctly after WiFi connection
- [ ] Test with ESP32-C6 WiFi module (hosted mode)
