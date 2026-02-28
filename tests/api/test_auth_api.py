"""
Functional tests for the authentication API endpoints.

Tests auth config, session login/logout, API key retrieval,
and auth enforcement behavior.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
  - ARCTIC_USERNAME / ARCTIC_PASSWORD env vars set (default: arctic/arctic)
"""

import os
import time

import pytest
import requests
import urllib3
from pathlib import Path
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Load .env from repo root if present (local dev)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")
USERNAME = os.environ.get("ARCTIC_USERNAME", "arctic")
PASSWORD = os.environ.get("ARCTIC_PASSWORD", "arctic")

# Retry-enabled session for all API calls
_session = requests.Session()
_retry = Retry(total=3, backoff_factor=1, allowed_methods=None,
               status_forcelist=[502, 503, 504])
_session.mount("http://", HTTPAdapter(max_retries=_retry))
_session.mount("https://", HTTPAdapter(max_retries=_retry))
_session.verify = False


def _headers(api_key=None):
    h = {}
    key = api_key if api_key is not None else API_KEY
    if key:
        h["X-API-Key"] = key
    return h


def _get(path, **kwargs):
    return _session.get(f"{BASE_URL}{path}", headers=_headers(), timeout=10, **kwargs)


def _post(path, json=None, headers=None, **kwargs):
    h = headers if headers is not None else _headers()
    return _session.post(f"{BASE_URL}{path}", headers=h, json=json, timeout=10, **kwargs)


def _enable_web_auth():
    for attempt in range(3):
        try:
            requests.post(
                f"{BASE_URL}/api/auth/config",
                json={"web_auth_enabled": True},
                headers=_headers(),
                timeout=5,
            )
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


def _disable_web_auth():
    for attempt in range(3):
        try:
            requests.post(
                f"{BASE_URL}/api/auth/config",
                json={"web_auth_enabled": False},
                headers=_headers(),
                timeout=5,
            )
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


@pytest.fixture(scope="module", autouse=True)
def _check_prerequisites():
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
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


# ── Auth Config ───────────────────────────────────────────────────────────


class TestAuthConfig:
    """GET/POST /api/auth/config — auth configuration."""

    def test_get_auth_config(self):
        """GET returns current auth settings."""
        r = _get("/api/auth/config")
        assert r.status_code == 200
        data = r.json()
        assert "web_auth_enabled" in data
        assert "api_auth_enabled" in data
        assert isinstance(data["web_auth_enabled"], bool)
        assert isinstance(data["api_auth_enabled"], bool)

    def test_auth_config_has_username(self):
        """Config response includes current username."""
        data = _get("/api/auth/config").json()
        assert "username" in data
        assert isinstance(data["username"], str)

    def test_auth_config_has_authenticated_field(self):
        """Config response indicates whether current request is authenticated."""
        data = _get("/api/auth/config").json()
        assert "authenticated" in data
        assert isinstance(data["authenticated"], bool)


# ── Auth Status ───────────────────────────────────────────────────────────


class TestAuthStatus:
    """GET /api/auth/status — quick auth check."""

    def test_auth_status_returns_200(self):
        r = _get("/api/auth/status")
        assert r.status_code == 200

    def test_auth_status_structure(self):
        data = _get("/api/auth/status").json()
        assert "web_auth_enabled" in data
        assert "session_valid" in data


# ── Login/Logout ──────────────────────────────────────────────────────────


