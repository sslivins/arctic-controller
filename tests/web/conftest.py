"""
Pytest fixtures for Arctic Controller web dashboard tests (Playwright).

Set ARCTIC_URL env var to override the default device address.
Example: ARCTIC_URL=http://192.168.1.23 pytest tests/web/ -v

Credentials are read from environment variables or .env file:
  ARCTIC_USERNAME (default: arctic)
  ARCTIC_PASSWORD (default: arctic)
"""

import os
import pytest
from pathlib import Path
from playwright.sync_api import Page, Browser, BrowserContext, expect

# Load .env file if present (for local development)
_env_file = Path(__file__).parent.parent.parent / ".env"
if _env_file.exists():
    for line in _env_file.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, _, value = line.partition("=")
            os.environ.setdefault(key.strip(), value.strip())

# Credentials from env (defaults match auth_manager defaults)
WEB_USERNAME = os.environ.get("ARCTIC_USERNAME", "arctic")
WEB_PASSWORD = os.environ.get("ARCTIC_PASSWORD", "arctic")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# Screenshot directory for failures
SCREENSHOT_DIR = Path(__file__).parent / "screenshots"


def pytest_addoption(parser):
    """Add command-line options for web tests."""
    parser.addoption(
        "--arctic-url",
        default=os.environ.get("ARCTIC_URL", "http://arctic.local"),
        help="Base URL of the Arctic Controller device",
    )


@pytest.fixture(scope="session")
def base_url(request) -> str:
    """Device base URL from CLI option or env var."""
    return request.config.getoption("--arctic-url")


@pytest.fixture(scope="session")
def browser_context_args():
    """Extra args applied to every browser context."""
    return {
        "viewport": {"width": 1280, "height": 900},
        "ignore_https_errors": True,
    }


# ---------- Auth helpers ----------

# Track auth state to avoid repeated API calls
_auth_disabled = None   # None = not checked yet, True = disabled, False = could not disable
_auth_needs_login = False


def _ensure_auth_disabled(base_url: str):
    """Disable web auth once, then cache the result for the session."""
    global _auth_disabled, _auth_needs_login
    if _auth_disabled is True:
        return True
    if _auth_disabled is False and _auth_needs_login:
        return False

    import requests

    headers = {}
    if API_KEY:
        headers["X-API-Key"] = API_KEY

    try:
        r = requests.get(f"{base_url}/api/auth/status", headers=headers, timeout=5)
        r.raise_for_status()
        status = r.json()

        if not status.get("web_auth_enabled"):
            _auth_disabled = True
            return True

        # If we have an API key, use it directly to disable auth
        if API_KEY:
            r = requests.post(
                f"{base_url}/api/auth/config",
                json={"web_auth_enabled": False},
                headers=headers,
                timeout=5,
            )
            if r.status_code == 200:
                _auth_disabled = True
                return True

        # Fall back to login + disable
        session = requests.Session()
        login_r = session.post(
            f"{base_url}/login",
            json={"username": WEB_USERNAME, "password": WEB_PASSWORD},
            timeout=5,
        )
        if login_r.status_code != 200 or not login_r.json().get("success"):
            _auth_needs_login = True
            return False

        r = session.post(
            f"{base_url}/api/auth/config",
            json={"web_auth_enabled": False},
            timeout=5,
        )
        if r.status_code == 200:
            _auth_disabled = True
            return True
    except Exception:
        pass

    _auth_needs_login = True
    return False


def _enable_web_auth(base_url: str):
    """Re-enable web auth via API."""
    global _auth_disabled
    import requests

    headers = {}
    if API_KEY:
        headers["X-API-Key"] = API_KEY

    try:
        if API_KEY:
            requests.post(
                f"{base_url}/api/auth/config",
                json={"web_auth_enabled": True},
                headers=headers,
                timeout=5,
            )
        else:
            session = requests.Session()
            session.post(
                f"{base_url}/login",
                json={"username": WEB_USERNAME, "password": WEB_PASSWORD},
                timeout=5,
            )
            session.post(
                f"{base_url}/api/auth/config",
                json={"web_auth_enabled": True},
                timeout=5,
            )
        _auth_disabled = False
    except Exception:
        pass


def _browser_login(page: Page):
    """Log in via the browser login form."""
    login_box = page.locator(".login-box")
    if login_box.is_visible():
        page.locator(".login-box input[type='text']").fill(WEB_USERNAME)
        page.locator(".login-box input[type='password']").fill(WEB_PASSWORD)
        page.locator(".login-box button[type='submit']").click()
        page.wait_for_selector("nav", timeout=30000)


# ---------- Page fixtures ----------


@pytest.fixture
def dashboard_page(page: Page, base_url: str) -> Page:
    """Navigate to the dashboard and wait for it to load.

    Tries to disable web auth first. If that fails (e.g. credentials changed),
    falls back to logging in via the browser.
    """
    auth_disabled = _ensure_auth_disabled(base_url)
    page.goto(base_url, wait_until="networkidle")

    if not auth_disabled:
        # Auth is still on — log in via the browser
        _browser_login(page)
    else:
        page.wait_for_selector("nav", timeout=30000)

    return page


@pytest.fixture
def login_page(page: Page, base_url: str) -> Page:
    """Navigate to the login page (web auth enabled).

    Enables web auth so the login form is shown.
    Cleans up by disabling web auth after the test.
    """
    _enable_web_auth(base_url)
    page.goto(base_url, wait_until="networkidle")
    # Wait for the login box to appear
    page.wait_for_selector(".login-box", timeout=10000)
    yield page
    # Cleanup: disable auth so dashboard tests work without login
    global _auth_disabled, _auth_needs_login
    _auth_disabled = None
    _auth_needs_login = False
    _ensure_auth_disabled(base_url)


# ---------- Failure screenshot ----------


@pytest.fixture(autouse=True)
def screenshot_on_failure(request, page: Page):
    """Capture a browser screenshot when a test fails."""
    yield
    rep = getattr(request.node, "rep_call", None)
    if rep and rep.failed:
        SCREENSHOT_DIR.mkdir(exist_ok=True)
        name = request.node.name.replace("/", "_").replace("::", "_")
        path = SCREENSHOT_DIR / f"web_{name}.png"
        try:
            page.screenshot(path=str(path), full_page=True)
            print(f"\n  Screenshot saved: {path}")
        except Exception as e:
            print(f"\n  Failed to capture screenshot: {e}")


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    """Stash test result on the item so fixtures can check pass/fail."""
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)
