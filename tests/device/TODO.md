# Device UI Tests — TODO

Test coverage backlog and infrastructure improvements for the Arctic Controller
device test suite. See [README.md](README.md) for architecture and usage.

## Coverage Gaps — Uncovered Screens & Features

The current 388-test suite covers: main screen, settings menu, navigation, WiFi
dialogs, firmware check, display brightness, time format, timezone, temperature
unit, language switching, localization (FR/ES), demo mode, status bar/notifications,
error mapping, error history duration, screenshot API, heat pump sub-screens
(temps, system, control, errors, event log), screen render performance,
REST API functional tests (heatpump status/control/demo/params/errors, logs,
auth, events, preferences), and API contract validation.

The following screens and features have **no automated test coverage** yet:

- [x] **Event log screen** — 10 tests in `test_event_log_screen.py`: navigation,
      title, clear button, empty state, system start event, events via API, clear via API.
- [x] **Heatpump control screen** — 19 tests in `test_control_screen.py`: power ON/OFF,
      5 mode buttons, active mode, 3 setpoint labels + values, P-parameter visibility.
- [x] **Heatpump system screen** — 31 tests in `test_system_screen.py`: temps, flows,
      component states, section headers, navigation.
- [x] **Heatpump temps screen** — 22 tests in `test_temps_screen.py`: all temperature
      rows, labels, values, navigation.
- [x] **Heatpump errors screen** — 12 tests in `test_errors_screen.py`: active/cleared
      errors, descriptions, clear history, errors API.
- [ ] **OTA update flow UI** — firmware *check* is tested (`test_firmware.py`) but not
      the download progress, status text, or completion UI. See OTA Install Button
      section below for options.
- [ ] **Startup animation** — transient and hard to test; low priority.
- [ ] **WiFi connection flow** — password dialog and mock networks are tested, but
      actual connect/disconnect/reconnect behavior is not (requires real network changes).

## REST API Testing

- [x] `GET /api/heatpump` — verify all fields returned, correct types
- [x] `GET /api/heatpump/status` — confirm status fields match device state
- [x] `GET /api/heatpump/errors` — test with 0 errors, active errors, error history
- [x] `PATCH /api/heatpump/demo` — inject all supported fields, verify UI updates
- [x] `PATCH /api/heatpump/demo` — test error injection (`error1`, `error2`) and clearing
- [x] `PATCH /api/heatpump/demo` — test status1 bit manipulation (compressor, fan, pump on/off)
- [x] `POST /api/heatpump/mode` — test all working modes
- [x] `POST /api/heatpump/setpoint` — test cooling/heating/hot water setpoints
- [x] `POST /api/heatpump/power` — test power on/off
- [x] Verify API auth (missing/wrong API key returns 401)
- [ ] Test API responses when device is disconnected (non-demo mode)
- [x] Verify `demo_mode` flag is present in all API responses
- [x] `GET /api/logs` — verify entries returned with seq, level, tag, message fields
- [x] `GET /api/logs?level=E` — verify only error-level entries returned
- [x] `GET /api/logs?since=N` — verify incremental fetch returns only newer entries
- [x] `GET /api/logs?limit=10` — verify at most 10 entries returned
- [x] `DELETE /api/logs` — verify buffer is cleared, seq numbers keep incrementing
- [x] `GET /api/time/config` — verify timezone, format_24h, synced fields
- [x] `POST /api/time/config` — round-trip set/restore timezone and format_24h
- [x] `GET /api/ota/status` — verify idle state, progress, version, download fields
- [x] `POST /api/time/sync` — verify NTP sync trigger returns success

## Web Dashboard Testing

- [x] Verify web dashboard loads and connects via WebSocket
- [x] Confirm real-time updates: temperatures, status, errors refresh live
- [x] Test web dashboard in demo mode: all values update when injected via API
- [x] Verify mode/setpoint controls work from web interface
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

- [x] Switch to °F: verify all temperature displays convert correctly (test_fahrenheit.py)
- [x] Verify dashboard hero card tank temp shows °F (test_fahrenheit.py::TestHeroTankFahrenheit)
- [ ] Verify expandable temps panel (inlet/outlet/ambient/coil) shows °F (panels hidden when collapsed — needs expand API)
- [ ] Verify compressor panel discharge/suction temps show °F (panels hidden when collapsed — needs expand API)
- [ ] Verify setpoint editing works in °F (converts back to °C for Macon write)
- [ ] Verify web dashboard respects unit preference (web dashboard hardcodes °C — feature gap)
- [ ] Test switching units while on temperature screens — values should update immediately
- [ ] Verify ΔT values use correct Fahrenheit differential conversion
- [x] Verify temps sub-screen shows all 9 values in °F (test_fahrenheit.py::TestTempsScreenFahrenheit)
- [x] Verify no °C symbols appear when in °F mode (test_fahrenheit.py::test_no_celsius_symbol_in_f_mode)
- [x] Verify preferences API reports correct unit (test_fahrenheit.py::TestPreferencesUnitAPI)
- [x] Verify C→F→C round-trip math (test_fahrenheit.py::TestConversionMath)

