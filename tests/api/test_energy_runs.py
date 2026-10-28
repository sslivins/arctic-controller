"""GET /api/energy/runs: per-run COP and outdoor temperature.

The temperature-history fixture also stores summaries for three of its four
runs (test firmware); this checks they come back with the right figures and
line up with the runs in the telemetry.
"""

import os
import time

import pytest
import requests
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")


@pytest.fixture(scope="module")
def api():
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    s = requests.Session()
    s.verify = False
    s.headers["X-API-Key"] = API_KEY
    yield s
    s.close()


def _runs(api, query=""):
    r = api.get(f"{BASE_URL}/api/energy/runs{query}", timeout=10)
    assert r.status_code == 200, r.text
    return r.json()


def test_seeded_runs_report_cop_and_outdoor(api):
    r = api.post(f"{BASE_URL}/api/test/populate-temperature-history", timeout=30)
    r.raise_for_status()
    assert r.json()["runs"] == 3

    now = int(time.time())
    body = _runs(api, f"?from={now - 9 * 3600}&to={now}")
    seeded = [x for x in body["runs"] if not x["open"] and x["electrical_wh"] == 3100]
    assert [(x["mode"], x["cop"], x["outdoor_c"]) for x in seeded] == [
        ("heating", 3.1, -3.5),
        ("cooling", 2.6, 24.1),
        ("hot_water", 2.3, 5.2),
    ]
    starts = [x["start"] for x in body["runs"]]
    assert starts == sorted(starts)
    for x in seeded:
        assert x["end"] - x["start"] == 160 * 30
        assert x["outdoor_seconds"] == 4500

    # The default range is the last 24 h, which holds all of them.
    assert len([x for x in _runs(api)["runs"] if x["electrical_wh"] == 3100]) == 3


def test_range_filters_and_rejects_bad_input(api):
    now = int(time.time())
    body = _runs(api, f"?from={now - 60}&to={now - 30}")
    assert all(not (x["end"] <= now - 60 or x["start"] >= now - 30) for x in body["runs"])
    for bad in ("?from=abc", "?to=-5", "?from=0"):
        r = api.get(f"{BASE_URL}/api/energy/runs{bad}", timeout=10)
        assert r.status_code == 400, bad
