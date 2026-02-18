# Device UI Tests

Automated integration tests for the Arctic Controller (ESP32-P4 / M5Stack Tab5).
Tests run over HTTP against a real device, driving the LVGL UI through a
test-only instrumentation API and asserting on widget state.

## Architecture

```
┌─────────────┐   HTTP   ┌──────────────────┐
│  pytest      │ ──────── │  Tab5 device     │
│  (host/CI)   │          │  (ESP32-P4)      │
│              │          │                  │
│  DeviceClient│ ───────► │  /api/test/*     │
│              │          │  (18 endpoints)  │
│              │ ◄─────── │                  │
│  assertions  │   JSON   │  LVGL UI engine  │
└─────────────┘          └──────────────────┘
```

**Key design decisions:**

- Tests never touch LVGL directly — all interaction goes through HTTP endpoints
  compiled into the firmware with `CONFIG_TEST_ENDPOINTS=y`.
- The test API is stripped from production builds (`CONFIG_TEST_ENDPOINTS=n`).
- A session lock mechanism prevents concurrent test runs from colliding on the
  shared physical device.
- Each test starts from the main screen (enforced by the `ensure_main_screen`
  autouse fixture).

## Prerequisites

### Device Setup
- M5Stack Tab5 (ESP32-P4) on the same network as the test runner
- Firmware built with `CONFIG_TEST_ENDPOINTS=y`
- Demo mode enabled (the conftest fixture does this automatically)

### Host Setup
```bash
pip install requests pytest schemathesis
```

### Environment Variable
```bash
# Default: http://arctic.local (mDNS)
export ARCTIC_URL=http://192.168.1.23    # use IP if mDNS is unreliable
```

## Running Tests

### Locally
```bash
cd tests/device
ARCTIC_URL=http://192.168.1.23 python -m pytest . -v
```

### Specific file or test
```bash
python -m pytest test_main_screen.py -v
python -m pytest test_firmware.py::test_current_version_displayed -v
```

### CI (GitHub Actions)
Tests run automatically on PRs that change firmware or test source code.
The pipeline:

1. **Build** (ubuntu-latest) — compiles firmware with `CONFIG_TEST_ENDPOINTS=y`
   using `espressif/esp-idf-ci-action`
2. **Flash** (self-hosted Pi Zero 2 W, label `tab5`) — uploads firmware via OTA
   (falls back to USB serial on failure)
3. **Test** — runs `pytest tests/device/ -v` against the freshly flashed device

The workflow uses `concurrency: group: device-tests` to serialize runs —
only one session can use the physical device at a time.

See `.github/workflows/device-tests.yml` for the full workflow.

## API Contract Tests

Separate from the UI-driven tests, `test_api_schema.py` validates that the
production REST API conforms to the OpenAPI spec in `docs/openapi.yaml`.

**How it works:**

- **Schemathesis** loads the spec and generates HTTP requests for every safe
  GET endpoint, validating that responses match the documented schema (status
  codes, field types, required properties, enum values).
- **Targeted smoke tests** verify specific response values beyond schema shape
  (e.g., `status == "ok"`, `platform == "ESP32-P4"`, `progress == 0`).
- **Dangerous endpoints** (OTA, reboot, credential changes) and all mutating
  methods (POST/PUT/DELETE) are skipped to protect the device.

**Running locally:**
```bash
pytest tests/api/ -v
```

**In CI**, API contract tests run as a separate step after UI tests,
with their own JUnit report.

## Test API Reference

All test endpoints are documented in the OpenAPI 3.0 spec:

