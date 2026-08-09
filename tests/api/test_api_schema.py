"""
API contract tests — validate production API responses against OpenAPI spec.

Uses Schemathesis to verify that every response from the device matches
the documented schema in docs/openapi.yaml.  Only safe (read-only)
endpoints are tested automatically; dangerous endpoints (OTA, reboot,
credential changes) are skipped.

These tests are separate from UI tests and focus exclusively on API
contract compliance.  They do NOT test the test instrumentation API
and do NOT depend on the device UI test infrastructure (conftest/DeviceClient).

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - pip install schemathesis

Run:
  pytest tests/api/ -v
"""

import gc
import os
import time

import pytest
import requests
import schemathesis
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
import yaml
from hypothesis import HealthCheck, Phase, assume, settings
from pathlib import Path

# Reduce memory footprint for constrained runners (Pi Zero 2 W = 512 MB).
# Disable the hypothesis example database (no caching to disk/memory) and
# use derandomize mode so runs are reproducible without storing state.
settings.register_profile(
    "ci",
    max_examples=5,
    database=None,
    deadline=None,
    derandomize=True,
    phases=[Phase.explicit, Phase.generate],  # skip shrinking to save RAM
    suppress_health_check=list(HealthCheck),
)
settings.load_profile(os.environ.get("HYPOTHESIS_PROFILE", "ci"))

# Load .env from repo root if present (local dev — never committed)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

# Import probe checks we want to exclude — ESP-IDF's httpd doesn't implement
# RFC-strict Allow headers on 405, and returns 401 (not 406) when auth headers
# are stripped.  Both are correct device behavior.
from schemathesis.checks import load_all_checks
load_all_checks()
from schemathesis.checks import CHECKS
_EXCLUDED_PROBES = [
    check for check in CHECKS.get_all()
    if check.__name__ in (
        "missing_required_header",  # 401 instead of 406 — correct for ESP-IDF
        "unsupported_method",       # 405 without Allow header — ESP-IDF httpd limitation
        "ignored_auth",             # API key alone is sufficient, cookie not required
        "negative_data_rejection",  # ESP-IDF httpd ignores unknown params/cookies by design
    )
]

# ── Configuration ─────────────────────────────────────────────────────────

SPEC_PATH = Path(__file__).resolve().parent.parent.parent / "docs" / "openapi.yaml"
BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# Load spec and override servers to point at the real device
with open(SPEC_PATH, encoding="utf-8") as f:
    raw_spec = yaml.safe_load(f)
raw_spec["servers"] = [{"url": BASE_URL}]

schema = schemathesis.openapi.from_dict(raw_spec)

# ── Safety filters ────────────────────────────────────────────────────────
# These endpoints are either dangerous or change persistent device state.
# They are excluded from automatic fuzz testing.

SKIP_ENTIRELY = {
    "/api/ota/releases",            # depends on live GitHub connectivity; covered by targeted tests
    "/api/ota/update",              # triggers firmware download
    "/api/ota/upload",              # writes firmware to flash
    "/api/ota/reboot",              # reboots the device
    "/api/ota/github",              # installs the latest GitHub release
    "/api/auth/credentials",        # changes login credentials
    "/api/auth/apikey/regenerate",  # invalidates existing API key
    "/api/factory-reset",           # erases all persistent state and reboots
    "/api/wifi/connect",            # changes the active network
    "/api/wifi/disconnect",         # drops the test connection
    "/api/heatpump/errors/history", # clears persisted error history
    "/api/heatpump/demo",           # mutates simulated sensor fields
    "/api/brownout/clear",          # clears persistent boot statistics
    "/login",                       # session management
    "/logout",                      # session management
}

# Only GET is safe — POST/PUT/DELETE changes persistent state or triggers actions
SKIP_NON_GET = {
    "/api/auth/config",             # POST changes auth settings
    "/api/time/config",             # POST changes timezone/format
    "/api/time/sync",               # POST triggers NTP sync
    "/api/events",                  # DELETE clears event log
    "/api/logs",                    # DELETE clears log buffer
    "/api/heatpump/power",          # PUT toggles power
    "/api/heatpump/mode",           # PUT changes operating mode
    "/api/heatpump/setpoints",      # PUT changes temperature setpoints
    "/api/heatpump/advanced/{id}",  # PUT changes AP (advanced) parameters
    "/api/display/brightness",      # PUT changes persistent display settings
    "/api/preferences",             # PATCH changes persistent preferences
    "/api/wifi/scan",               # POST starts radio scan
}


