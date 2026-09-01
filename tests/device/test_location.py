"""
Test: Location Picker + Automatic Timezone

Verifies the Time & Location screen's location card and its type-to-search
picker (backed by Open-Meteo geocoding), plus the automatic-timezone behaviour:

1. The location card shows the current location and, in automatic mode, the
   timezone derived from it.
2. Searching resolves candidate locations (via a mocked geocoding response so
   the test never depends on the network) and lists them.
3. Selecting a result updates the location, closes the dialog, and — in
   automatic mode — re-derives the timezone (reflected in the preferences API).
4. Turning automatic mode off reveals the manual timezone roller.
5. Empty / failed searches surface a status message instead of results.

Location and timezone are global device state, so every test that mutates them
requests the ``location_restore`` fixture, which restores the device to the
Sun Peaks default (automatic timezone) on teardown (F-10). The geocoding mock
is installed per-test and cleared automatically when leaving the time screen
(see conftest ``_return_to_main``).
"""

import pytest
from device_client import DeviceClient
from conftest import _return_to_main


# ---- Canned geocoding results (Open-Meteo shape) -------------------------
SUN_PEAKS = {
    "name": "Sun Peaks", "admin1": "British Columbia", "country_code": "CA",
    "timezone": "America/Vancouver", "latitude": 50.8762, "longitude": -119.91075,
}
KAMLOOPS = {
    "name": "Kamloops", "admin1": "British Columbia", "country_code": "CA",
    "timezone": "America/Vancouver", "latitude": 50.6745, "longitude": -120.3273,
}
TORONTO = {
    "name": "Toronto", "admin1": "Ontario", "country_code": "CA",
    "timezone": "America/Toronto", "latitude": 43.7001, "longitude": -79.4163,
}
TOKYO = {
    "name": "Tokyo", "admin1": "Tokyo", "country_code": "JP",
    "timezone": "Asia/Tokyo", "latitude": 35.6895, "longitude": 139.6917,
}

# IANA → POSIX rules the firmware derives (must match main/iana_tz.cpp).
POSIX_VANCOUVER = "PST8PDT,M3.2.0,M11.1.0"
POSIX_TORONTO = "EST5EDT,M3.2.0,M11.1.0"
POSIX_TOKYO = "JST-9"


def _navigate_to_time_screen(device: DeviceClient):
    """Open settings → time sub-screen (from the main screen)."""
    device.click(tag="settings")
    device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_time")
    device.wait_for_screen("time", timeout=5.0)


def _tz_auto_checked(device: DeviceClient):
    sw = device.find_widget(tag="tz_auto_switch")
    return sw.checked if sw is not None else None


def _location_text(device: DeviceClient) -> str:
    w = device.find_widget(tag="location_value")
    return (w.text or "").strip() if w is not None else ""


def _open_search(device: DeviceClient):
    """Open the location search dialog and wait for its input to appear."""
    device.click(tag="location_change_btn")
    device.wait_for_widget(tag="location_search_input", timeout=5.0)


def _search(device: DeviceClient, query: str):
    """Type a query and wait for the debounced (mocked) search to populate."""
    device.type_text("location_search_input", query)
    # Debounce (~450ms) + mocked worker → results or a status message.
    device.wait_until(
        f"search for '{query}' produced results or a status",
        lambda: device.find_widget(tag="loc_result_0") is not None
        or _search_status(device) in (
            "No matches found", "Search failed. Check network.",
        ),
        timeout=8.0,
    )


def _search_status(device: DeviceClient) -> str:
    w = device.find_widget(tag="location_search_status")
    return (w.text or "").strip() if w is not None else ""


def _select_first_result(device: DeviceClient, expect_name: str):
    """Click the first result and wait for the dialog to close + label update."""
    device.click(tag="loc_result_0")
    device.wait_until(
        f"location updated to '{expect_name}' and dialog closed",
        lambda: device.find_widget(tag="location_search_input") is None
        and expect_name in _location_text(device),
        timeout=5.0,
    )


def _set_location(device: DeviceClient, result: dict):
    """Install a single-result mock, open search, type, and select it."""
    device.geocoding_mock([result])
    _open_search(device)
    _search(device, result["name"][:4])
    _select_first_result(device, result["name"])


