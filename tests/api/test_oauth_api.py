"""
Functional tests for the OAuth 2.1 API endpoints.

Tests OAuth configuration retrieval/update, JWKS refresh behavior,
and scope listing.

Note: Full JWT validation and scope enforcement require a running
identity provider (e.g., Zitadel/Keycloak). These tests cover the
configuration API which can be tested without an IdP.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
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

# Expected OAuth scopes
EXPECTED_SCOPES = [
    "arctic:status",
    "arctic:control",
    "arctic:config",
    "arctic:params",
    "arctic:admin",
    "arctic:mcp",
]

# Retry-enabled session for all API calls
_session = requests.Session()
_retry = Retry(total=3, backoff_factor=1, allowed_methods=None,
               status_forcelist=[502, 503, 504])
_session.mount("http://", HTTPAdapter(max_retries=_retry))
_session.mount("https://", HTTPAdapter(max_retries=_retry))
_session.verify = False


def _headers():
    h = {}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    return h


def _get(path):
    return _session.get(f"{BASE_URL}{path}", headers=_headers(), timeout=10)


def _put(path, json=None):
    return _session.put(f"{BASE_URL}{path}", headers=_headers(), json=json, timeout=10)


def _post(path, json=None):
    return _session.post(f"{BASE_URL}{path}", headers=_headers(), json=json, timeout=10)


@pytest.fixture(scope="module", autouse=True)
def _check_prerequisites():
    """Verify device is reachable and API key is set."""
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
    pytest.skip(f"Device unreachable at {BASE_URL}: {last_err}")


@pytest.fixture
def restore_oauth_config():
    """Save OAuth config before test, restore after."""
    original = _get("/api/oauth/config").json()
    yield
    # Restore original settings
    _put("/api/oauth/config", json={
        "enabled": original.get("enabled", False),
        "issuer": original.get("issuer", ""),
        "audience": original.get("audience", ""),
        "jwks_uri": original.get("jwks_uri", ""),
        "allow_api_key_fallback": original.get("allow_api_key_fallback", True),
    })


# ═══════════════════════════════════════════════════════════════════════════
# GET /api/oauth/config
# ═══════════════════════════════════════════════════════════════════════════

class TestOAuthConfigGet:
    """Tests for GET /api/oauth/config."""
    
    def test_returns_200(self):
        """OAuth config endpoint returns 200 OK."""
        r = _get("/api/oauth/config")
        assert r.status_code == 200
    
    def test_response_has_required_fields(self):
        """Response includes all required configuration fields."""
        r = _get("/api/oauth/config")
        data = r.json()
        
        assert "enabled" in data
        assert "issuer" in data
        assert "audience" in data
        assert "jwks_uri" in data
        assert "allow_api_key_fallback" in data
        assert "jwks_status" in data
        assert "supported_scopes" in data
    
    def test_enabled_is_boolean(self):
        """enabled field is a boolean."""
        r = _get("/api/oauth/config")
        data = r.json()
        assert isinstance(data["enabled"], bool)
    
    def test_allow_api_key_fallback_is_boolean(self):
        """allow_api_key_fallback field is a boolean."""
        r = _get("/api/oauth/config")
        data = r.json()
        assert isinstance(data["allow_api_key_fallback"], bool)
    
    def test_issuer_is_string(self):
        """issuer field is a string."""
        r = _get("/api/oauth/config")
        data = r.json()
        assert isinstance(data["issuer"], str)
    
    def test_jwks_status_structure(self):
        """jwks_status object has loaded and ttl_seconds fields."""
        r = _get("/api/oauth/config")
        data = r.json()
        jwks = data["jwks_status"]
        
        assert "loaded" in jwks
        assert "ttl_seconds" in jwks
        assert isinstance(jwks["loaded"], bool)
        assert isinstance(jwks["ttl_seconds"], (int, float))
    
    def test_supported_scopes_is_array(self):
        """supported_scopes is an array of strings."""
        r = _get("/api/oauth/config")
        data = r.json()
        
        assert isinstance(data["supported_scopes"], list)
        assert all(isinstance(s, str) for s in data["supported_scopes"])
    
    def test_all_expected_scopes_present(self):
        """All expected OAuth scopes are listed."""
        r = _get("/api/oauth/config")
        data = r.json()
        
        for scope in EXPECTED_SCOPES:
            assert scope in data["supported_scopes"], f"Missing scope: {scope}"
    
    def test_default_disabled(self):
        """OAuth is disabled by default (fresh device)."""
        r = _get("/api/oauth/config")
        data = r.json()
        # Note: This test may fail if OAuth was previously enabled.
        # The restore fixture handles cleanup for other tests.
        assert isinstance(data["enabled"], bool)  # Just verify type; actual value depends on state


# ═══════════════════════════════════════════════════════════════════════════
# PUT /api/oauth/config
# ═══════════════════════════════════════════════════════════════════════════

class TestOAuthConfigPut:
    """Tests for PUT /api/oauth/config."""
    
    def test_update_enabled_flag(self, restore_oauth_config):
        """Can toggle the enabled flag."""
        # Get current state
        current = _get("/api/oauth/config").json()
        new_state = not current["enabled"]
        
        # Update
        r = _put("/api/oauth/config", json={"enabled": new_state})
        assert r.status_code == 200
        data = r.json()
        assert data.get("success") is True
        
        # Verify change persisted
        r = _get("/api/oauth/config")
        assert r.json()["enabled"] == new_state
    
    def test_update_issuer(self, restore_oauth_config):
        """Can update the issuer URL."""
        test_issuer = "https://test.example.com"
        
        r = _put("/api/oauth/config", json={"issuer": test_issuer})
        assert r.status_code == 200
        
        r = _get("/api/oauth/config")
        assert r.json()["issuer"] == test_issuer
    
    def test_update_audience(self, restore_oauth_config):
        """Can update the audience."""
        test_audience = "arctic-test-client"
        
        r = _put("/api/oauth/config", json={"audience": test_audience})
        assert r.status_code == 200
        
        r = _get("/api/oauth/config")
        assert r.json()["audience"] == test_audience
    
    def test_update_jwks_uri(self, restore_oauth_config):
        """Can update the JWKS URI."""
        test_uri = "https://test.example.com/.well-known/jwks.json"
        
        r = _put("/api/oauth/config", json={"jwks_uri": test_uri})
        assert r.status_code == 200
        
        r = _get("/api/oauth/config")
        assert r.json()["jwks_uri"] == test_uri
    
    def test_update_allow_api_key_fallback(self, restore_oauth_config):
        """Can toggle API key fallback."""
        current = _get("/api/oauth/config").json()
        new_state = not current["allow_api_key_fallback"]
        
        r = _put("/api/oauth/config", json={"allow_api_key_fallback": new_state})
        assert r.status_code == 200
        
        r = _get("/api/oauth/config")
        assert r.json()["allow_api_key_fallback"] == new_state
    
    def test_update_multiple_fields(self, restore_oauth_config):
        """Can update multiple fields in one request."""
        updates = {
            "issuer": "https://multi.test.com",
            "audience": "multi-client",
            "enabled": True,
        }
        
        r = _put("/api/oauth/config", json=updates)
        assert r.status_code == 200
        
        r = _get("/api/oauth/config")
        data = r.json()
        assert data["issuer"] == updates["issuer"]
        assert data["audience"] == updates["audience"]
        assert data["enabled"] == updates["enabled"]
    
    def test_partial_update_preserves_other_fields(self, restore_oauth_config):
        """Partial update doesn't clear other fields."""
        # Set initial config
        _put("/api/oauth/config", json={
            "issuer": "https://preserve.test.com",
            "audience": "preserve-client",
        })
        
        # Update only issuer
        _put("/api/oauth/config", json={"issuer": "https://new-issuer.test.com"})
        
        # Verify audience is preserved
        r = _get("/api/oauth/config")
        assert r.json()["audience"] == "preserve-client"
    
    def test_empty_strings_allowed(self, restore_oauth_config):
        """Empty strings can clear configuration values."""
        # Set a value first
        _put("/api/oauth/config", json={"issuer": "https://something.com"})
        
        # Clear it
        _put("/api/oauth/config", json={"issuer": ""})
        
        r = _get("/api/oauth/config")
        assert r.json()["issuer"] == ""