# ── Shared auth session ───────────────────────────────────────────────────
# Authenticate once at module load so both Schemathesis and smoke tests
# can reuse the session cookie.

_auth_session = requests.Session()
_auth_session.verify = False

def _setup_auth():
    """Log in if web auth is enabled, or set API key header."""
    if API_KEY:
        _auth_session.headers["X-API-Key"] = API_KEY
    for attempt in range(3):
        try:
            auth_cfg = _auth_session.get(f"{BASE_URL}/api/auth/config", timeout=5).json()
            if auth_cfg.get("web_auth_enabled") and not auth_cfg.get("authenticated"):
                username = os.environ.get("ARCTIC_USERNAME", "arctic")
                password = os.environ.get("ARCTIC_PASSWORD", "arctic")
                _auth_session.post(
                    f"{BASE_URL}/login",
                    json={"username": username, "password": password},
                    timeout=5,
                )
            return
        except Exception:
            if attempt < 2:
                time.sleep(2)

_setup_auth()


# ── Tier 1: Automatic schema validation ──────────────────────────────────
# Schemathesis generates test cases for every endpoint × method combination
# and validates that responses conform to the documented OpenAPI schema.
# Each endpoint gets up to 5 hypothesis-generated parameter variations.


@schema.parametrize()
def test_production_api_schema(case):
    """Response from every safe endpoint must match the OpenAPI schema."""
    # Skip entirely for dangerous endpoints (all methods)
    if case.path in SKIP_ENTIRELY:
        pytest.skip(f"dangerous endpoint: {case.path}")

    # For endpoints where only GET is safe:
    if case.path in SKIP_NON_GET:
        spec_method = case.operation.method.upper()
        if spec_method in ("POST", "PUT", "DELETE", "PATCH"):
            # This parametrized test IS a mutating operation — skip it.
            pytest.skip(f"mutating: {spec_method} {case.path}")
        # GET test case, but hypothesis may inject non-GET probes —
        # reject those examples while allowing the real GET examples.
        actual = case.method.upper()
        if actual in ("POST", "PUT", "DELETE", "PATCH"):
            assume(False)

    # Inject auth headers/cookies from the shared session
    if API_KEY:
        case.headers = case.headers or {}
        case.headers["X-API-Key"] = API_KEY

    time.sleep(0.1)  # be gentle on the ESP32
    try:
        case.call_and_validate(
            base_url=BASE_URL,
            session=_auth_session,
            excluded_checks=_EXCLUDED_PROBES,
        )
    except requests.exceptions.ConnectionError:
        pytest.skip(f"Device unreachable at {BASE_URL}")
    except BaseException as exc:
        # Schemathesis generates probe requests that strip auth. The device
        # correctly returns 401, but that status code isn't documented per-
        # endpoint in the spec (it's a global auth concern).  Skip if ALL
        # sub-exceptions are 401-related UndefinedStatusCode.
        from schemathesis.openapi.checks import UndefinedStatusCode
        sub = getattr(exc, "exceptions", None)
        if sub and all(
            isinstance(e, UndefinedStatusCode) and "401" in str(e)
            for e in sub
        ):
            pytest.skip("Undocumented 401 — auth probe, not a schema issue")
        raise


# ── Tier 2: Targeted smoke tests ─────────────────────────────────────────
# These verify specific response values beyond just schema shape.
# Schemathesis validates structure; these check semantics.


@pytest.fixture(scope="module")
def api():
    """Authenticated requests session for manual API smoke tests."""
    last_err = None
    for attempt in range(3):
        try:
            r = _auth_session.get(f"{BASE_URL}/api/health", timeout=5)
            r.raise_for_status()
            return _auth_session
        except Exception as e:
            last_err = e
            if attempt < 2:
                time.sleep(2)
    pytest.skip(f"Device not reachable at {BASE_URL}: {last_err}")


