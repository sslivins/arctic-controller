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

### Device / Hardware Testing
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

### WiFi Testing  
- [ ] Test WiFi scan and connect from settings
- [ ] Verify mDNS discovery (`arctic.local`)
- [ ] Test WiFi reconnection after signal loss
- [ ] Verify API server starts correctly after WiFi connection
- [ ] Test with ESP32-C6 WiFi module (hosted mode)