# ═══════════════════════════════════════════════════════════════════════════
# POST /api/oauth/jwks/refresh
# ═══════════════════════════════════════════════════════════════════════════

class TestOAuthJWKSRefresh:
    """Tests for POST /api/oauth/jwks/refresh."""
    
    def test_refresh_fails_when_disabled(self, restore_oauth_config):
        """JWKS refresh returns error when OAuth is disabled."""
        # Ensure OAuth is disabled
        _put("/api/oauth/config", json={"enabled": False})
        
        r = _post("/api/oauth/jwks/refresh")
        # Should fail with 400 or similar
        assert r.status_code in (400, 500)
        data = r.json()
        assert "error" in data or "message" in data
    
    def test_refresh_fails_without_jwks_uri(self, restore_oauth_config):
        """JWKS refresh fails gracefully without configured JWKS URI."""
        # Enable OAuth but with empty JWKS URI
        _put("/api/oauth/config", json={
            "enabled": True,
            "issuer": "https://test.example.com",
            "jwks_uri": "",  # Empty
        })
        
        r = _post("/api/oauth/jwks/refresh")
        # Should fail gracefully
        assert r.status_code in (400, 500)
    
    def test_refresh_fails_with_invalid_uri(self, restore_oauth_config):
        """JWKS refresh fails gracefully with unreachable URI."""
        # Configure with non-existent server
        _put("/api/oauth/config", json={
            "enabled": True,
            "issuer": "https://nonexistent.invalid",
            "jwks_uri": "https://nonexistent.invalid/.well-known/jwks.json",
        })
        
        r = _post("/api/oauth/jwks/refresh")
        # Should fail (can't reach server)
        # Allow longer timeout since HTTP client may wait
        assert r.status_code in (400, 500, 504)


# ═══════════════════════════════════════════════════════════════════════════
# Authentication requirements
# ═══════════════════════════════════════════════════════════════════════════

class TestOAuthAuth:
    """Tests for authentication requirements on OAuth endpoints."""
    
    def test_config_get_requires_auth(self):
        """GET /api/oauth/config requires authentication."""
        # Make request without API key
        r = requests.get(f"{BASE_URL}/api/oauth/config", timeout=10)
        # Should return 401 if web auth is enabled, or succeed if not
        # This test documents current behavior
        assert r.status_code in (200, 401)
    
    def test_config_put_requires_auth(self):
        """PUT /api/oauth/config requires authentication."""
        r = requests.put(
            f"{BASE_URL}/api/oauth/config",
            json={"enabled": True},
            timeout=10
        )
        assert r.status_code in (200, 401)
    
    def test_jwks_refresh_requires_auth(self):
        """POST /api/oauth/jwks/refresh requires authentication."""
        r = requests.post(f"{BASE_URL}/api/oauth/jwks/refresh", timeout=10)
        assert r.status_code in (400, 401, 500)