def test_health_returns_ok(api):
    """GET /api/health — status must be 'ok', uptime must be positive."""
    r = api.get(f"{BASE_URL}/api/health", timeout=5)
    assert r.status_code == 200
    data = r.json()
    assert data["status"] == "ok"
    assert data["uptime_ms"] > 0


def test_status_structure(api):
    """GET /api/status — must contain device, wifi, and time sections."""
    r = api.get(f"{BASE_URL}/api/status", timeout=5)
    if r.status_code == 401:
        pytest.skip("Auth required — set ARCTIC_API_KEY or ARCTIC_USERNAME/ARCTIC_PASSWORD")
    assert r.status_code == 200
    data = r.json()
    assert "device" in data
    assert "uptime_ms" in data
    assert "wifi" in data
    assert data["wifi"]["state"] in [
        "not_initialized", "disconnected", "connecting", "connected", "error"
    ]


def test_info_has_version(api):
    """GET /api/info — must include version and platform."""
    r = api.get(f"{BASE_URL}/api/info", timeout=5)
    if r.status_code == 401:
        pytest.skip("API auth enabled — set ARCTIC_API_KEY")
    assert r.status_code == 200
    data = r.json()
    assert "version" in data
    assert "platform" in data
    assert data["platform"] == "ESP32-P4"


def test_ota_status_idle(api):
    """GET /api/ota/status — state should be 'idle' when no update in progress."""
    r = api.get(f"{BASE_URL}/api/ota/status", timeout=5)
    if r.status_code == 401:
        pytest.skip("Auth required — set ARCTIC_API_KEY or ARCTIC_USERNAME/ARCTIC_PASSWORD")
    assert r.status_code == 200
    data = r.json()
    assert data["state"] == "idle"
    assert data["progress"] == 0


def test_time_has_epoch(api):
    """GET /api/time — must return epoch as a number."""
    r = api.get(f"{BASE_URL}/api/time", timeout=5)
    if r.status_code == 401:
        pytest.skip("Auth required — set ARCTIC_API_KEY or ARCTIC_USERNAME/ARCTIC_PASSWORD")
    assert r.status_code == 200
    data = r.json()
    assert "epoch" in data
    assert isinstance(data["epoch"], (int, float))


def test_heatpump_status_structure(api):
    """GET /api/heatpump/status — must have connected field and temp data."""
    r = api.get(f"{BASE_URL}/api/heatpump/status", timeout=5)
    if r.status_code == 401:
        pytest.skip("API auth enabled — set ARCTIC_API_KEY")
    assert r.status_code == 200
    data = r.json()
    assert "connected" in data
    if data["connected"] or data.get("demo_mode"):
        assert "temperatures" in data
        assert "setpoints" in data


def test_events_structure(api):
    """GET /api/events — must have total count and events array."""
    r = api.get(f"{BASE_URL}/api/events", timeout=5)
    if r.status_code == 401:
        pytest.skip("API auth enabled — set ARCTIC_API_KEY")
    assert r.status_code == 200
    data = r.json()
    assert "total" in data
    assert "events" in data
    assert isinstance(data["events"], list)
    assert data["offset"] == 0
    assert data["count"] == len(data["events"])
    assert "current_boot_id" in data
    for event in data["events"]:
        assert event["category"] in ("problems", "equipment", "changes", "system")
        assert isinstance(event["boot_id"], int)


def test_events_supports_pagination(api):
    """GET /api/events — offset and limit bound the returned page."""
    r = api.get(
        f"{BASE_URL}/api/events",
        params={"offset": 1, "limit": 2},
        timeout=5,
    )
    if r.status_code == 401:
        pytest.skip("API auth enabled — set ARCTIC_API_KEY")
    assert r.status_code == 200
    data = r.json()
    assert data["offset"] == 1
    assert data["count"] == len(data["events"])
    assert len(data["events"]) <= 2


def test_logs_supports_filtering(api):
    """GET /api/logs — query params since, level, limit must work."""
    r = api.get(
        f"{BASE_URL}/api/logs",
        params={"since": 0, "level": "I", "limit": 5},
        timeout=5,
    )
    if r.status_code == 401:
        pytest.skip("API auth enabled — set ARCTIC_API_KEY")
    assert r.status_code == 200
    data = r.json()
    assert "total" in data
    assert "latest_seq" in data
    assert "entries" in data
    assert len(data["entries"]) <= 5
