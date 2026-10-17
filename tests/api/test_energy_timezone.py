"""A timezone change must not close or skip the open energy day.

Field-reproducible on 3.3.0: a timezone east of local time (e.g. Tokyo set from
Vancouver after 08:00) moved the local date to tomorrow, so the ledger closed
today early and opened tomorrow. Switching back left it refusing to reopen the
closed date, so nothing was booked until the real midnight and the device's
"Today" tile showed "--".

The ledger now spans each day by real time (its own midnight to the next), so a
timezone change only relabels the clock. This test moves the local date with a
fixed-offset zone and checks the open day is the same one afterwards and no day
was closed.
"""

import os
import time
from datetime import date, datetime, timedelta, timezone

import pytest
import requests
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# POSIX TZ strings: the sign is inverted (UTC+14 is "-14").
TZ_EAST = "<+14>-14"
TZ_WEST = "<-12>12"
# The ledger books about once a second; give it several readings.
READINGS_S = 5


@pytest.fixture(scope="module")
def api():
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    s = requests.Session()
    s.verify = False
    s.headers["X-API-Key"] = API_KEY
    yield s
    s.close()


def _get(api, path):
    r = api.get(f"{BASE_URL}{path}", timeout=10)
    r.raise_for_status()
    return r.json()


def _set_tz(api, tz):
    r = api.post(f"{BASE_URL}/api/time/config", json={"timezone": tz}, timeout=10)
    assert r.status_code == 200, r.text
    assert _get(api, "/api/time/config")["timezone"] == tz


def _let_ledger_run(api):
    """Wait until the device has run for READINGS_S more seconds. Each probe is
    a network round trip, which paces the loop."""
    start = _get(api, "/api/status")["uptime_ms"]
    deadline = time.monotonic() + READINGS_S + 20
    while time.monotonic() < deadline:
        if _get(api, "/api/status")["uptime_ms"] - start >= READINGS_S * 1000:
            return
    pytest.fail("device uptime did not advance")


def _days(api, local_date: date):
    lo = (local_date - timedelta(days=2)).isoformat()
    hi = (local_date + timedelta(days=2)).isoformat()
    days = _get(api, f"/api/energy/daily?from={lo}&to={hi}")["days"]
    open_days = [d["date"] for d in days if not d["final"]]
    final_days = {d["date"] for d in days if d["final"]}
    return open_days, final_days


def test_timezone_change_keeps_the_open_day(api):
    orig_tz = _get(api, "/api/time/config")["timezone"]
    now = _get(api, "/api/time")
    assert now.get("ntp_synced"), "device clock is not synced"
    local = date.fromisoformat(now["date"])
    hour = int(now["time_24h"][:2])
    wall = datetime.fromisoformat(now["iso"]).replace(tzinfo=timezone.utc)
    offset_h = round((wall.timestamp() - now["epoch"]) / 3600)
    # Pick the side that really moves the local date: east past midnight, or
    # (early in the day) west back before it. One of them always does.
    east = hour + (14 - offset_h) >= 24
    tz = TZ_EAST if east else TZ_WEST

    open_before, final_before = _days(api, local)
    assert len(open_before) == 1, f"expected one open day, got {open_before}"

    try:
        _set_tz(api, tz)
        shifted = date.fromisoformat(_get(api, "/api/time")["date"])
        assert shifted != local, f"{tz} did not change the local date"
        _let_ledger_run(api)
        open_mid, final_mid = _days(api, local)
        assert open_mid == open_before, (
            f"open day changed under {tz}: {open_before} -> {open_mid}")
        assert final_mid == final_before, (
            f"a day was closed under {tz}: {sorted(final_mid - final_before)}")
    finally:
        _set_tz(api, orig_tz)

    _let_ledger_run(api)
    open_after, final_after = _days(api, local)
    assert open_after == open_before, (
        f"open day changed after restoring the timezone: {open_before} -> {open_after}")
    assert final_after == final_before, (
        f"a day was closed after restoring the timezone: {sorted(final_after - final_before)}")
