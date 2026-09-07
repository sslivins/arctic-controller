# Web Dashboard Tests

Browser-based tests for the Arctic Controller web dashboard using **Playwright**.

These tests open the dashboard in a headless Chromium browser, interact with it like
a real user, and verify that pages load, navigation works, data is displayed, and
settings controls are functional.

## Prerequisites

```bash
pip install -r tests/web/requirements.txt
playwright install chromium
```

## Running Tests

Against the device on the local network:

```bash
# Default: http://arctic.local
pytest tests/web/ -v

# Custom device URL
ARCTIC_URL=http://192.168.1.23 pytest tests/web/ -v

# With visible browser window
pytest tests/web/ -v --headed
```

## Test Structure

| File | Tests | Coverage |
|------|-------|----------|
| `test_login.py` | 4 | Login form, success, failure, nav hidden before login |
| `test_password_recovery.py` | 4 | Forgot password, cancel, bad code rejected, recovery with a one-time code |
| `test_change_password.py` | 2 | Credentials form, change password then sign in again |
| `test_dashboard.py` | 9 | Hero card, dots, perf strip, panels, polling |
| `test_navigation.py` | 9 | 6-page nav, logs page, events page, params page |
| `test_notifications.py` | 4 | Notification tray, badge, dismissal |
| `test_settings.py` | 8 | Settings cards, toggles, buttons, file upload, security tab |
| `test_location_weather.py` | 10 | Location card, place search, automatic timezone, status-bar weather |
| `test_temperature_history.py` | 3 | History chart, range paging, refresh |
| `test_i18n.py` | 3 | Language selector, EN→FR→ES switching, persistence |
| `test_tls.py` | 3 | TLS auth prerequisite, cert install/delete, PEM validation |

**Total: 59 tests**

## Architecture

- **Framework**: Playwright (sync API) via `pytest-playwright`
- **Target**: Real device dashboard at `ARCTIC_URL`
- **Auth handling**: Tests toggle web auth on/off via the REST API as needed
- **Failure screenshots**: Saved to `tests/web/screenshots/` on test failure
- **No mocking**: All tests run against the real device — data comes from the device API.
  Where a test needs a deterministic outdoor condition or geocoding result it uses the
  device's own `/api/test/*-mock` endpoints, so the firmware under test is still the real one.

## Fixtures (`conftest.py`)

| Fixture | Scope | Description |
|---------|-------|-------------|
| `base_url` | session | Device URL from `ARCTIC_URL` env var or `--arctic-url` |
| `dashboard_page` | function | Page navigated to dashboard (auth disabled) |
| `login_page` | function | Page showing login form (auth enabled) |
| `screenshot_on_failure` | function (auto) | Captures screenshot on test failure |

## Adding Tests

1. Create `test_<feature>.py` in this directory
2. Use `dashboard_page` fixture for tests that need the dashboard loaded
3. Use `login_page` fixture for login-specific tests
4. Use CSS class selectors (no `data-testid` attributes exist in the dashboard)
5. Use `page.wait_for_timeout()` after interactions for Alpine.js reactivity
6. Update this README with the new test count
