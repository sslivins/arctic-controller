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
import urllib3
from pathlib import Path
from playwright.sync_api import Page, Browser, BrowserContext, expect

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

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

# Cache only the SUCCESS of disabling web auth. A transient failure must NEVER
# be cached: doing so used to poison the whole web suite — once one flaky
# /api/auth/status or /api/auth/config call failed, the cached "needs login"
# state made every later test short-circuit to "auth still enabled" and fall
# back to slow browser logins (or fail outright). Each test now re-attempts the
# disable until it genuinely succeeds.
_auth_disabled = None   # None/False = not confirmed disabled yet, True = confirmed disabled


def _ensure_auth_disabled(base_url: str):
    """Ensure web auth is disabled. Caches only success; re-attempts on failure."""
    global _auth_disabled
    if _auth_disabled is True:
        return True

    import requests
    import time

    headers = {}
    if API_KEY:
        headers["X-API-Key"] = API_KEY

    for attempt in range(3):
        try:
            r = requests.get(f"{base_url}/api/auth/status", headers=headers, timeout=5, verify=False)
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
                    verify=False,
                )
                if r.status_code == 200:
                    _auth_disabled = True
                    return True

            # Fall back to login + disable
            session = requests.Session()
            session.verify = False
            login_r = session.post(
                f"{base_url}/login",
                json={"username": WEB_USERNAME, "password": WEB_PASSWORD},
                timeout=5,
            )
            if login_r.status_code != 200 or not login_r.json().get("success"):
                return False

            r = session.post(
                f"{base_url}/api/auth/config",
                json={"web_auth_enabled": False},
                timeout=5,
            )
            if r.status_code == 200:
                _auth_disabled = True
                return True

            break  # Non-transient failure, stop retrying
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt < 2:
                time.sleep(2)
                continue
        except Exception:
            break  # Non-transient error, stop retrying

    return False


def _enable_web_auth(base_url: str):
    """Re-enable web auth via API."""
    global _auth_disabled
    import requests
    import time

    headers = {}
    if API_KEY:
        headers["X-API-Key"] = API_KEY

    for attempt in range(3):
        try:
            if API_KEY:
                requests.post(
                    f"{base_url}/api/auth/config",
                    json={"web_auth_enabled": True},
                    headers=headers,
                    timeout=5,
                    verify=False,
                )
            else:
                session = requests.Session()
                session.verify = False
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
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt < 2:
                time.sleep(2)
                continue
        except Exception:
            break


def _browser_login(page: Page):
    """Log in via the browser login form."""
    login_box = page.locator(".login-card")
    if login_box.is_visible():
        page.locator(".login-card input[name='username']").fill(WEB_USERNAME)
        page.locator(".login-card input[name='password']").fill(WEB_PASSWORD)
        page.locator(".login-card button[type='submit']").click()
        page.wait_for_selector(".rail", timeout=10000)


# ---------- Session baseline enforcement ----------


def _wait_for_web_ready(base_url: str, timeout: float = 45.0) -> None:
    """Block until the device answers on its web endpoint, or fail the session.

    Without this, a device whose web server never comes up makes *every* web
    test independently time out on ``page.goto`` — turning one dead device into
    a multi-minute (or multi-hour, across retries) suite crawl. Gate once at
    session start and fail fast with a clear message instead. ``/api/health`` is
    unauthenticated (the CI "Require HTTPS readiness" step polls the same route).
    """
    import requests
    import time

    headers = {"X-API-Key": API_KEY} if API_KEY else {}
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        try:
            r = requests.get(f"{base_url}/api/health", headers=headers,
                             timeout=5, verify=False)
            if r.ok:
                return
            last_err = f"HTTP {r.status_code}"
        except Exception as e:  # noqa: BLE001
            last_err = repr(e)
        time.sleep(1)
    pytest.fail(
        f"Device web endpoint at {base_url} did not become ready within "
        f"{timeout:.0f}s (last error: {last_err}). Aborting the web suite fast "
        f"instead of letting every test time out on page.goto.",
        pytrace=False,
    )


@pytest.fixture(scope="session", autouse=True)
def _web_auth_baseline(base_url: str):
    """Wait for web readiness, then force a known web-auth baseline at session end.

    The web suite toggles global web-auth state via cached module globals
    (``login_page`` enables it, ``dashboard_page`` disables it). If a test errors
    before its own fixture cleanup runs — or a transient re-disable call fails —
    web auth can be left enabled and leak into a later suite that shares the same
    physical device in a CI run. This session-scoped teardown resets the cached
    flag and forces web auth back to disabled, then confirms it via
    ``/api/auth/status`` so the baseline is verified, not merely requested.
    """
    _wait_for_web_ready(base_url)
    yield
    global _auth_disabled
    _auth_disabled = None
    _ensure_auth_disabled(base_url)

    # Verify the baseline is externally observable (best-effort).
    import requests
    import time

    headers = {"X-API-Key": API_KEY} if API_KEY else {}
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            r = requests.get(f"{base_url}/api/auth/status", headers=headers,
                             timeout=5, verify=False)
            if r.ok and not r.json().get("web_auth_enabled"):
                return
        except Exception:
            pass
        time.sleep(0.5)
    print("WARNING: web-auth baseline reset could not be confirmed disabled at "
          "session end; a later suite may inherit web auth enabled.")


# ---------- Page fixtures ----------


@pytest.fixture
def dashboard_page(page: Page, base_url: str) -> Page:
    """Navigate to the dashboard and wait for it to load.

    Tries to disable web auth first. If that fails (e.g. credentials changed),
    falls back to logging in via the browser.
    Retries page.goto on transient network/DNS errors.
    """
    auth_disabled = _ensure_auth_disabled(base_url)

    # Retry page.goto to handle transient mDNS resolution failures
    last_err = None
    for attempt in range(3):
        try:
            page.goto(base_url, wait_until="domcontentloaded")
            last_err = None
            break
        except Exception as e:
            last_err = e
            if attempt < 2:
                page.wait_for_timeout(2000)
    if last_err:
        raise last_err

    page.wait_for_selector(".rail, .login-card", timeout=10000)
    if page.locator(".login-card").is_visible():
        _browser_login(page)
    else:
        page.wait_for_selector(".rail", timeout=10000)

    return page


@pytest.fixture
def login_page(page: Page, base_url: str) -> Page:
    """Navigate to the login page (web auth enabled).

    Enables web auth so the login form is shown.
    Cleans up by disabling web auth after the test.
    """
    _enable_web_auth(base_url)
    page.goto(base_url, wait_until="domcontentloaded")
    # Wait for the login box to appear
    page.wait_for_selector(".login-card", timeout=10000)
    yield page
    # Cleanup: disable auth so dashboard tests work without login
    global _auth_disabled
    _auth_disabled = None
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