@pytest.fixture
def location_restore(device: DeviceClient):
    """Restore the device to the Sun Peaks default (automatic timezone)."""
    yield

    _return_to_main(device)
    _navigate_to_time_screen(device)
    try:
        if SUN_PEAKS["name"] not in _location_text(device):
            _set_location(device, SUN_PEAKS)
        # Ensure automatic timezone is back on.
        if _tz_auto_checked(device) is False:
            device.toggle("tz_auto_switch")
            device.wait_until(
                "automatic timezone restored",
                lambda: _tz_auto_checked(device) is True,
                timeout=5.0, raise_on_timeout=False,
            )
    except Exception as e:
        print(f"\n⚠️ location_restore best-effort cleanup failed: {e}")
    finally:
        try:
            device.geocoding_mock_reset()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_location_card_shows_default(device: DeviceClient):
    """The location card shows a location and, in auto mode, a derived timezone."""
    _navigate_to_time_screen(device)

    loc = device.find_widget(tag="location_value")
    assert loc is not None, "Could not find location_value widget"
    assert _location_text(device), "Location label should not be empty"

    auto = device.find_widget(tag="tz_auto_switch")
    assert auto is not None, "Could not find tz_auto_switch widget"

    if auto.checked:
        derived = device.find_widget(tag="tz_auto_value")
        assert derived is not None and (derived.text or "").strip(), \
            "Automatic mode should show a derived timezone label"
        # The manual roller is hidden while automatic mode is on.
        assert device.find_widget(tag="timezone_roller") is None, \
            "Manual roller should be hidden in automatic mode"


def test_search_lists_results(device: DeviceClient, location_restore):
    """Typing a query lists candidate locations from the (mocked) geocoder."""
    _navigate_to_time_screen(device)
    device.geocoding_mock([KAMLOOPS, TORONTO])
    _open_search(device)
    _search(device, "kam")

    r0 = device.find_widget(tag="loc_result_0")
    r1 = device.find_widget(tag="loc_result_1")
    assert r0 is not None and r1 is not None, "Expected two search results"
    assert _search_status(device) == "Select a location"


def test_select_result_updates_location(device: DeviceClient, location_restore):
    """Selecting a result updates the location label and closes the dialog."""
    _navigate_to_time_screen(device)
    _set_location(device, KAMLOOPS)

    assert "Kamloops" in _location_text(device)
    assert device.find_widget(tag="location_search_input") is None, \
        "Search dialog should close after selecting a result"


def test_auto_timezone_derived_from_location(device: DeviceClient, location_restore):
    """In automatic mode, selecting a location re-derives the timezone."""
    _navigate_to_time_screen(device)

    # Ensure automatic mode is on.
    if _tz_auto_checked(device) is False:
        device.toggle("tz_auto_switch")
        device.wait_until("auto tz on", lambda: _tz_auto_checked(device) is True,
                          timeout=5.0)

    # Toronto → Eastern
    _set_location(device, TORONTO)
    device.wait_until(
        f"preferences timezone is {POSIX_TORONTO}",
        lambda: device.get_preferences().get("timezone") == POSIX_TORONTO,
        timeout=5.0,
    )

    # Tokyo → JST (a very different zone) to prove it re-derives each time.
    _set_location(device, TOKYO)
    device.wait_until(
        f"preferences timezone is {POSIX_TOKYO}",
        lambda: device.get_preferences().get("timezone") == POSIX_TOKYO,
        timeout=5.0,
    )


def test_manual_override_reveals_roller(device: DeviceClient, location_restore):
    """Turning automatic mode off reveals the manual timezone roller."""
    _navigate_to_time_screen(device)

    # Force automatic on first so we exercise the auto → manual transition.
    if _tz_auto_checked(device) is False:
        device.toggle("tz_auto_switch")
        device.wait_until("auto tz on", lambda: _tz_auto_checked(device) is True,
                          timeout=5.0)
    assert device.find_widget(tag="timezone_roller") is None, \
        "Roller should be hidden while automatic"

    device.toggle("tz_auto_switch")
    device.wait_until(
        "manual roller becomes visible",
        lambda: device.find_widget(tag="timezone_roller") is not None
        and _tz_auto_checked(device) is False,
        timeout=5.0,
    )
    roller = device.find_widget(tag="timezone_roller")
    assert roller.type == "roller"
    assert roller.selected_text, "Roller should show a selected timezone"


def test_search_no_matches(device: DeviceClient, location_restore):
    """An empty geocoding result set surfaces a 'no matches' status."""
    _navigate_to_time_screen(device)
    device.geocoding_mock([])  # zero results
    _open_search(device)
    _search(device, "zzznowhere")

    assert device.find_widget(tag="loc_result_0") is None
    assert _search_status(device) == "No matches found"


def test_search_failure_shows_status(device: DeviceClient, location_restore):
    """A geocoding failure surfaces an error status rather than crashing."""
    _navigate_to_time_screen(device)
    device.geocoding_mock_error()
    _open_search(device)
    _search(device, "kamloops")

    assert device.find_widget(tag="loc_result_0") is None
    assert _search_status(device) == "Search failed. Check network."


def test_cancel_closes_dialog(device: DeviceClient, location_restore):
    """The Cancel button dismisses the search dialog without changing location."""
    _navigate_to_time_screen(device)
    before = _location_text(device)

    device.geocoding_mock([KAMLOOPS])
    _open_search(device)
    device.click(tag="location_search_cancel")
    device.wait_until(
        "search dialog closed",
        lambda: device.find_widget(tag="location_search_input") is None,
        timeout=5.0,
    )
    assert _location_text(device) == before, \
        "Cancelling must not change the current location"
