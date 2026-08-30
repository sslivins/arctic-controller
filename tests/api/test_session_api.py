"""
Functional tests for web session management.

Covers the full session lifecycle: login, session-cookie access,
multiple concurrent sessions, logout, credential changes, and
session enforcement on various protected endpoints.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
  - ARCTIC_USERNAME / ARCTIC_PASSWORD env vars set
"""

import os
import time

import pytest
import requests
import urllib3
from pathlib import Path

# Load .env from repo root if present (local dev)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Disable TLS verification for self-signed device certificate
_OrigSessionInit = requests.Session.__init__
def _session_init_no_verify(self, *args, **kwargs):
    _OrigSessionInit(self, *args, **kwargs)
    self.verify = False
requests.Session.__init__ = _session_init_no_verify

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")
USERNAME = os.environ.get("ARCTIC_USERNAME", "arctic")
PASSWORD = os.environ.get("ARCTIC_PASSWORD", "arctic")

# ── Helpers ───────────────────────────────────────────────────────────────

def _api_headers():
    """Headers with API key for unauthenticated administrative calls."""
    h = {}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    return h


_RETRY_BACKOFF_S = 2


def _request_with_retry(do_request, *, attempts: int = 3):
    """Call ``do_request`` (a Response-returning thunk), retrying only on
    transient transport errors (ConnectionError / Timeout), never on an HTTP
    status code.

    Credential and auth-config mutations trigger a synchronous NVS flash write
    plus a session-table rebuild on the device's single-threaded httpd, which
    can occasionally exceed one 5s read window under CI load and raise
    ReadTimeout. A real HTTP response (200/401/403/…) returns immediately
    without retry, so status-code assertions are unaffected; only a genuine
    device stall is retried. Mirrors the resilience already used by
    _auth_config_post / _restore_credentials.
    """
    for attempt in range(attempts):
        try:
            return do_request()
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == attempts - 1:
                raise
            time.sleep(_RETRY_BACKOFF_S)


def _login(session: requests.Session, username=None, password=None):
    """POST /login and return the response.  Cookies are stored in *session*."""
    return _request_with_retry(lambda: session.post(
        f"{BASE_URL}/login",
        json={
            "username": username or USERNAME,
            "password": password or PASSWORD,
        },
        timeout=5,
    ))


def _admin_session(username=None, password=None):
    """Return a requests.Session that is logged in (for admin endpoints).

    When web auth is enabled, /api/auth/config POST and /api/auth/credentials
    POST require a session cookie (check_web_auth), not just an API key.
    """
    s = requests.Session()
    _login(s, username=username, password=password)
    return s


def _auth_config_post(payload: dict):
    """POST /api/auth/config, falling back to a session login on 401."""
    def _once():
        r = requests.post(
            f"{BASE_URL}/api/auth/config",
            json=payload,
            headers=_api_headers(),
            timeout=5,
        )
        if r.status_code == 403:
            return False
        if r.status_code == 401:
            s = _admin_session()
            r2 = s.post(f"{BASE_URL}/api/auth/config", json=payload, timeout=5)
            if r2.status_code == 403:
                return False
        return True
    return _request_with_retry(_once)


def _enable_web_auth():
    """Enable web + API auth so that check_api_auth enforces key validation."""
    _auth_config_post({"web_auth_enabled": True, "api_auth_enabled": True})


def _disable_web_auth():
    """Attempt to disable mandatory auth; returns False when rejected."""
    return _auth_config_post({"web_auth_enabled": False, "api_auth_enabled": False})


def _restore_credentials():
    """Reset username/password back to the CI-known values."""
    def _once():
        # Try API-key first (works when web auth is off)
        r = requests.post(
            f"{BASE_URL}/api/auth/credentials",
            json={"username": USERNAME, "password": PASSWORD},
            headers=_api_headers(),
            timeout=5,
        )
        if r.status_code == 401:
            # Web auth is on — use a session
            s = _admin_session()
            s.post(
                f"{BASE_URL}/api/auth/credentials",
                json={"username": USERNAME, "password": PASSWORD},
                timeout=5,
            )
    _request_with_retry(_once)


# ── Fixtures ──────────────────────────────────────────────────────────────

