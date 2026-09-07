"""
Functional tests for the location and weather API endpoints.

Covers:
  - GET/POST /api/location — device location and automatic-timezone mode
  - GET /api/location/search — Open-Meteo geocoding lookup
  - GET /api/weather — cached outdoor conditions

Geocoding and weather are both driven through the test-only mock endpoints
(/api/test/geocoding-mock, /api/test/weather-mock), so nothing here depends on
outbound internet access or on the real weather at the device's location.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
  - Firmware built with CONFIG_TEST_ENDPOINTS=y
"""

import os
import time

import pytest
import requests
import urllib3
from pathlib import Path
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# Load .env from repo root if present (local dev)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

_OrigSessionInit = requests.Session.__init__
def _session_init_no_verify(self, *args, **kwargs):
    _OrigSessionInit(self, *args, **kwargs)
    self.verify = False
requests.Session.__init__ = _session_init_no_verify

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

_session = requests.Session()
_retry = Retry(total=3, backoff_factor=1, allowed_methods=None,
               status_forcelist=[502, 503, 504])
_session.mount("http://", HTTPAdapter(max_retries=_retry))
_session.mount("https://", HTTPAdapter(max_retries=_retry))

# A geocoding response with two distinguishable hits, so tests can assert on
# ordering and on the full label rather than just "something came back".
GEOCODING_BODY = {
    "results": [
        {
            "name": "Kamloops",
            "admin1": "British Columbia",
            "country": "Canada",
            "country_code": "CA",
            "timezone": "America/Vancouver",
            "latitude": 50.6745,
            "longitude": -120.3273,
        },
        {
            "name": "Kamloops Lake",
            "admin1": "British Columbia",
            "country": "Canada",
            "country_code": "CA",
            "timezone": "America/Vancouver",
            "latitude": 50.7500,
            "longitude": -120.6500,
        },
    ]
}


def _headers():
    h = {}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    return h


def _get(path, **kwargs):
    return _session.get(f"{BASE_URL}{path}", headers=_headers(), timeout=20, **kwargs)


def _post(path, json=None, **kwargs):
    return _session.post(f"{BASE_URL}{path}", headers=_headers(), json=json, timeout=20, **kwargs)


@pytest.fixture(scope="module", autouse=True)
def _check_prerequisites():
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    last_err = None
    for attempt in range(3):
        try:
            r = requests.get(f"{BASE_URL}/api/health", timeout=5)
            r.raise_for_status()
            break
        except Exception as e:  # noqa: BLE001 - reachability probe
            last_err = e
            if attempt < 2:
                time.sleep(2)
    else:
        pytest.skip(f"Device not reachable at {BASE_URL}: {last_err}")

    # The test-only mocks live behind CONFIG_TEST_ENDPOINTS; without them these
    # tests would silently hit the real internet and become flaky.
    probe = _post("/api/test/geocoding-mock-reset")
    if probe.status_code == 404:
        pytest.skip("Firmware not built with CONFIG_TEST_ENDPOINTS")


@pytest.fixture(autouse=True)
def _restore_location():
    """Put the device's real location and timezone mode back after each test.

    There is one physical controller and the rest of the suite reads its clock,
    so a test that leaves it in Kamloops with a mocked geocoder would poison
    everything downstream.
    """
    original = _get("/api/location").json()
    yield
    _post("/api/test/geocoding-mock-reset")
    if original.get("valid"):
        _post("/api/location", {
            "latitude": original["latitude"],
            "longitude": original["longitude"],
            "name": original.get("name", ""),
            "iana_tz": original.get("iana_tz", ""),
            "tz_auto": original.get("tz_auto", True),
        })
    else:
        _post("/api/location", {"tz_auto": original.get("tz_auto", True)})


class TestLocationGet:
    """GET /api/location."""

    def test_returns_200(self):
        assert _get("/api/location").status_code == 200

    def test_response_structure(self):
        data = _get("/api/location").json()
        for key in ("valid", "latitude", "longitude", "name",
                    "iana_tz", "tz_auto", "derived_posix"):
            assert key in data, f"missing {key}"
        assert isinstance(data["valid"], bool)
        assert isinstance(data["tz_auto"], bool)

    def test_requires_api_key(self):
        r = _session.get(f"{BASE_URL}/api/location", timeout=20)
        assert r.status_code == 401


