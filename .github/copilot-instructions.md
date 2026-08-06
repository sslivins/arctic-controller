# Copilot Instructions — Arctic Controller

## Conversation Preferences

- **Do not use popup selection dialogs** (the `ask_questions` tool). Ask questions
  directly in the chat and wait for a response. The user prefers inline conversation.
- Be direct and concise. Skip preamble like "I'll now…" — just do the work.
- When multiple approaches exist, pick the best one and proceed. Only ask if the
  choice has significant irreversible consequences.
- After completing file operations, confirm briefly rather than restating what was done.
- When committing, write clear **conventional-commit** messages. The release
  workflow auto-generates changelogs from these prefixes, so consistent usage
  matters:
  - `feat:` — new feature or capability
  - `fix:` — bug fix
  - `docs:` — documentation only
  - `ci:` — CI/workflow changes
  - `refactor:` — code restructuring (no behavior change)
  - `test:` — adding or updating tests
  - `chore:` — maintenance tasks (skipped in release notes)
  - Scoped variants are fine: `fix(ci):`, `feat(modbus):`
- **Always work on a feature branch** — never commit directly to `main`. Create a
  branch (e.g. `feat/log-api`, `fix/modbus-timeout`) before making changes. The user
  will merge via PR.
- **Batch related changes into a single PR** — device tests take ~20 minutes to run
  in CI. Don't create separate PRs for small, related fixes (e.g. multiple test
  tweaks). Group them on one branch and open one PR when the batch is ready.
- When asked for a PR description, always output it in **Markdown** format.

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
- **Printf format specifiers**: The RISC-V toolchain does not reliably handle `%lld`
  for `int64_t`. Always cast to `(long)` and use `%ld`, or cast to `(unsigned long)`
  and use `%lu`. This applies to `esp_timer_get_time()` results and any other
  `int64_t` / `uint64_t` values in log statements.
- LVGL widgets use `lv_obj_set_user_data()` with string tags for test addressability
- All user-facing strings go through the i18n translation layer (`i18n.h`)
- Three languages: English, French, Spanish — update all three when adding strings

### UI / UX Conventions (screens & dialogs)

These are the **target** conventions for all LVGL screens and dialogs. Rollout is
incremental — **start with the control screen** (`heatpump_params_screen.cpp`) and
bring the other screens into line in later passes. When you touch a screen, migrate
it toward these rules rather than matching the surrounding legacy layout.

- **Modal action buttons go in a bottom action bar, not the top corners.** A
  full-width row at the bottom of the dialog, close to the control the user just
  interacted with (Fitts's Law + proximity). Do **not** put Save/confirm in a top
  corner.
- **Cancel on the left, Save/confirm on the right** (LTR reading order — the eye
  lands on the primary action last).
- **Primary action is visually dominant, dismiss is quiet.** Save = filled/accent
  button; Cancel = ghost/outline. Space them apart so a mis-tap can't flip between
  them.
- **Label buttons with text, not bare icons.** A lone `✓`/`✕` is ambiguous. Use
  worded actions (`Save` / `Cancel`) — especially for irreversible writes (e.g.
  technician parameters). Text goes through i18n (EN/FR/ES).
- **Never commit on dismiss.** Closing, backing out, or tapping outside always
  **discards**. Committing must be an explicit, deliberate tap on Save.
- **Large, well-spaced touch targets** (≥60 px tall).

**Unavailable / disconnected state:**
- **Signal it once at the screen level**, not on every widget/dialog. A single
  screen-level banner (e.g. "Heat pump not connected") is preferred over repeating a
  note in each dialog.
- **Disconnected messaging is red text + a warning icon (`⚠`), used consistently
  across screens.** The home error card and the control-screen banner both render
  `⚠ Heat pump not connected` in the error color so the state reads the same
  everywhere.
- **Show `--` for unavailable live values — never fabricate a default.** Live values
  (Modbus reads) are not cached; when disconnected we genuinely have no value, so
  display `--`. Do not fall back to the vendor default and present it as if it were a
  reading. Static reference text (parameter name, plain-language detail, range) may
  still be shown since it isn't live state.
- **Render controls view-only rather than erroring on action.** When an action can't
  succeed (e.g. a write while disconnected), hide/disable the control (steppers,
  Save) instead of letting the user act and then showing an error toast.

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
- **Demo mode toggle requires a reboot**: Changing `demo_mode` via
  `set_preference(demo_mode=...)` only persists the setting. The controller must
  be rebooted (`device.reboot()`) for the change to take effect. After rebooting,
  call `device.wait_for_device(timeout=30.0)` to wait for it to come back online.
- Test files follow pattern: `test_<feature>.py`

### API
- Production API spec: `docs/openapi.yaml` (OpenAPI 3.0.3)
- Test API spec: `tests/device/openapi-test.yaml` (OpenAPI 3.0.3)
- Production endpoints under `/api/` — test endpoints under `/api/test/`

## CI / Workflow Awareness

- **Path filters**: `build.yml` and `device-tests.yml` use `paths:` allowlists —
  only firmware source, config, and test file changes trigger builds. Doc-only
  changes (`.md`, `docs/`) do **not** trigger builds.
- **Device tests** run on a self-hosted runner (4-core VM, 4 GB RAM, runner label `vm-mi`)
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