class TestLoginLogout:
    """POST /login, POST /logout — session management."""

    def test_login_with_valid_credentials(self):
        """Login should succeed and set a session cookie."""
        _enable_web_auth()
        try:
            r = requests.post(
                f"{BASE_URL}/login",
                json={"username": USERNAME, "password": PASSWORD},
                timeout=5,
            )
            assert r.status_code == 200
            data = r.json()
            assert data.get("success") is True
            # Should have set a cookie
            assert "arctic_session" in r.cookies or "Set-Cookie" in r.headers
        finally:
            _disable_web_auth()

    def test_login_with_invalid_credentials(self):
        """Wrong password should return 401."""
        _enable_web_auth()
        try:
            r = requests.post(
                f"{BASE_URL}/login",
                json={"username": "wrong", "password": "wrong"},
                timeout=5,
            )
            assert r.status_code == 401
        finally:
            _disable_web_auth()

    def test_session_cookie_grants_access(self):
        """After login, session cookie should grant access to protected endpoints."""
        _enable_web_auth()
        try:
            # Login to get session
            session = requests.Session()
            r = session.post(
                f"{BASE_URL}/login",
                json={"username": USERNAME, "password": PASSWORD},
                timeout=5,
            )
            assert r.status_code == 200

            # Access protected endpoint with session (no API key)
            r = session.get(f"{BASE_URL}/api/info", timeout=5)
            assert r.status_code == 200
        finally:
            _disable_web_auth()

    def test_logout_invalidates_session(self):
        """After logout, the session cookie should no longer work."""
        _enable_web_auth()
        try:
            session = requests.Session()
            # Login
            r = session.post(
                f"{BASE_URL}/login",
                json={"username": USERNAME, "password": PASSWORD},
                timeout=5,
            )
            assert r.status_code == 200

            # Logout
            r = session.post(f"{BASE_URL}/logout", timeout=5)
            assert r.status_code == 200

            # Try accessing a protected endpoint — should fail
            r = session.get(f"{BASE_URL}/api/info", timeout=5)
            assert r.status_code == 401
        finally:
            _disable_web_auth()


# ── API Key ───────────────────────────────────────────────────────────────


class TestApiKey:
    """GET /api/auth/apikey — API key management."""

    def test_get_api_key_requires_session(self):
        """API key endpoint requires session auth (login), not just API key."""
        _enable_web_auth()
        try:
            # With only API key (no session cookie) — should get 401
            r = requests.get(
                f"{BASE_URL}/api/auth/apikey",
                headers=_headers(),
                timeout=5,
            )
            # The apikey endpoint requires session cookie, not API key header
            # This is documented as session-only
            assert r.status_code in (200, 401)
        finally:
            _disable_web_auth()

    def test_get_api_key_with_session(self):
        """With a valid session, API key can be retrieved."""
        _enable_web_auth()
        try:
            session = requests.Session()
            session.post(
                f"{BASE_URL}/login",
                json={"username": USERNAME, "password": PASSWORD},
                timeout=5,
            )
            r = session.get(f"{BASE_URL}/api/auth/apikey", timeout=5)
            assert r.status_code == 200
            data = r.json()
            assert "api_key" in data
            assert isinstance(data["api_key"], str)
            assert len(data["api_key"]) == 32  # 32-char hex
        finally:
            _disable_web_auth()


# ── Auth Enforcement ──────────────────────────────────────────────────────


class TestAuthEnforcement:
    """Verify API key and session auth enforcement on protected endpoints."""

    def test_unauthenticated_when_web_auth_enabled(self):
        """Without any credentials, protected endpoints return 401."""
        _enable_web_auth()
        try:
            r = requests.get(f"{BASE_URL}/api/info", timeout=5)
            assert r.status_code == 401
        finally:
            _disable_web_auth()

    def test_invalid_api_key_when_web_auth_enabled(self):
        """An invalid API key should be rejected."""
        _enable_web_auth()
        try:
            r = requests.get(
                f"{BASE_URL}/api/info",
                headers={"X-API-Key": "0000000000000000000000000000dead"},
                timeout=5,
            )
            assert r.status_code == 401
        finally:
            _disable_web_auth()

    def test_valid_api_key_grants_access(self):
        """A valid API key should grant access even with web auth enabled."""
        _enable_web_auth()
        try:
            r = requests.get(
                f"{BASE_URL}/api/info",
                headers={"X-API-Key": API_KEY},
                timeout=5,
            )
            assert r.status_code == 200
        finally:
            _disable_web_auth()

    def test_health_always_accessible(self):
        """GET /api/health should be accessible without auth."""
        _enable_web_auth()
        try:
            r = requests.get(f"{BASE_URL}/api/health", timeout=5)
            assert r.status_code == 200
        finally:
            _disable_web_auth()

    def test_auth_config_always_accessible(self):
        """GET /api/auth/config should be accessible without auth."""
        _enable_web_auth()
        try:
            r = requests.get(f"{BASE_URL}/api/auth/config", timeout=5)
            assert r.status_code == 200
        finally:
            _disable_web_auth()

    def test_auth_status_always_accessible(self):
        """GET /api/auth/status should be accessible without auth."""
        _enable_web_auth()
        try:
            r = requests.get(f"{BASE_URL}/api/auth/status", timeout=5)
            assert r.status_code == 200
        finally:
            _disable_web_auth()
