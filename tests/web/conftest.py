"""
Pytest fixtures for Arctic Controller web dashboard tests (Playwright).

Set ARCTIC_URL env var to override the default device address.
Example: ARCTIC_URL=http://192.168.1.23 pytest tests/web/ -v
"""

import os
import pytest
from pathlib import Path
from playwright.sync_api import Page, Browser, BrowserContext, expect

# Default credentials (matching auth_manager defaults)
DEFAULT_USERNAME = "admin"
DEFAULT_PASSWORD = "arctic"

# Screenshot directory for failures
SCREENSHOT_DIR = Path(__file__).parent / "screenshots"


def pytest_addoption(parser):
    """Add command-line options for web tests."""
    parser.addoption(
        "--arctic-url",
        default=os.environ.get("ARCTIC_URL", "http://arctic.local"),
        help="Base URL of the Arctic Controller device",
    )
    parser.addoption(
        "--headed",
        action="store_true",
        default=False,
        help="Run browser in headed mode (visible window)",
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


@pytest.fixture(scope="session")
def browser_type_launch_args(request):
    """Launch args for the browser (headed mode support)."""
    args = {"args": ["--disable-gpu"]}
    if request.config.getoption("--headed"):
        args["headless"] = False
    return args


# ---------- Auth helpers ----------


def _disable_web_auth(base_url: str):
    """Disable web auth via API so tests can access the dashboard without login."""
    import requests

    # Check current auth status
    r = requests.get(f"{base_url}/api/auth/status", timeout=5)
    r.raise_for_status()
    status = r.json()

    if not status.get("web_auth_enabled"):
        return  # Already disabled

    # Try to login first to get a session cookie
    session = requests.Session()
    session.post(
        f"{base_url}/login",
        json={"username": DEFAULT_USERNAME, "password": DEFAULT_PASSWORD},
        timeout=5,
    )

    # Disable web auth
    session.post(
        f"{base_url}/api/auth/config",
        json={"web_auth_enabled": False, "api_auth_enabled": False},
        timeout=5,
    )


def _enable_web_auth(base_url: str):
    """Re-enable web auth via API."""
    import requests

    session = requests.Session()
    # Login first (may need session to change config)
    session.post(
        f"{base_url}/login",
        json={"username": DEFAULT_USERNAME, "password": DEFAULT_PASSWORD},
        timeout=5,
    )
    session.post(
        f"{base_url}/api/auth/config",
        json={"web_auth_enabled": True},
        timeout=5,
    )


# ---------- Page fixtures ----------


@pytest.fixture
def dashboard_page(page: Page, base_url: str) -> Page:
    """Navigate to the dashboard and wait for it to load.

    Disables web auth first so no login is required.
    After the test, the page is automatically closed by Playwright.
    """
    _disable_web_auth(base_url)
    page.goto(base_url, wait_until="networkidle")
    # Wait for Alpine.js to initialize — the nav bar should be visible
    page.wait_for_selector("nav", timeout=10000)
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
    # Cleanup: disable auth so other tests aren't affected
    _disable_web_auth(base_url)


# ---------- Failure screenshot ----------


@pytest.fixture(autouse=True)
def screenshot_on_failure(request, page: Page):
    """Capture a browser screenshot when a test fails."""
    yield
    if request.node.rep_call and request.node.rep_call.failed:
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