## Error Handling Testing (expanded)

- [ ] Inject error1/error2 via demo API, verify error card turns red with count
- [ ] Tap error card: verify navigation to error details screen
- [ ] Verify error history shows timestamps and durations
- [ ] Clear error registers: verify card returns to green "No Errors"
- [ ] Test multiple simultaneous errors (both error1 and error2 active)
- [ ] Verify disconnected state: hero card shows DISCONNECTED, error card shows appropriate message
- [ ] Test error history clear button

## OTA / Firmware Testing (expanded)

### Current state

Bootloader rollback is enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).
After an OTA update, the new firmware must call `ota_mgr_mark_valid()` (which
happens after `create_ui()` succeeds) before the next reboot, or the bootloader
reverts to the previous partition. This is a one-shot mechanism — if `mark_valid()`
is called and the firmware crash-loops later, no automatic rollback occurs.

### Tier 1 — Safe tests (no reboot, no real OTA)

These can run in the normal test suite without risk:

- [x] **URL allowlist enforcement** — `POST /api/ota/update` with non-GitHub,
      HTTP, wrong owner/repo, bare domain, empty URL → 403 rejection (9 tests)
- [x] **Concurrent OTA prevention** — start a URL download to a nonexistent
      GitHub URL, then attempt upload → 409 (1 test)
- [x] **Upload with bad data** — random bytes, empty body, truncated header,
      garbage body, JSON content type → rejection without bricking (6 tests)
- [x] **Status schema validation** — verify all required fields, types, enum
      values, idle baseline (zero counters, no error/new_version) (7 tests)
- [x] **Version reporting** — verify `current_version` matches `CMakeLists.txt`,
      semver format (2 tests)
- [x] **Auth enforcement** — all 6 OTA endpoints reject invalid API key (6 tests)
- [x] **Releases endpoint** — field presence, version consistency (2 tests)
- [x] **GitHub update precondition** — 400 without prior release check (1 test)
- [x] **Error state** — failed download populates error field (1 test)
- [x] **pending_verify field** — boolean presence and value checks (2 tests,
      skip on firmware without ota-hardening)
- [ ] Verify OTA update check works from settings menu (UI test)
- [ ] Confirm firmware version displays correctly on settings screen (UI test)

### OTA via Device UI (Tier 1 — safe, no real update)

The device screen tests (`test_firmware.py`) cover check/mock but not the full
update flow UI. These tests use `firmware_mock()` to simulate update states
without triggering a real OTA.

- [ ] **Progress bar display** — mock OTA state to DOWNLOADING with progress 0→50→100,
      verify progress bar widget appears and updates
- [ ] **Status text updates** — verify status label changes through states:
      "Checking...", "Downloading...", "Verifying...", "Ready to reboot"
- [ ] **Error state UI** — mock a failed download, verify error message displays
      on the update screen