@pytest.fixture(scope="module", autouse=True)
def _check_prerequisites():
    """Skip the entire module if the device is unreachable or creds missing."""
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    if not USERNAME or not PASSWORD:
        pytest.skip("ARCTIC_USERNAME / ARCTIC_PASSWORD not set")
    last_err = None
    for attempt in range(3):
        try:
            r = requests.get(f"{BASE_URL}/api/health", timeout=5)
            r.raise_for_status()
            return
        except Exception as e:
            last_err = e
            if attempt < 2:
                time.sleep(2)
    pytest.skip(f"Device not reachable at {BASE_URL}: {last_err}")


@pytest.fixture(autouse=True)
def _always_restore_auth():
    """Ensure mandatory auth and CI credentials are restored after each test."""
    yield
    _enable_web_auth()
    _restore_credentials()


# =========================================================================
# Session Login
# =========================================================================

class TestSessionLogin:
    """POST /login — credential validation and cookie issuance."""

    def test_login_returns_session_cookie(self):
        """Successful login sets an arctic_session cookie."""
        _enable_web_auth()
        s = requests.Session()
        r = _login(s)
        assert r.status_code == 200
        assert r.json().get("success") is True
        assert "arctic_session" in s.cookies

    def test_login_wrong_password_returns_401(self):
        _enable_web_auth()
        s = requests.Session()
        r = _login(s, password="wr0ng!")
        assert r.status_code == 401

    def test_login_wrong_username_returns_401(self):
        _enable_web_auth()
        s = requests.Session()
        r = _login(s, username="nobody")
        assert r.status_code == 401

    def test_login_missing_fields_returns_400(self):
        _enable_web_auth()
        r = requests.post(f"{BASE_URL}/login", json={}, timeout=5)
        assert r.status_code == 400

    def test_login_no_body_returns_400(self):
        _enable_web_auth()
        r = requests.post(f"{BASE_URL}/login", timeout=5)
        assert r.status_code == 400


# =========================================================================
# Session Access
# =========================================================================

class TestSessionAccess:
    """Verify that a session cookie grants access to protected endpoints."""

    PROTECTED_ENDPOINTS = [
        "/api/info",
        "/api/heatpump/status",
    ]

    @pytest.mark.parametrize("endpoint", PROTECTED_ENDPOINTS)
    def test_session_cookie_grants_get_access(self, endpoint):
        """A valid session cookie should authorize GET on protected routes."""
        _enable_web_auth()
        s = requests.Session()
        _login(s)
        r = s.get(f"{BASE_URL}{endpoint}", timeout=5)
        assert r.status_code == 200, f"{endpoint} returned {r.status_code}"

    def test_no_credentials_rejected(self):
        """Without cookie or API-key, protected endpoint returns 401."""
        _enable_web_auth()
        r = requests.get(f"{BASE_URL}/api/info", timeout=5)
        assert r.status_code == 401

    def test_stale_cookie_rejected(self):
        """A fabricated / invalid cookie should be rejected."""
        _enable_web_auth()
        s = requests.Session()
        s.cookies.set("arctic_session", "00000000deadbeef00000000deadbeef")
        r = s.get(f"{BASE_URL}/api/info", timeout=5)
        assert r.status_code == 401

    def test_session_and_api_key_both_work(self):
        """Both auth methods should independently grant access."""
        _enable_web_auth()

        # Via session
        s = requests.Session()
        _login(s)
        r1 = s.get(f"{BASE_URL}/api/info", timeout=5)
        assert r1.status_code == 200

        # Via API key (no session)
        r2 = requests.get(
            f"{BASE_URL}/api/info", headers=_api_headers(), timeout=5
        )
        assert r2.status_code == 200


# =========================================================================
# Multiple Concurrent Sessions
# =========================================================================

class TestConcurrentSessions:
    """The device supports up to AUTH_MAX_SESSIONS (4) concurrent sessions."""

    def test_two_independent_sessions(self):
        """Two separate logins should each get a working session."""
        _enable_web_auth()
        s1 = requests.Session()
        s2 = requests.Session()
        _login(s1)
        _login(s2)

        r1 = s1.get(f"{BASE_URL}/api/info", timeout=5)
        r2 = s2.get(f"{BASE_URL}/api/info", timeout=5)
        assert r1.status_code == 200
        assert r2.status_code == 200

        # Tokens should differ
        assert s1.cookies["arctic_session"] != s2.cookies["arctic_session"]

    def test_logout_one_does_not_affect_other(self):
        """Logging out session A should leave session B working."""
        _enable_web_auth()
        s1 = requests.Session()
        s2 = requests.Session()
        _login(s1)
        _login(s2)

        # Logout session 1
        s1.post(f"{BASE_URL}/logout", timeout=5)

        # Session 1 should fail
        r1 = s1.get(f"{BASE_URL}/api/info", timeout=5)
        assert r1.status_code == 401

        # Session 2 should still work
        r2 = s2.get(f"{BASE_URL}/api/info", timeout=5)
        assert r2.status_code == 200


