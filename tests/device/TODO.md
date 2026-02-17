# Device UI Tests — TODO

Test coverage backlog and infrastructure improvements for the Arctic Controller
device test suite. See [README.md](README.md) for architecture and usage.

## Coverage Gaps — Uncovered Screens & Features

The current 129-test suite covers: main screen, settings menu, navigation, WiFi
dialogs, firmware check, display brightness, time format, timezone, temperature
unit, language switching, localization (FR/ES), demo mode, status bar/notifications,
error mapping, and error history duration.

The following screens and features have **no automated test coverage** yet:

- [ ] **Event log screen** — `event_log_screen.cpp` exists but has zero tests.
      Test ideas: navigate to event log, verify events appear after state changes,
      verify event timestamps, test clearing events, test scrolling with many entries.
- [ ] **Heatpump control screen** — power/mode/setpoint control UI untested.
      Needs: navigate to control screen, toggle power, change mode, adjust setpoints,
      verify UI reflects changes.
- [ ] **Heatpump params screen** — parameter display/editing untested.
      Needs: navigate to params, verify parameter values display, test editing a param.
- [ ] **Heatpump system screen** — system info display untested.
      Needs: navigate to system screen, verify labels and values render.
- [ ] **Heatpump temps screen** — temperature detail view untested.
      Needs: navigate to temps screen, verify all temperature labels and values.
- [ ] **Heatpump errors screen** — dedicated errors screen not tested (only the error
      card on the main screen is covered by `test_error_mapping.py`).
      Needs: navigate to errors screen, verify error list, test with 0/1/many errors.
- [ ] **OTA update flow UI** — firmware *check* is tested (`test_firmware.py`) but not
      the download progress, status text, or completion UI. See OTA Install Button
      section below for options.
- [ ] **Startup animation** — transient and hard to test; low priority.
- [ ] **WiFi connection flow** — password dialog and mock networks are tested, but
      actual connect/disconnect/reconnect behavior is not (requires real network changes).

## REST API Testing

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
- [ ] `GET /api/logs` — verify entries returned with seq, level, tag, message fields
- [ ] `GET /api/logs?level=E` — verify only error-level entries returned
- [ ] `GET /api/logs?since=N` — verify incremental fetch returns only newer entries
- [ ] `GET /api/logs?limit=10` — verify at most 10 entries returned
- [ ] `DELETE /api/logs` — verify buffer is cleared, seq numbers keep incrementing

## Web Dashboard Testing

- [ ] Verify web dashboard loads and connects via WebSocket
- [ ] Confirm real-time updates: temperatures, status, errors refresh live
- [ ] Test web dashboard in demo mode: all values update when injected via API
- [ ] Verify mode/setpoint controls work from web interface
- [ ] Test web dashboard on mobile browsers (responsive layout)
- [ ] Verify error display matches device error screen
- [ ] Test web dashboard reconnection after WiFi dropout

## Localization Testing (expanded)

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

## Fahrenheit / Unit Conversion Testing (expanded)

- [ ] Switch to °F: verify all temperature displays convert correctly
- [ ] Verify dashboard hero card tank temp shows °F
- [ ] Verify expandable temps panel (inlet/outlet/ambient/coil) shows °F
- [ ] Verify compressor panel discharge/suction temps show °F
- [ ] Verify setpoint editing works in °F (converts back to °C for Modbus write)
- [ ] Verify web dashboard respects unit preference
- [ ] Test switching units while on temperature screens — values should update immediately
- [ ] Verify ΔT values use correct Fahrenheit differential conversion

## Error Handling Testing (expanded)

- [ ] Inject error1/error2 via demo API, verify error card turns red with count
- [ ] Tap error card: verify navigation to error details screen
- [ ] Verify error history shows timestamps and durations
- [ ] Clear error registers: verify card returns to green "No Errors"
- [ ] Test multiple simultaneous errors (both error1 and error2 active)
- [ ] Verify disconnected state: hero card shows DISCONNECTED, error card shows appropriate message
- [ ] Test error history clear button

## OTA / Firmware Testing (expanded)

- [ ] Verify OTA update check works from settings menu
- [ ] Test OTA download and install process
- [ ] Confirm firmware version displays correctly on settings screen
- [ ] Verify device reboots cleanly after OTA update

### OTA Install Button — Future Options

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

## WiFi Testing (expanded)

- [ ] Test WiFi scan and connect from settings
- [ ] Verify mDNS discovery (`arctic.local`)
- [ ] Test WiFi reconnection after signal loss
- [ ] Verify API server starts correctly after WiFi connection
- [ ] Test with ESP32-C6 WiFi module (hosted mode)

## Manual Device Testing

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

## CI / Infrastructure Improvements

- [ ] **Unit tests (no device)**: Extract pure logic (event ring buffer, °C↔°F conversion,
      error lookups, demo register mapping, i18n string resolution, setpoint validation)
      into host-compilable modules; test with Google Test or Catch2 on GitHub Actions runner
- [ ] **API contract tests**: Build Python mock server (FastAPI) implementing the same REST
      API; validate against `openapi.yaml` with schemathesis; run pytest suite for all
      endpoints; add to CI workflow
- [ ] **OpenAPI spec validation**: Add spectral or openapi-generator lint step to CI to catch
      spec drift and malformed schemas
- [ ] **Web dashboard tests**: Run Playwright against mock server serving `index.html`; test
      page load, real-time updates, mode/setpoint controls, language switching, mobile viewport
- [ ] Add unit test and API contract test stages to `.github/workflows/build.yml`
- [ ] `GET /api/test/screenshot` — Capture framebuffer via `lv_snapshot_take()` and return
      as PNG (useful for visual regression and CI artifacts)

## End-to-End Tests with Modbus Simulator

Once the Modbus simulator hardware is set up (see main `todo.md`), add integration
tests that drive both the simulator and the Tab5 to verify real RS485 communication:

- [ ] Controller connects and reads valid data → verify dashboard populates correctly
- [ ] Simulator injects error → verify Tab5 error card shows correct code
- [ ] Iterate all 32 error codes over real RS485 (parameterized, mirrors `test_error_mapping.py`)
- [ ] Controller sends mode/setpoint/power changes → verify simulator register updated
- [ ] Simulate communication failure → verify controller shows DISCONNECTED after timeout
- [ ] Restore communication → verify controller reconnects and resumes polling
- [ ] Defrost cycle scenario → verify controller detects state transition
- [ ] Temperature ramp scenario → verify dashboard temps update progressively

## Done

Items completed in this branch (for reference):

- [x] CI pipeline — build + flash + test on self-hosted Pi Zero 2 W runner
- [x] `.github/workflows/device-tests.yml` with build/flash/test jobs
- [x] Concurrency protection (workflow-level + device-side session lock)
- [x] Path filtering — doc-only changes don't trigger builds
- [x] `CONFIG_TEST_ENDPOINTS` Kconfig option to strip test endpoints from production
- [x] 17 test instrumentation endpoints in `test_endpoints.cpp`
- [x] `DeviceClient` Python HTTP client wrapping all endpoints
- [x] Session lock protocol with TTL auto-expiry
- [x] `conftest.py` with auto-return-to-main and lock management
- [x] OpenAPI 3.0 spec for test API (`openapi-test.yaml`)
- [x] 129 tests across 15 files covering core screens and features