- [ ] **Install button behavior** — when update is available (mocked), verify
      install button is visible and tappable (but don't actually trigger OTA)
- [ ] **Cancel/dismiss during update check** — verify user can navigate away
      from the update screen during a check

### OTA via Web Dashboard (Tier 1 — safe, no real update)

The web tests (`test_settings.py`) verify UI elements exist but don't test the
OTA flow. These tests use API mocking to simulate update states.

- [ ] **Check for Updates button** — click button, verify spinner/loading state
      appears, then result (no update / update available)
- [ ] **Update available card** — mock `GET /api/ota/releases` to return a newer
      version, click check, verify "Update Available" banner with version + release
      notes appears
- [ ] **Install Update button** — when update available, verify install button
      appears and is clickable (intercept the POST to avoid real install)
- [ ] **File upload via drag-and-drop zone** — upload a small invalid .bin file,
      verify error message appears (header validation rejects it)
- [ ] **File upload via file picker** — use the hidden file input to select a
      .bin file, verify upload starts (intercept to avoid real flash)
- [ ] **Upload progress display** — verify progress bar appears during file upload
- [ ] **OTA status polling** — verify the web dashboard polls `/api/ota/status`
      and reflects state changes (idle → downloading → complete/failed)

### Cross-path OTA consistency

Verify that OTA state is consistent across all three interfaces.

- [ ] **API-triggered update visible on web** — start OTA via API, verify web
      dashboard shows download progress
- [ ] **API-triggered update visible on device UI** — start OTA via API, verify
      device screen shows update status
- [ ] **Web-triggered update visible via API** — start OTA from web dashboard,
      verify `/api/ota/status` reflects progress

### Tier 2 — Real OTA (requires reboot)

These tests trigger an actual firmware update. The device reboots, so the test
must wait for it to come back online. Mark with `@pytest.mark.destructive`.

- [ ] **Same-version OTA round-trip** — upload the current firmware binary via
      `POST /api/ota/upload`, let it flash, wait for reboot, verify device
      returns to idle with same version on the alternate partition
- [ ] **GitHub release OTA** — trigger `POST /api/ota/github` with the current
      release, wait for download + reboot, verify device comes back healthy
- [ ] **Verify `mark_valid()` fires** — after OTA reboot, check that the device
      is NOT in `PENDING_VERIFY` state (query new `/api/ota/status` field or
      check serial log for "Firmware validated")

### Tier 3 — Rollback (complex, manual or CI-only)

These validate that a bad firmware gets reverted by the bootloader.

- [ ] **Crash-before-mark-valid** — build a firmware that crashes in `create_ui()`
      (before `mark_valid()`). Flash via OTA. Verify the bootloader reverts to the
      previous working partition after reboot. This proves the rollback mechanism.
- [ ] **Hang-before-mark-valid** — similar, but firmware hangs instead of crashing.
      Requires watchdog to trigger reboot.

### Future enhancements

- [ ] **Dry-run mode**: Add a `CONFIG_TEST_OTA_DRY_RUN` flag that replaces the
      real `ota_mgr_start_update()` with a fake task that simulates progress
      (0→100%) and sets state to `OTA_STATE_READY_TO_REBOOT` without actually
      flashing or rebooting. Tests could then verify the progress bar, download
      status text, and completion UI.
- [ ] **CI with hardware-in-the-loop**: Flash a known old version, run the
      destructive update test, verify the device comes back with the new version.
      Requires a dedicated test device on the CI network.
- [x] **`is_pending_verify` API field** — exposed `ota_mgr_is_pending_verify()`
      in `/api/ota/status` response; tested in `test_ota_api.py`.

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
- [x] Test demo mode toggle: reboot confirmation panel appears, Cancel reverts preference
- [ ] Confirm startup animation → dashboard transition works cleanly
- [ ] Test memory usage: open/close screens repeatedly, check for LVGL object leaks
- [ ] Verify touch targets are large enough (especially expandable panel headers, nav buttons)

## CI / Infrastructure Improvements

- [ ] **Test execution speed**: CI test suite takes ~45 min (267 device tests + web tests).
      Opportunities to reduce: batch Fahrenheit tests to minimize settings toggle round-trips,
      share fixture state across related tests, reduce `time.sleep()` guards where
      `wait_for_widget()` polling would suffice, parallelize API tests (no UI dependency).
- [ ] **Unit tests (no device)**: Extract pure logic (event ring buffer, °C↔°F conversion,
      error lookups, demo register mapping, i18n string resolution, setpoint validation)
      into host-compilable modules; test with Google Test or Catch2 on GitHub Actions runner
- [x] **API contract tests**: Schemathesis validates all safe GET endpoints against
      `docs/openapi.yaml`; targeted smoke tests verify key response values; integrated
      into `device-tests.yml` workflow as a separate step
- [ ] **OpenAPI spec validation**: Add spectral or openapi-generator lint step to CI to catch
      spec drift and malformed schemas
- [x] **Web dashboard tests**: 50 Playwright tests across 5 files (dashboard, navigation,
      login, i18n, settings) — page load, real-time updates, controls, language switching
- [ ] Add unit test and API contract test stages to `.github/workflows/build.yml`
- [ ] **Out-of-box / factory-reset testing**: Erase NVS before a test run to verify
      default credentials (`arctic`/`arctic`), default API key generation, default
      settings (language, timezone, temp unit, web auth). Ensures CI tests work on a
      clean device without pre-configured secrets or manual setup.
- [x] `GET /api/test/screenshot` — Capture framebuffer via `lv_snapshot_take()` and return
      as PNG (useful for visual regression and CI artifacts)

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
- [x] 243 tests across 21 files covering core screens and features
- [x] Heat pump sub-screen tests: temps (22), system (31), control (19), errors (12), event log (10)
- [x] Screen render performance tests (11): 300 ms budget for all transitions, heavy state, leak detection
- [x] Lightweight `/api/test/screen` endpoint for fast screen detection
- [x] Iterative widget tree walk with PSRAM buffer (handles 100+ widget screens)
- [x] REST API functional tests — 254 tests across 7 files covering heatpump
      status/control/demo/params/errors/status1-bits, logs, auth (config,
      login/logout, API key, sessions), events, health, time config, OTA
      (status schema, auth, URL allowlist, bad uploads, releases, error state),
      WiFi, info, display brightness, preferences
- [x] Session lifecycle tests — 23 tests covering login/logout, concurrent
      sessions (max 4), credential changes, auth toggle, API key via session
- [x] Web dashboard tests — 53 Playwright tests across 6 files (dashboard,
      navigation, login, i18n, settings, password change) in `tests/web/`