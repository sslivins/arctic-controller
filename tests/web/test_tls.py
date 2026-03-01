"""Tests for TLS certificate management on the Security tab.

Verifies the auth prerequisite: TLS cert upload is only available when
both web auth AND API key auth are enabled. When either is off, the UI
shows a warning instead of the upload form.
"""

import os
import time
import pytest
import requests
import urllib3
from playwright.sync_api import Page, expect

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

from conftest import API_KEY, WEB_USERNAME, WEB_PASSWORD

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")


# ── Helpers ───────────────────────────────────────────────────────────────

def _api_headers():
    h = {}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    return h


def _device_has_tls_certs():
    """Check whether the device has TLS certs provisioned."""
    try:
        r = requests.get(
            f"{BASE_URL}/api/tls/status",
            headers=_api_headers(),
            timeout=5,
            verify=False,
        )
        return r.status_code == 200 and r.json().get("has_certs", False)
    except Exception:
        return False


def _set_auth(web: bool, api: bool):
    """Set web_auth_enabled and api_auth_enabled via the REST API.
    Returns False if blocked (e.g. TLS certs prevent disabling)."""
    for attempt in range(3):
        try:
            r = requests.post(
                f"{BASE_URL}/api/auth/config",
                json={"web_auth_enabled": web, "api_auth_enabled": api},
                headers=_api_headers(),
                timeout=5,
                verify=False,
            )
            if r.status_code == 403:
                return False
            return True
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


def _go_to_security(page: Page):
    """Navigate to the Security tab (5th nav button, index 4)."""
    page.locator("nav button").nth(4).click()
    page.wait_for_timeout(500)


def _browser_login(page: Page):
    """Log in via the browser login form if it appears."""
    login_box = page.locator(".login-box")
    if login_box.is_visible():
        page.locator(".login-box input[type='text']").fill(WEB_USERNAME)
        page.locator(".login-box input[type='password']").fill(WEB_PASSWORD)
        page.locator(".login-box button[type='submit']").click()
        page.wait_for_selector("nav", timeout=10000)


# ── Fixtures ──────────────────────────────────────────────────────────────

@pytest.fixture(autouse=True)
def _restore_auth():
    """Ensure auth toggles are restored after each test."""
    yield
    # Disable web auth, keep API auth enabled (test suite default)
    _set_auth(web=False, api=True)


# ── Tests ─────────────────────────────────────────────────────────────────

class TestTlsAuthPrerequisite:
    """TLS cert upload requires both web auth and API key auth enabled."""

    def test_warning_shown_when_both_auth_disabled(self, dashboard_page: Page):
        """Warning is shown when neither auth method is enabled."""
        if not _set_auth(web=False, api=False):
            pytest.skip("Auth cannot be disabled when TLS certs are provisioned")
        dashboard_page.reload(wait_until="networkidle")
        dashboard_page.wait_for_selector("nav", timeout=10000)
        _go_to_security(dashboard_page)

        # Warning should be visible (contains ⚠️)
        warning = dashboard_page.locator("text=⚠️").first
        expect(warning).to_be_visible(timeout=5000)

        # Upload form textarea should NOT be visible
        textareas = dashboard_page.locator("textarea").locator("visible=true")
        assert textareas.count() == 0, "Upload textarea should be hidden when auth is not ready"

    def test_warning_shown_when_only_web_auth_enabled(self, page: Page, base_url: str):
        """Warning persists when only web auth is on (API key still off)."""
        if not _set_auth(web=True, api=False):
            pytest.skip("Auth cannot be disabled when TLS certs are provisioned")
        page.goto(base_url, wait_until="networkidle")
        _browser_login(page)
        page.wait_for_selector("nav", timeout=10000)
        _go_to_security(page)

        warning = page.locator("text=⚠️").first
        expect(warning).to_be_visible(timeout=5000)

    def test_warning_shown_when_only_api_auth_enabled(self, dashboard_page: Page):
        """Warning persists when only API key auth is on (web auth off)."""
        if not _set_auth(web=False, api=True):
            pytest.skip("Auth cannot be disabled when TLS certs are provisioned")
        dashboard_page.reload(wait_until="networkidle")
        dashboard_page.wait_for_selector("nav", timeout=10000)
        _go_to_security(dashboard_page)

        warning = dashboard_page.locator("text=⚠️").first
        expect(warning).to_be_visible(timeout=5000)

    def test_upload_form_shown_when_both_auth_enabled(self, page: Page, base_url: str):
        """Upload form appears when both web auth and API key auth are enabled."""
        _set_auth(web=True, api=True)
        page.goto(base_url, wait_until="networkidle")
        _browser_login(page)
        page.wait_for_selector("nav", timeout=10000)
        _go_to_security(page)

        # Certificate textarea should be visible
        cert_textarea = page.locator("textarea").first
        expect(cert_textarea).to_be_visible(timeout=5000)

        # Warning should NOT be visible
        warnings = page.locator("text=⚠️").locator("visible=true")
        assert warnings.count() == 0, "Warning should be hidden when both auth methods are enabled"

    def test_tls_status_badges_visible(self, dashboard_page: Page):
        """TLS status badges (provisioned/active) are always visible."""
        _go_to_security(dashboard_page)

        # Second card on Security page is TLS Certificates
        cards = dashboard_page.locator(".card")
        tls_card = cards.nth(1)
        expect(tls_card).to_be_visible()
        text = tls_card.inner_text()
        assert len(text) > 10, "TLS card appears empty"

    def test_cert_upload_rejected_by_api_without_auth(self):
        """Server returns 403 when uploading certs without both auth enabled."""
        if not _set_auth(web=False, api=True):
            pytest.skip("Auth cannot be disabled when TLS certs are provisioned")
        time.sleep(0.3)

        r = requests.post(
            f"{BASE_URL}/api/tls/certificate",
            json={"cert": "fake-cert", "key": "fake-key"},
            headers=_api_headers(),
            timeout=5,
            verify=False,
        )
        assert r.status_code == 403, f"Expected 403, got {r.status_code}: {r.text}"


