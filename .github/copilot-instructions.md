# Copilot Instructions — Arctic Controller

## Conversation Preferences

- **Do not use popup selection dialogs** (the `ask_questions` tool). Ask questions
  directly in the chat and wait for a response. The user prefers inline conversation.
- Be direct and concise. Skip preamble like "I'll now…" — just do the work.
- When multiple approaches exist, pick the best one and proceed. Only ask if the
  choice has significant irreversible consequences.
- After completing file operations, confirm briefly rather than restating what was done.
- When committing, write clear conventional-commit messages (e.g. `feat:`, `fix:`,
  `docs:`, `ci:`, `refactor:`, `test:`).
- **Always work on a feature branch** — never commit directly to `main`. Create a
  branch (e.g. `feat/log-api`, `fix/modbus-timeout`) before making changes. The user
  will merge via PR.

## Project Overview

Arctic Controller is a heat pump controller running on an **ESP32-P4** (M5Stack Tab5).
It communicates with an ECO-600 heat pump via Modbus RTU over RS485 and exposes a
REST API + WebSocket dashboard for monitoring and control.

- **Framework**: ESP-IDF v5.4.3
- **UI**: LVGL 9.2 with `esp_lvgl_port`
- **Display**: 720×1280 IPS (Tab5 built-in)
- **Languages**: C++ (firmware), Python (tests), HTML/JS (web dashboard)
- **Build**: CMake via ESP-IDF. Dependencies fetched by `fetch_repos.py` from `repos.json`

## Repository Structure

| Path | Purpose |
|------|---------|
| `main/` | Application source — screens, API server, Modbus, OTA, i18n |
| `components/` | Local ESP-IDF components (board support, custom libs) |
| `managed_components/` | ESP-IDF component manager dependencies |
| `dependencies/` | Git-fetched libraries (LVGL, mooncake, smooth_ui_toolkit) |
| `docs/` | Documentation and `openapi.yaml` (production API spec) |
| `tests/device/` | Pytest device tests, `DeviceClient`, test API OpenAPI spec |
| `.github/workflows/` | CI: `build.yml`, `device-tests.yml`, `create-release.yml` |
| `todo.md` | Product roadmap and feature backlog |
| `tests/device/TODO.md` | Test coverage backlog |
| `tests/device/README.md` | Test architecture, setup, and writing guide |

## Code Conventions

### C++ / Firmware
- Follow ESP-IDF patterns: `ESP_LOGI/LOGW/LOGE` for logging, `esp_err_t` returns
- LVGL widgets use `lv_obj_set_user_data()` with string tags for test addressability
- All user-facing strings go through the i18n translation layer (`i18n.h`)
- Three languages: English, French, Spanish — update all three when adding strings

### Web Dashboard (`main/web/index.html`)
- After editing `index.html`, run `idf.py reconfigure` before `idf.py build`.
  The gzip compression step only runs during CMake configure, not on every ninja
  build. Without reconfigure, the firmware will embed a stale `index.html.gz`.
- Test-only code is guarded by `#ifdef CONFIG_TEST_ENDPOINTS`
- When adding HTTP endpoints: bump `max_uri_handlers` in `api_server.cpp` if needed
  (currently 80 = ~40 production + ~32 test + headroom)
- New test endpoints must include `CHECK_SESSION_LOCK(req)` for mutating handlers

### Python / Tests
- Tests use `DeviceClient` (HTTP client) — never call LVGL directly
- Use `wait_for_screen()` / `wait_for_widget()` after clicks (LVGL animations need time)
- Use `time.sleep(0.3–0.5)` after toggles/rollers for event processing
- Use widget **tags** over labels for language-independent targeting
- Mock, don't connect: `wifi_mock()`, `firmware_mock()`, `notification_mock()`
- Restore persistent settings (language, timezone, temp unit) at test end
- Test files follow pattern: `test_<feature>.py`

### API
- Production API spec: `docs/openapi.yaml` (OpenAPI 3.0.3)
- Test API spec: `tests/device/openapi-test.yaml` (OpenAPI 3.0.3)
- Production endpoints under `/api/` — test endpoints under `/api/test/`

## CI / Workflow Awareness

- **Path filters**: `build.yml` and `device-tests.yml` use `paths:` allowlists —
  only firmware source, config, and test file changes trigger builds. Doc-only
  changes (`.md`, `docs/`) do **not** trigger builds.
- **Device tests** run on a single physical device (Pi Zero 2 W, runner label `tab5`)
- **Concurrency group** `device-tests` serializes all device access (build + release)
- `CONFIG_TEST_ENDPOINTS=y` is set only in `device-tests.yml`, not production builds
- **Release workflow** (`create-release.yml`) gates on device tests passing and
  ships 4 binaries: bootloader, partition-table, ota_data_initial, firmware

## After Major Changes — Checklist

After completing a feature, bug fix, or refactor, review this list before
considering the work done:

### Documentation
- [ ] Update `README.md` if user-facing behavior, setup steps, or architecture changed
- [ ] Update `docs/openapi.yaml` if production API endpoints were added/changed/removed
- [ ] Update `tests/device/openapi-test.yaml` if test endpoints were added/changed/removed
- [ ] Update `todo.md` — mark completed items with `[x]`, add new items for follow-up work
- [ ] Update `tests/device/TODO.md` if test coverage changed (new tests, gaps filled)
- [ ] Update `tests/device/README.md` if test count, file list, or architecture changed

### Code Quality
- [ ] Tag new LVGL widgets with `lv_obj_set_user_data()` for test addressability
- [ ] Add i18n translations for new user-facing strings (EN, FR, ES)
- [ ] Add `CHECK_SESSION_LOCK(req)` to new mutating test endpoints
- [ ] Bump `max_uri_handlers` in `api_server.cpp` if new endpoints were registered
- [ ] Guard test-only code with `#ifdef CONFIG_TEST_ENDPOINTS`

### Testing
- [ ] Add or update device tests for new/changed UI screens or features
- [ ] Run the full test suite locally before pushing (`pytest tests/device/ -v`)
- [ ] Verify test count in `tests/device/README.md` matches actual count

### CI / Build
- [ ] If new source directories were added, check `paths:` filters in workflow files
- [ ] If `sdkconfig.defaults` changed, verify both production and test builds work
- [ ] Check firmware image size hasn't grown unexpectedly (`idf.py size`)

### Commit / PR
- [ ] Use conventional commit messages (`feat:`, `fix:`, `docs:`, `ci:`, `test:`, `refactor:`)
- [ ] Keep commits atomic — one logical change per commit
- [ ] Verify the PR description summarizes what changed and why