# =========================================================================
# Logout
# =========================================================================

class TestLogout:
    """POST /logout — session invalidation."""

    def test_logout_clears_session(self):
        """After logout, the same cookie should no longer be valid."""
        _enable_web_auth()
        s = requests.Session()
        _login(s)

        # Verify access works
        assert s.get(f"{BASE_URL}/api/info", timeout=5).status_code == 200

        # Logout
        r = s.post(f"{BASE_URL}/logout", timeout=5)
        assert r.status_code == 200
        assert r.json().get("success") is True

        # Same session should now be rejected
        assert s.get(f"{BASE_URL}/api/info", timeout=5).status_code == 401

    def test_double_logout_is_safe(self):
        """Calling logout twice should not error."""
        _enable_web_auth()
        s = requests.Session()
        _login(s)
        s.post(f"{BASE_URL}/logout", timeout=5)
        r = s.post(f"{BASE_URL}/logout", timeout=5)
        assert r.status_code == 200


# =========================================================================
# Credential Changes
# =========================================================================

class TestCredentialChange:
    """POST /api/auth/credentials — changing username/password."""

    def test_change_password_invalidates_old_login(self):
        """After changing the password, old credentials should fail."""
        _enable_web_auth()
        new_password = "Tmp$ecure990"

        try:
            # Change password via session (admin endpoints require session auth)
            s = _admin_session()
            r = _request_with_retry(lambda: s.post(
                f"{BASE_URL}/api/auth/credentials",
                json={"username": USERNAME, "password": new_password},
                timeout=5,
            ))
            assert r.status_code == 200

            # Old password should fail
            s2 = requests.Session()
            r2 = _login(s2, password=PASSWORD)
            assert r2.status_code == 401

            # New password should work
            s3 = requests.Session()
            r3 = _login(s3, password=new_password)
            assert r3.status_code == 200
        finally:
            # Restore creds using whatever password is currently active
            for pw in (new_password, PASSWORD):
                s_fix = _admin_session(password=pw)
                _request_with_retry(lambda s=s_fix: s.post(
                    f"{BASE_URL}/api/auth/credentials",
                    json={"username": USERNAME, "password": PASSWORD},
                    timeout=5,
                ))

    def test_change_username_invalidates_old_login(self):
        """After changing the username, logging in with the old one should fail."""
        _enable_web_auth()
        new_user = "admin"

        try:
            # Change username via session
            s = _admin_session()
            _request_with_retry(lambda: s.post(
                f"{BASE_URL}/api/auth/credentials",
                json={"username": new_user, "password": PASSWORD},
                timeout=5,
            ))

            # Old username should fail
            s2 = requests.Session()
            r = _login(s2, username=USERNAME)
            assert r.status_code == 401

            # New username should work
            s3 = requests.Session()
            r3 = _login(s3, username=new_user)
            assert r3.status_code == 200
        finally:
            # Restore username using whatever is currently active
            for user in (new_user, USERNAME):
                s_fix = _admin_session(username=user)
                _request_with_retry(lambda s=s_fix: s.post(
                    f"{BASE_URL}/api/auth/credentials",
                    json={"username": USERNAME, "password": PASSWORD},
                    timeout=5,
                ))

    def test_credential_change_with_session_auth(self):
        """Credentials can be changed using session auth (not just API key)."""
        _enable_web_auth()
        s = requests.Session()
        _login(s)

        new_pw = "TempPass123!"
        try:
            r = _request_with_retry(lambda: s.post(
                f"{BASE_URL}/api/auth/credentials",
                json={"username": USERNAME, "password": new_pw},
                timeout=5,
            ))
            assert r.status_code == 200
        finally:
            s_fix = _admin_session(password=new_pw)
            _request_with_retry(lambda: s_fix.post(
                f"{BASE_URL}/api/auth/credentials",
                json={"username": USERNAME, "password": PASSWORD},
                timeout=5,
            ))


# =========================================================================
# Auth Config Toggle
# =========================================================================

