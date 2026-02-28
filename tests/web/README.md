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
| `test_login.py` | 6 | Login form, success, failure, empty fields |
| `test_dashboard.py` | 11 | Hero card, dots, perf strip, panels, polling |
| `test_navigation.py` | 16 | 6-page nav, logs page, events page, params page |
| `test_settings.py` | 11 | Settings cards, toggles, buttons, file upload, security tab |
| `test_i18n.py` | 5 | Language selector, EN→FR→ES switching, persistence |
| `test_tls.py` | 9 | TLS auth prerequisite, cert install/delete, API 403, PEM validation |

**Total: 58 tests**

## Architecture

- **Framework**: Playwright (sync API) via `pytest-playwright`
- **Target**: Real device dashboard at `ARCTIC_URL`
- **Auth handling**: Tests toggle web auth on/off via the REST API as needed
- **Failure screenshots**: Saved to `tests/web/screenshots/` on test failure
- **No mocking**: All tests run against the real device — data comes from the device API

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
