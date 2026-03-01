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


def _get_tls_status():
    """Return the full TLS status dict from the device."""
    r = requests.get(
        f"{BASE_URL}/api/tls/status",
        headers=_api_headers(),
        timeout=5,
        verify=False,
    )
    r.raise_for_status()
    return r.json()


def _device_has_tls_certs():
    """Check whether the device has TLS certs provisioned."""
    try:
        return _get_tls_status().get("has_certs", False)
    except Exception:
        return False


def _set_auth(web: bool, api: bool):
    """Set web_auth_enabled and api_auth_enabled via the REST API.

    Handles 401 by falling back to a web session login.
    Returns False if blocked by 403 (e.g. TLS certs prevent disabling).
    Returns True on success, raises on network errors.
    """
    for attempt in range(3):
        try:
            r = requests.post(
                f"{BASE_URL}/api/auth/config",
                json={"web_auth_enabled": web, "api_auth_enabled": api},
                headers=_api_headers(),
                timeout=5,
                verify=False,
            )
            if r.status_code == 200:
                return True
            if r.status_code == 403:
                return False
            if r.status_code == 401:
                # Web auth is enabled — need a web session to change config
                s = requests.Session()
                s.verify = False
                for pw in (WEB_PASSWORD,):
                    login_r = s.post(
                        f"{BASE_URL}/login",
                        json={"username": WEB_USERNAME, "password": pw},
                        timeout=5,
                    )
                    if login_r.status_code == 200:
                        r2 = s.post(
                            f"{BASE_URL}/api/auth/config",
                            json={"web_auth_enabled": web, "api_auth_enabled": api},
                            timeout=5,
                        )
                        if r2.status_code == 200:
                            return True
                        if r2.status_code == 403:
                            return False
                        break
                # Fall through to retry
            time.sleep(1)
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)
    return False


def _go_to_security(page: Page):
    """Navigate to the Security tab (5th nav button, index 4)."""
    page.locator("nav button").nth(4).click()
    page.wait_for_timeout(500)


def _security_section(page: Page):
    """Return a locator scoped to the Security page section."""
    return page.locator("[x-show=\"page === 'security'\"]")


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
    # Disable web auth, keep API auth enabled (test suite default).
    # Ignore failure — the conftest dashboard_page fixture handles login fallback.
    _set_auth(web=False, api=True)


# ── Tests ─────────────────────────────────────────────────────────────────

class TestTlsAuthPrerequisite:
    """TLS cert upload requires both web auth and API key auth enabled."""

    def test_warning_shown_when_both_auth_disabled(self, dashboard_page: Page):
        """Warning is shown when neither auth method is enabled."""
        if _device_has_tls_certs():
            pytest.skip("Device has TLS certs — UI shows upload form instead of warning")
        if not _set_auth(web=False, api=False):
            pytest.skip("Auth cannot be disabled when TLS certs are provisioned")
        dashboard_page.reload(wait_until="networkidle")
        dashboard_page.wait_for_selector("nav", timeout=10000)
        _go_to_security(dashboard_page)

        # Warning should be visible within the Security section
        sec = _security_section(dashboard_page)
        warning = sec.locator("text=⚠️").first
        expect(warning).to_be_visible(timeout=5000)

        # Upload form textarea should NOT be visible
        textareas = sec.locator("textarea").locator("visible=true")
        assert textareas.count() == 0, "Upload textarea should be hidden when auth is not ready"

    def test_warning_shown_when_only_web_auth_enabled(self, page: Page, base_url: str):
        """Warning persists when only web auth is on (API key still off)."""
        if _device_has_tls_certs():
            pytest.skip("Device has TLS certs — UI shows upload form instead of warning")
        if not _set_auth(web=True, api=False):
            pytest.skip("Auth cannot be disabled when TLS certs are provisioned")
        page.goto(base_url, wait_until="networkidle")
        _browser_login(page)
        page.wait_for_selector("nav", timeout=10000)
        _go_to_security(page)

        sec = _security_section(page)
        warning = sec.locator("text=⚠️").first
        expect(warning).to_be_visible(timeout=5000)

    def test_warning_shown_when_only_api_auth_enabled(self, dashboard_page: Page):
        """Warning persists when only API key auth is on (web auth off)."""
        if _device_has_tls_certs():
            pytest.skip("Device has TLS certs — UI shows upload form instead of warning")
        if not _set_auth(web=False, api=True):
            pytest.skip("Auth cannot be disabled when TLS certs are provisioned")
        dashboard_page.reload(wait_until="networkidle")
        dashboard_page.wait_for_selector("nav", timeout=10000)
        _go_to_security(dashboard_page)

        sec = _security_section(dashboard_page)
        warning = sec.locator("text=⚠️").first
        expect(warning).to_be_visible(timeout=5000)

    def test_upload_form_shown_when_both_auth_enabled(self, page: Page, base_url: str):
        """Upload form appears when both web auth and API key auth are enabled."""
        _set_auth(web=True, api=True)
        page.goto(base_url, wait_until="networkidle")
        _browser_login(page)
        page.wait_for_selector("nav", timeout=10000)
        _go_to_security(page)

        sec = _security_section(page)

        # Certificate textarea should be visible
        cert_textarea = sec.locator("textarea").first
        expect(cert_textarea).to_be_visible(timeout=5000)

    def test_tls_status_badges_visible(self, page: Page, base_url: str):
        """TLS status badges (provisioned/active) are always visible."""
        _set_auth(web=True, api=True)
        page.goto(base_url, wait_until="networkidle")
        _browser_login(page)
        page.wait_for_selector("nav", timeout=10000)
        _go_to_security(page)

        sec = _security_section(page)
        # TLS Certificates is the second card on the Security page
        cards = sec.locator(".card")
        assert cards.count() >= 2, f"Expected at least 2 cards on Security page, got {cards.count()}"
        tls_card = cards.nth(1)
        expect(tls_card).to_be_visible()
        text = tls_card.inner_text()
        assert len(text) > 10, "TLS card appears empty"

    def test_cert_upload_rejected_by_api_without_auth(self):
        """Server returns 403 when uploading certs without both auth enabled."""
        if not _set_auth(web=False, api=True):
            pytest.skip("Auth cannot be disabled when TLS certs are provisioned")
        time.sleep(0.5)

        # Verify auth was actually disabled
        status = _get_tls_status()
        if status.get("auth_ready", False):
            pytest.skip("Auth is still fully enabled despite disable attempt")

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
        """Upload real certs, verify success, then delete to clean up."""
        _set_auth(web=True, api=True)
        time.sleep(0.3)

        # Record state before upload
        pre_status = _get_tls_status()
        was_https_active = pre_status.get("https_active", False)

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
        status = _get_tls_status()
        assert status["has_certs"] is True, f"Expected has_certs=true after upload: {status}"

        # If HTTPS was not active before, it shouldn't be active now (needs reboot)
        if not was_https_active:
            assert status["https_active"] is False, \
                "HTTPS should not be active without reboot"

        # Delete certs to prevent HTTPS activation on next reboot
        r = requests.delete(
            f"{BASE_URL}/api/tls/certificate",
            headers=_api_headers(),
            timeout=5,
            verify=False,
        )
        assert r.status_code == 200, f"Delete failed ({r.status_code}): {r.text}"

        # Verify certs are gone
        status = _get_tls_status()
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