# Environment variables for real cert testing (set in CI via GitHub secrets)
TLS_FULLCHAIN = os.environ.get("TLS_FULLCHAIN_PEM", "")
TLS_PRIVKEY = os.environ.get("TLS_PRIVKEY_PEM", "")
_has_real_certs = bool(TLS_FULLCHAIN and TLS_PRIVKEY)


@pytest.mark.skipif(not _has_real_certs, reason="TLS_FULLCHAIN_PEM / TLS_PRIVKEY_PEM not set")
class TestTlsCertInstall:
    """Upload and delete real TLS certificates (requires GitHub secrets)."""

    def test_upload_real_certs(self):
        """Upload real certs, verify success, then delete to avoid switching to HTTPS."""
        _set_auth(web=True, api=True)
        time.sleep(0.3)

        # Upload
        r = requests.post(
            f"{BASE_URL}/api/tls/certificate",
            json={"cert": TLS_FULLCHAIN, "key": TLS_PRIVKEY},
            headers=_api_headers(),
            timeout=10,
            verify=False,
        )
        assert r.status_code == 200, f"Upload failed ({r.status_code}): {r.text}"
        data = r.json()
        assert data.get("success") is True, f"Upload response: {data}"

        # Verify status shows certs provisioned
        r = requests.get(
            f"{BASE_URL}/api/tls/status",
            headers=_api_headers(),
            timeout=5,
            verify=False,
        )
        assert r.status_code == 200
        status = r.json()
        assert status["has_certs"] is True, f"Expected has_certs=true after upload: {status}"
        # https_active should still be false (no reboot)
        assert status["https_active"] is False, "HTTPS should not be active without reboot"

        # Delete certs to prevent HTTPS activation on next reboot
        r = requests.delete(
            f"{BASE_URL}/api/tls/certificate",
            headers=_api_headers(),
            timeout=5,
            verify=False,
        )
        assert r.status_code == 200, f"Delete failed ({r.status_code}): {r.text}"

        # Verify certs are gone
        r = requests.get(
            f"{BASE_URL}/api/tls/status",
            headers=_api_headers(),
            timeout=5,
            verify=False,
        )
        assert r.status_code == 200
        status = r.json()
        assert status["has_certs"] is False, f"Expected has_certs=false after delete: {status}"

    def test_upload_rejects_invalid_cert(self):
        """Server rejects cert that doesn't look like PEM."""
        _set_auth(web=True, api=True)
        time.sleep(0.3)

        r = requests.post(
            f"{BASE_URL}/api/tls/certificate",
            json={"cert": "not-a-cert", "key": TLS_PRIVKEY},
            headers=_api_headers(),
            timeout=5,
            verify=False,
        )
        assert r.status_code == 400, f"Expected 400 for bad cert, got {r.status_code}"

    def test_upload_rejects_invalid_key(self):
        """Server rejects key that doesn't look like PEM."""
        _set_auth(web=True, api=True)
        time.sleep(0.3)

        r = requests.post(
            f"{BASE_URL}/api/tls/certificate",
            json={"cert": TLS_FULLCHAIN, "key": "not-a-key"},
            headers=_api_headers(),
            timeout=5,
            verify=False,
        )
        assert r.status_code == 400, f"Expected 400 for bad key, got {r.status_code}"