- **Raw spec**: [`openapi-test.yaml`](openapi-test.yaml)
- **Interactive viewer**: [View in Swagger UI](https://petstore.swagger.io/?url=https://raw.githubusercontent.com/sslivins/arctic-controller/main/tests/device/openapi-test.yaml)

### Endpoint Categories

| Category | Endpoints | Purpose |
|----------|-----------|---------|
| **UI State** | `GET /api/test/ui-state` | Read the LVGL widget tree |
| **Interaction** | `click`, `set-slider`, `set-roller`, `toggle`, `type-text` | Drive UI interactions |
| **Mocking** | `wifi-mock`, `firmware-mock`, `notification-mock`, `set-demo-field` | Inject fake state |
| **Cleanup** | `*-mock-reset`, `clear-error-history` | Reset mocked state |
| **Session Lock** | `lock`, `unlock` (GET + POST) | Exclusive device access |
| **Screenshot** | `GET /api/test/screenshot` | Capture display as PNG |

### Session Lock Protocol

The device supports a single exclusive lock to prevent test session collisions:

1. `POST /api/test/lock` with `{"session_id": "<uuid>", "ttl_seconds": 900}`
2. All mutating endpoints require `X-Session-Id: <uuid>` header
3. Mismatched sessions get `423 Locked` responses
4. Lock auto-expires after TTL (handles test runner crashes)
5. `POST /api/test/unlock` with `{"force": true}` for manual cleanup

The `conftest.py` session fixture handles this automatically.

## File Organization

### Infrastructure

| File | Purpose |
|------|---------|
| [`conftest.py`](conftest.py) | Session fixture (device client, lock, demo mode, auto-return to main screen, screenshot on failure) |
| [`device_client.py`](device_client.py) | Python HTTP client wrapping all 18 test endpoints + production API |
| [`openapi-test.yaml`](openapi-test.yaml) | OpenAPI 3.0 spec for the test instrumentation API |

### Test Files (129 UI tests + 9 screenshot API tests + API contract tests)

| File | Tests | Description |
|------|-------|-------------|
| [`test_main_screen.py`](test_main_screen.py) | 20 | Hero card states, tank temp, component dots, performance strip, error card |
| [`test_error_mapping.py`](test_error_mapping.py) | 36 | All 32 error1/error2 bits → correct code + description on UI |
| [`test_localization.py`](test_localization.py) | 18 | French and Spanish translations on main screen |
| [`test_navigation.py`](test_navigation.py) | 10 | Settings sub-screen navigation and back buttons |
| [`test_language.py`](test_language.py) | 7 | Language switching via roller + preferences API |
| [`test_status_bar.py`](test_status_bar.py) | 6 | WiFi icon, notification badge, dropdown, firmware notification |
| [`test_firmware.py`](test_firmware.py) | 5 | Version display, GitHub check, mock update states |
| [`test_timezone.py`](test_timezone.py) | 5 | Timezone roller, time preview, preferences |
| [`test_time_format.py`](test_time_format.py) | 5 | 12h/24h toggle, status bar display |
| [`test_wifi.py`](test_wifi.py) | 4 | Password dialog, show/hide toggle, open network bypass |
| [`test_demo_mode.py`](test_demo_mode.py) | 4 | Demo mode toggle, banner show/hide |
| [`test_settings_menu.py`](test_settings_menu.py) | 3 | Open/close settings, close button |
| [`test_display_brightness.py`](test_display_brightness.py) | 3 | Brightness slider control and label |
| [`test_temperature_unit.py`](test_temperature_unit.py) | 2 | °C/°F toggle and preferences |
| [`test_error_history_duration.py`](test_error_history_duration.py) | 1 | Error history duration format ("3s") |
| [`test_screenshot_api.py`](test_screenshot_api.py) | 9 | Production screenshot endpoint: PNG validity, dimensions, auth |
| [`../api/test_api_schema.py`](../api/test_api_schema.py) | 8+ | API contract validation via Schemathesis + targeted smoke tests |

### DeviceClient Methods

The `DeviceClient` class provides typed wrappers for all device communication:

**UI inspection**: `get_ui_state()`, `screen`, `widgets`, `find_widget()`, `has_widget()`

**UI interaction**: `click()`, `set_slider()`, `set_roller()`, `toggle()`, `type_text()`

**Mocking**: `wifi_mock()`, `firmware_mock()`, `notification_mock()`, `set_demo_fields()`,
`clear_error_history()`, and corresponding `*_reset()` methods

**Polling**: `wait_for_screen()`, `wait_for_widget()` — poll with configurable timeout

**Session lock**: `lock()`, `unlock()`, `check_lock()`

**Screenshot**: `screenshot(path)` — capture display as PNG, save to file

**Production API**: `get_brightness()`, `get_preferences()`, `get_heatpump_status()`

## Writing New Tests

### Basic pattern

```python
def test_something(device):
    # 1. Navigate to the screen you're testing
    device.click(tag="settings")
    device.wait_for_screen("settings", timeout=5.0)

    # 2. Perform actions
    device.click(tag="wifi_row")
    device.wait_for_screen("wifi", timeout=5.0)

    # 3. Assert on widget state
    assert device.has_widget(tag="wifi_scan_list")

    # No cleanup needed — ensure_main_screen fixture resets after each test
```

### Tips

- **Use tags over labels** — tags are language-independent and don't break when
  translations change
- **Use `wait_for_screen()` and `wait_for_widget()`** — LVGL animations need
  time; never assert immediately after a click
- **Use `time.sleep(0.3–0.5)`** after toggles/rollers — LVGL processes events
  on the next tick
- **Mock, don't connect** — use `wifi_mock()`, `firmware_mock()`, etc. to inject
  controlled state rather than depending on real network services
- **Restore state** — if your test changes a persistent setting (language,
  timezone, temperature unit), restore it at the end
- **Demo mode fields** — use `set_demo_fields()` to inject temperatures, status
  bits, error codes; the conftest ensures demo mode is enabled

### Adding a new mock endpoint

1. Add the handler in `main/test_endpoints.cpp` with `CHECK_SESSION_LOCK(req)`
2. Register it in `register_test_endpoints()` (watch `max_uri_handlers` in
   `api_server.cpp` — currently 83)
3. Add a client method in `device_client.py`
4. Document in `openapi-test.yaml`
5. Write tests