class TestLocationSet:
    """POST /api/location."""

    def test_sets_coordinates_and_name(self):
        r = _post("/api/location", {
            "latitude": 50.6745, "longitude": -120.3273,
            "name": "Kamloops, British Columbia, CA",
            "iana_tz": "America/Vancouver",
        })
        assert r.status_code == 200
        body = r.json()
        assert body["success"] is True
        assert body["valid"] is True
        assert body["name"] == "Kamloops, British Columbia, CA"
        assert abs(body["latitude"] - 50.6745) < 0.001

        # And it is persisted, not just echoed back.
        after = _get("/api/location").json()
        assert after["name"] == "Kamloops, British Columbia, CA"
        assert after["iana_tz"] == "America/Vancouver"

    def test_tz_auto_can_be_toggled_on_its_own(self):
        off = _post("/api/location", {"tz_auto": False})
        assert off.status_code == 200
        assert off.json()["tz_auto"] is False
        assert _get("/api/location").json()["tz_auto"] is False

        on = _post("/api/location", {"tz_auto": True})
        assert on.status_code == 200
        assert on.json()["tz_auto"] is True

    def test_tz_auto_derives_a_posix_zone_from_the_location(self):
        body = _post("/api/location", {
            "latitude": 50.6745, "longitude": -120.3273,
            "name": "Kamloops", "iana_tz": "America/Vancouver",
            "tz_auto": True,
        }).json()
        assert body["tz_auto"] is True
        # America/Vancouver is a mapped zone, so a POSIX string must come back.
        assert body["derived_posix"], "expected a derived POSIX TZ string"

    def test_empty_body_is_rejected(self):
        r = _post("/api/location", {})
        assert r.status_code == 400

    def test_latitude_only_is_rejected(self):
        # Half a coordinate pair would otherwise be silently ignored.
        r = _post("/api/location", {"latitude": 50.6745})
        assert r.status_code == 400

    @pytest.mark.parametrize("lat,lon", [
        (91.0, 0.0), (-91.0, 0.0), (0.0, 181.0), (0.0, -181.0),
    ])
    def test_out_of_range_coordinates_are_rejected(self, lat, lon):
        r = _post("/api/location", {"latitude": lat, "longitude": lon})
        assert r.status_code == 400

    def test_requires_api_key(self):
        r = _session.post(f"{BASE_URL}/api/location", json={"tz_auto": True}, timeout=20)
        assert r.status_code == 401


class TestLocationSearch:
    """GET /api/location/search."""

    def test_returns_mocked_results(self):
        assert _post("/api/test/geocoding-mock", GEOCODING_BODY).status_code == 200
        r = _get("/api/location/search", params={"q": "kamloops"})
        assert r.status_code == 200
        results = r.json()["results"]
        assert len(results) == 2
        assert results[0]["name"] == "Kamloops"
        assert results[0]["iana_tz"] == "America/Vancouver"
        assert abs(results[0]["latitude"] - 50.6745) < 0.001

    def test_results_carry_a_display_label(self):
        assert _post("/api/test/geocoding-mock", GEOCODING_BODY).status_code == 200
        results = _get("/api/location/search", params={"q": "kamloops"}).json()["results"]
        assert results[0]["label"] == "Kamloops, British Columbia, CA"

    def test_multi_word_query_is_decoded(self):
        # The query arrives percent-encoded; a decoding bug would search for
        # "kamloops%20lake" and the mock would not notice, so assert the
        # request is at least accepted and answered.
        assert _post("/api/test/geocoding-mock", GEOCODING_BODY).status_code == 200
        r = _get("/api/location/search", params={"q": "kamloops lake"})
        assert r.status_code == 200
        assert len(r.json()["results"]) == 2

    def test_missing_query_is_rejected(self):
        assert _get("/api/location/search").status_code == 400

    def test_empty_query_is_rejected(self):
        assert _get("/api/location/search", params={"q": ""}).status_code == 400

    def test_network_failure_reports_bad_gateway(self):
        assert _post("/api/test/geocoding-mock", {"__error__": True}).status_code == 200
        r = _get("/api/location/search", params={"q": "kamloops"})
        assert r.status_code == 502

    def test_requires_api_key(self):
        r = _session.get(f"{BASE_URL}/api/location/search?q=kamloops", timeout=20)
        assert r.status_code == 401


class TestWeather:
    """GET /api/weather."""

    @staticmethod
    def _await_weather(predicate, timeout=15.0):
        """Poll /api/weather until predicate holds.

        The mock endpoint kicks off a refresh on a worker task, so the new
        reading lands a moment after the POST returns.
        """
        deadline = time.monotonic() + timeout
        latest = None
        while time.monotonic() < deadline:
            latest = _get("/api/weather").json()
            if predicate(latest):
                return latest
            time.sleep(0.5)
        pytest.fail(f"weather never satisfied the predicate; last was {latest}")

    def test_returns_200(self):
        assert _get("/api/weather").status_code == 200

    def test_reports_the_mocked_reading(self):
        assert _post("/api/test/weather-mock",
                     {"current": {"temperature_2m": -5.0, "weather_code": 71}}).status_code == 200
        data = self._await_weather(lambda d: d.get("valid") and d.get("weather_code") == 71)
        assert abs(data["temp_c"] - (-5.0)) < 0.1
        assert data["description"] == "Snow"

    def test_description_matches_the_code(self):
        assert _post("/api/test/weather-mock",
                     {"current": {"temperature_2m": 21.5, "weather_code": 0}}).status_code == 200
        data = self._await_weather(lambda d: d.get("valid") and d.get("weather_code") == 0)
        assert data["description"] == "Clear"

    def test_invalid_reading_omits_the_measurements(self):
        # When there is nothing to report the payload must not carry a
        # placeholder temperature that a client would render as real.
        data = _get("/api/weather").json()
        assert "valid" in data
        if not data["valid"]:
            assert "temp_c" not in data
            assert "weather_code" not in data

    def test_requires_api_key(self):
        r = _session.get(f"{BASE_URL}/api/weather", timeout=20)
        assert r.status_code == 401

    @pytest.fixture(autouse=True)
    def _clear_weather_mock(self):
        yield
        _post("/api/test/weather-mock-reset")
