"""
Test: heat pump problems notification

A new heat pump fault adds ONE "heat pump problems" entry to the status-bar
bell (and /api/notifications). Further faults update the same entry's count
rather than adding entries, tapping it acknowledges it and opens Error Status,
and POST /api/heatpump/fault-notice/clear acknowledges it from the web.

The flash write policy, count limit and restore-on-boot are covered by the
native test main/tests/test_fault_notice.cpp.
"""

import pytest
from device_client import DeviceClient

ITEM_TAG = "notify_item_heatpump_fault"


def _fault_entries(device: DeviceClient) -> list:
    r = device.session.get(f"{device.base_url}/api/notifications", timeout=device.timeout)
    r.raise_for_status()
    return [n for n in r.json()["notifications"] if n["key"] == "heatpump_fault"]


def _wait_for_fault(device: DeviceClient, count: int, code: str) -> dict:
    def ok():
        e = _fault_entries(device)
        return (len(e) == 1 and e[0]["fault"]["count"] == count
                and e[0]["fault"]["latest_code"] == code and code in e[0]["message"])
    device.wait_until(f"bell shows {count} problem(s), latest {code}", ok, timeout=10.0)
    return _fault_entries(device)[0]


def _close_dropdown(device: DeviceClient):
    # The bell toggles, so a dropdown left open by a failed test would make
    # the next tap close it instead of opening it.
    if device.has_widget(tag="notify_title"):
        device.click(tag="notifications")


@pytest.fixture(autouse=True)
def _clean_notice(device: DeviceClient):
    _close_dropdown(device)
    device.notification_mock_reset()
    yield
    _close_dropdown(device)
    for code in ("P06", "E01"):
        try:
            device.inject_fault(code, False)
        except Exception:
            pass
    device.notification_mock_reset()


def test_fault_raises_bell_and_tap_opens_error_status(device: DeviceClient):
    assert _fault_entries(device) == []
    device.inject_fault("P06", True)
    entry = _wait_for_fault(device, 1, "P06")
    assert entry["fault"]["latest_name"], "fault name missing"
    assert entry["fault"]["latest_at"] >= entry["fault"]["first_at"]

    device.click(tag="notifications")
    assert device.wait_for_widget(tag=ITEM_TAG, timeout=5.0), "fault entry not in dropdown"
    texts = [w.text_en or w.text or "" for w in device.widgets]
    assert any("Heat pump problem" in t and "P06" in t for t in texts), \
        f"dropdown should name the fault; texts: {[t for t in texts if t]}"

    device.click(tag=ITEM_TAG)
    assert device.wait_for_screen("errors", timeout=5.0), \
        f"Expected Error Status, got '{device.screen}'"
    device.wait_until("entry acknowledged", lambda: _fault_entries(device) == [], timeout=5.0)

    device.click(tag="errors_close")
    assert device.wait_for_screen("main", timeout=5.0)
    # The fault is still active, but acknowledged: the bell must not re-raise it.
    assert _fault_entries(device) == []


def test_repeat_faults_update_one_entry(device: DeviceClient):
    device.inject_fault("P06", True)
    _wait_for_fault(device, 1, "P06")
    device.inject_fault("P06", False)
    device.inject_fault("E01", True)
    entry = _wait_for_fault(device, 2, "E01")
    assert entry["message"].startswith("2 heat pump problems"), entry["message"]


def test_clear_endpoint_acknowledges(device: DeviceClient):
    device.inject_fault("P06", True)
    _wait_for_fault(device, 1, "P06")
    r = device.session.post(f"{device.base_url}/api/heatpump/fault-notice/clear",
                            timeout=device.timeout)
    r.raise_for_status()
    assert r.json().get("success") is True
    device.wait_until("entry cleared", lambda: _fault_entries(device) == [], timeout=5.0)

    # A new fault after the acknowledge starts a fresh entry at count 1.
    device.inject_fault("E01", True)
    _wait_for_fault(device, 1, "E01")