class TestAuthToggle:
    """POST /api/auth/config — mandatory authentication enforcement."""

    def test_disable_web_auth_is_rejected(self):
        """Remote administration must never permit unauthenticated access."""
        _enable_web_auth()

        # Confirm locked first (no API key, no session)
        r = requests.get(f"{BASE_URL}/api/info", timeout=5)
        assert r.status_code == 401

        # Disable auth (needs session since web auth is on)
        assert _disable_web_auth() is False

        # The endpoint remains protected.
        r = requests.get(f"{BASE_URL}/api/info", timeout=5)
        assert r.status_code == 401

    def test_enable_web_auth_locks_endpoints(self):
        """Enabling web auth should immediately require credentials."""
        _enable_web_auth()

        # Locked without credentials; API-key access remains available.
        r = requests.get(f"{BASE_URL}/api/info", timeout=5)
        assert r.status_code == 401
        authorized = requests.get(
            f"{BASE_URL}/api/info", headers=_api_headers(), timeout=5
        )
        assert authorized.status_code == 200

    def test_auth_status_reflects_session_validity(self):
        """GET /api/auth/status should report session_valid correctly."""
        _enable_web_auth()

        # No session — session_valid should be false
        r1 = requests.get(f"{BASE_URL}/api/auth/status", timeout=5)
        assert r1.status_code == 200
        assert r1.json()["session_valid"] is False

        # With valid session
        s = requests.Session()
        _login(s)
        r2 = s.get(f"{BASE_URL}/api/auth/status", timeout=5)
        assert r2.status_code == 200
        assert r2.json()["session_valid"] is True


# =========================================================================
# API Key via Session
# =========================================================================

class TestApiKeyViaSession:
    """GET /api/auth/apikey and POST /api/auth/apikey/regenerate."""

    def test_retrieve_api_key_with_session(self):
        """A logged-in session can retrieve the current API key."""
        _enable_web_auth()
        s = requests.Session()
        _login(s)

        r = s.get(f"{BASE_URL}/api/auth/apikey", timeout=5)
        assert r.status_code == 200
        data = r.json()
        assert "api_key" in data
        assert len(data["api_key"]) == 32

    @pytest.mark.skip(reason="Regenerating the API key invalidates ARCTIC_API_KEY for the rest of the run")
    def test_regenerate_api_key_changes_value(self):
        """Regenerating the API key should produce a different key."""
        _enable_web_auth()
        s = requests.Session()
        _login(s)

        old = s.get(f"{BASE_URL}/api/auth/apikey", timeout=5).json()["api_key"]
        r = s.post(f"{BASE_URL}/api/auth/apikey/regenerate", timeout=5)
        assert r.status_code == 200
        new_key = r.json()["api_key"]
        assert new_key != old
        assert len(new_key) == 32

    def test_api_key_endpoint_blocked_without_session(self):
        """Without a session, /api/auth/apikey should be blocked."""
        _enable_web_auth()
        r = requests.get(f"{BASE_URL}/api/auth/apikey", timeout=5)
        assert r.status_code == 401


# =========================================================================
# Mandatory Auth Enforcement
# =========================================================================

class TestMandatoryAuthEnforcement:
    """Remote administration authentication must remain enabled."""

    def test_cannot_disable_web_auth(self):
        """POST /api/auth/config with web_auth_enabled=false should return 403."""
        s = _admin_session()
        r = s.post(
            f"{BASE_URL}/api/auth/config",
            json={"web_auth_enabled": False},
            timeout=5,
        )
        assert r.status_code == 403
        assert "TLS" in r.json().get("error", "") or "cannot" in r.json().get("error", "").lower()

    def test_cannot_disable_api_auth(self):
        """POST /api/auth/config with api_auth_enabled=false should return 403."""
        s = _admin_session()
        r = s.post(
            f"{BASE_URL}/api/auth/config",
            json={"api_auth_enabled": False},
            timeout=5,
        )
        assert r.status_code == 403

    def test_cannot_disable_both_auth_methods(self):
        """Disabling both web and API auth simultaneously should return 403."""
        s = _admin_session()
        r = s.post(
            f"{BASE_URL}/api/auth/config",
            json={"web_auth_enabled": False, "api_auth_enabled": False},
            timeout=5,
        )
        assert r.status_code == 403

    def test_can_still_enable_auth(self):
        """Enabling authentication when already enabled should succeed."""
        s = _admin_session()
        r = s.post(
            f"{BASE_URL}/api/auth/config",
            json={"web_auth_enabled": True},
            timeout=5,
        )
        assert r.status_code == 200
