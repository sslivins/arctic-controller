"""Tests for changing the password via the web dashboard Settings page.

Verifies the full user-facing flow:
  1. Log in with default credentials
  2. Navigate to Settings → Security
  3. Change the password via the credential form
  4. Log out and re-login with the new password
  5. Revert to original credentials (cleanup)

Prerequisites:
  - Device reachable at ARCTIC_URL
  - ARCTIC_USERNAME / ARCTIC_PASSWORD / ARCTIC_API_KEY set
"""

import os
import time
import pytest
import requests
import urllib3
from playwright.sync_api import Page, expect

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

from conftest import WEB_USERNAME, WEB_PASSWORD, API_KEY

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
NEW_PASSWORD = "N3wS3cure!Pass"


# ── Helpers ───────────────────────────────────────────────────────────────

def _api_headers():
    h = {}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    return h


def _enable_web_auth():
    """Enable web auth via API (only works when web auth is currently OFF)."""
    for attempt in range(3):
        try:
            requests.post(
                f"{BASE_URL}/api/auth/config",
                json={"web_auth_enabled": True},
                headers=_api_headers(),
                timeout=5,
                verify=False,
            )
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


def _disable_web_auth():
    """Disable web auth, falling back to session if API key alone fails."""
    for attempt in range(3):
        try:
            r = requests.post(
                f"{BASE_URL}/api/auth/config",
                json={"web_auth_enabled": False},
                headers=_api_headers(),
                timeout=5,
                verify=False,
            )
            if r.status_code == 401:
                s = requests.Session()
                s.verify = False
                for pw in (WEB_PASSWORD, NEW_PASSWORD):
                    login_r = s.post(
                        f"{BASE_URL}/login",
                        json={"username": WEB_USERNAME, "password": pw},
                        timeout=5,
                    )
                    if login_r.status_code == 200:
                        s.post(
                            f"{BASE_URL}/api/auth/config",
                            json={"web_auth_enabled": False},
                            timeout=5,
                        )
                        break
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


def _restore_credentials():
    """Reset credentials back to the CI-known values."""
    for attempt in range(3):
        try:
            r = requests.post(
                f"{BASE_URL}/api/auth/credentials",
                json={"username": WEB_USERNAME, "password": WEB_PASSWORD},
                headers=_api_headers(),
                timeout=5,
                verify=False,
            )
            if r.status_code == 401:
                # Web auth is on — log in with whatever password is active
                for pw in (WEB_PASSWORD, NEW_PASSWORD):
                    s = requests.Session()
                    s.verify = False
                    login_r = s.post(
                        f"{BASE_URL}/login",
                        json={"username": WEB_USERNAME, "password": pw},
                        timeout=5,
                    )
                    if login_r.status_code == 200:
                        s.post(
                            f"{BASE_URL}/api/auth/credentials",
                            json={"username": WEB_USERNAME, "password": WEB_PASSWORD},
                            timeout=5,
                        )
                        break
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


def _browser_login(page: Page, username: str, password: str):
    """Fill and submit the login form."""
    page.locator(".login-box input[type='text']").fill(username)
    page.locator(".login-box input[type='password']").fill(password)
    page.locator(".login-box button[type='submit']").click()


def _go_to_security(page: Page):
    """Click the Security nav button (5th button, index 4)."""
    page.locator("nav button").nth(4).click()
    page.wait_for_timeout(500)


# ── Fixtures ──────────────────────────────────────────────────────────────

@pytest.fixture(scope="module", autouse=True)
def _check_prerequisites():
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    last_err = None
    for attempt in range(3):
        try:
            r = requests.get(f"{BASE_URL}/api/health", timeout=5, verify=False)
            r.raise_for_status()
            return
        except Exception as e:
            last_err = e
            if attempt < 2:
                time.sleep(2)
    pytest.skip(f"Device not reachable at {BASE_URL}: {last_err}")


@pytest.fixture(autouse=True)
def _always_cleanup():
    """Restore credentials and disable web auth after each test."""
    yield
    _restore_credentials()
    _disable_web_auth()


# =========================================================================
# Tests
# =========================================================================

class TestChangePasswordFromUI:
    """Change password through the web dashboard and verify it works."""

    def test_change_password_via_settings(self, page: Page, base_url: str):
        """Full flow: login → security tab → change password → logout → re-login."""
        # ── Step 1: Enable web auth and log in ───────────────────
        _enable_web_auth()
        page.goto(base_url, wait_until="networkidle")
        page.wait_for_selector(".login-box", timeout=10000)

        _browser_login(page, WEB_USERNAME, WEB_PASSWORD)
        page.wait_for_selector("nav", timeout=10000)
        expect(page.locator("nav")).to_be_visible()

        # ── Step 2: Navigate to Security ───────────────────────
        _go_to_security(page)

        # ── Step 3: The credentials form should be visible ───────────
        # (web auth is ON, so the credentials section is rendered)
        password_input = page.locator('input[x-model="security.newPassword"]')
        expect(password_input).to_be_visible(timeout=5000)

        # ── Step 4: Change the password ──────────────────────────────
        save_btn = page.get_by_text("Save Credentials")

        password_input.fill(NEW_PASSWORD)
        expect(save_btn).to_be_visible()
        save_btn.click()

        # Wait for the success toast
        page.wait_for_timeout(1500)

        # ── Step 5: Logout ───────────────────────────────────────────
        # Reload to force a new session check
        page.goto(base_url, wait_until="networkidle")

        # Force logout by clearing cookies
        page.context.clear_cookies()
        page.goto(base_url, wait_until="networkidle")
        page.wait_for_selector(".login-box", timeout=10000)

        # ── Step 6: Old password should fail ─────────────────────────
        _browser_login(page, WEB_USERNAME, WEB_PASSWORD)
        page.wait_for_timeout(1500)
        # Should still be on login page
        expect(page.locator(".login-box")).to_be_visible()

        # ── Step 7: New password should succeed ──────────────────────
        # Clear fields and try new password
        page.locator(".login-box input[type='text']").fill(WEB_USERNAME)
        page.locator(".login-box input[type='password']").fill(NEW_PASSWORD)
        page.locator(".login-box button[type='submit']").click()
        page.wait_for_selector("nav", timeout=10000)
        expect(page.locator("nav")).to_be_visible()
        expect(page.locator(".login-box")).not_to_be_visible()

    def test_credentials_form_hidden_when_auth_disabled(
        self, page: Page, base_url: str
    ):
        """When web auth is OFF, the credentials form is not shown."""
        _disable_web_auth()
        page.goto(base_url, wait_until="networkidle")
        page.wait_for_selector("nav", timeout=10000)
        _go_to_security(page)

        # The "Save Credentials" button should not exist
        save_btn = page.get_by_text("Save Credentials")
        expect(save_btn).to_have_count(0)

    def test_credentials_form_appears_when_auth_enabled(
        self, page: Page, base_url: str
    ):
        """Enabling web auth via the toggle reveals the credentials form."""
        _disable_web_auth()
        page.goto(base_url, wait_until="networkidle")
        page.wait_for_selector("nav", timeout=10000)
        _go_to_security(page)

        # Find the visible toggle label that wraps the hidden web auth checkbox
        web_auth_label = page.locator(
            'label.toggle:has(input[x-model="security.webAuthEnabled"])'
        )
        web_auth_label.scroll_into_view_if_needed()
        web_auth_label.click()
        page.wait_for_timeout(500)

        # Credentials form should now be visible
        save_btn = page.get_by_text("Save Credentials")
        expect(save_btn).to_be_visible()
