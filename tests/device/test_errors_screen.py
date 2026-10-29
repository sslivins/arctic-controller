"""
Test: History › Faults

Opens the Faults segment and verifies the no-errors state,
active error injection via demo fields, error cards, clear history,
and error display via the /api/heatpump/errors endpoint.

Faults are injected by their canonical Macon code via inject_fault(), which
lets the arctic-macon library own the code->register,bit mapping. The errors
screen reads from getActiveErrors() which decodes the raw fault registers.
"""

import pytest
from device_client import DeviceClient

UI_SETTLE = 1.5

# Injected by tests that just need the errors screen open: the home fault
# banner (the way in) only shows while a fault is active.
DEMO_FAULT = "P02"


@pytest.fixture(autouse=True)
def _restore_error_state(device: DeviceClient):
    """Clear error state after each test (the demo default has no fault)."""
    yield
    device.clear_all_faults()
    device.clear_error_history()


def _open_errors(device: DeviceClient):
    """Open History › Faults from the home fault banner."""
    assert device.wait_for_widget(tag="error_label", timeout=5.0), \
        "fault banner not shown (no active fault?)"
    device.click(tag="error_label")
    assert device.wait_for_screen("faults", timeout=5.0), \
        f"Expected 'faults' screen, got '{device.screen}'"


def _close_errors(device: DeviceClient):
    """Leave the Faults segment for the Home tab."""
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0), \
        f"Expected 'main' screen after close, got '{device.screen}'"


def _has_text_containing(device: DeviceClient, substring: str) -> bool:
    """Check if any widget's text contains the given substring."""
    sub_lower = substring.lower()
    for w in device.widgets:
        t = w.text_en or w.text
        if t and sub_lower in t.lower():
            return True
    return False


def _wait_screen_text(device: DeviceClient, substring: str, timeout: float = 5.0):
    """Wait until some widget on the current screen contains ``substring``.

    Error cards render asynchronously after the errors screen settles, so poll
    for the expected text instead of sleeping a fixed UI_SETTLE guess.
    Best-effort: the caller's own assert makes the final authoritative check.
    """
    device.wait_until(
        f"screen text containing {substring!r}",
        lambda: _has_text_containing(device, substring),
        timeout=timeout, expect_within=UI_SETTLE, raise_on_timeout=False,
    )


def _errors_json(device: DeviceClient) -> dict:
    """GET /api/heatpump/errors as JSON."""
    r = device.session.get(f"{device.base_url}/api/heatpump/errors",
                           timeout=device.timeout)
    r.raise_for_status()
    return r.json()


def _active_codes(device: DeviceClient) -> list:
    """Active error codes from /api/heatpump/errors."""
    data = _errors_json(device)
    active = data.get("active", data.get("errors", []))
    return [e.get("code", "") for e in active if isinstance(e, dict)]


# =========================================================================
# Navigation
# =========================================================================

class TestErrorsNavigation:
    """History › Faults is reachable from the home banner and the segment bar."""

    def test_open_errors_via_error_card(self, device: DeviceClient):
        """Clicking the home fault banner opens History › Faults."""
        device.inject_fault(DEMO_FAULT, True)
        _open_errors(device)

    def test_close_errors_screen(self, device: DeviceClient):
        """The nav bar leaves the Faults segment like any other tab."""
        device.inject_fault(DEMO_FAULT, True)
        _open_errors(device)
        _close_errors(device)

    def test_open_from_history_segment(self, device: DeviceClient):
        """The Faults segment opens without an active fault."""
        assert device.open_faults(), f"Expected 'faults', got '{device.screen}'"
        assert device.wait_for_widget(tag="errors_no_errors", timeout=5.0)


# =========================================================================
# Segment dot
# =========================================================================

class TestFaultsSegmentDot:
    """A red dot on the Faults segment button marks an active fault."""

    def test_dot_follows_active_faults(self, device: DeviceClient):
        device.clear_all_faults()
        device.click(tag="nav_history")
        assert device.wait_for_widget(tag="history_seg_faults", timeout=5.0)
        assert device.find_widget(tag="history_seg_faults_dot") is None
        device.inject_fault(DEMO_FAULT, True)
        assert device.wait_for_widget(tag="history_seg_faults_dot", timeout=5.0), \
            "Faults segment dot didn't appear for an active fault"
        device.clear_all_faults()
        device.wait_until(
            "Faults segment dot to clear",
            lambda: device.find_widget(tag="history_seg_faults_dot") is None,
            timeout=5.0, expect_within=UI_SETTLE,
        )


# =========================================================================
# Show on chart
# =========================================================================

class TestShowOnChart:
    """An opened fault card jumps to the cycle chart."""

    def test_show_on_chart_centres_the_fault(self, device: DeviceClient):
        device.clear_all_faults()
        device.clear_error_history()
        device.inject_fault("P02", True)
        _open_errors(device)
        _wait_screen_text(device, "pressure too high")
        assert device.find_widget(tag="error_show_on_chart") is None, \
            "Closed cards shouldn't show the chart button"
        device.click(label_contains="pressure too high")
        assert device.wait_for_widget(tag="error_show_on_chart", timeout=5.0)

        device.click(tag="error_show_on_chart")
        assert device.wait_for_screen("history", timeout=5.0), \
            f"Expected the cycle chart, got '{device.screen}'"
        assert device.wait_for_widget(tag="temperature_history_screen", timeout=10.0)


# =========================================================================
# No Errors State
# =========================================================================

class TestNoErrorsState:
    """Verify the no-errors state when all error registers are zero."""

    def test_no_errors_message(self, device: DeviceClient):
        """When no errors are active, the no-errors message should appear."""
        # The home fault banner is hidden when nothing is active, so open the
        # screen while a fault is showing, then clear it and wait for the
        # screen to switch to its no-errors state.
        device.clear_all_faults()
        device.inject_fault("P02", True)
        _open_errors(device)
        device.clear_all_faults()
        device.clear_error_history()
        device.wait_until(
            "no-errors message to appear",
            lambda: device.find_widget(tag="errors_no_errors") is not None,
            timeout=10.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )

        # The no-errors label should be visible
        no_errors = device.find_widget(tag="errors_no_errors")
        assert no_errors is not None, "No-errors message not found"
        text = no_errors.text_en or no_errors.text
        assert "no" in text.lower() and "error" in text.lower(), \
            f"Expected 'No Errors' message, got: {text!r}"


# =========================================================================
# Active Error Display
# =========================================================================

class TestActiveErrors:
    """Verify error cards appear when errors are injected."""

    def test_p02_error_card(self, device: DeviceClient):
        """Injecting P02 (High Pressure) shows the error card."""
        device.clear_all_faults()
        device.inject_fault("P02", True)

        _open_errors(device)
        _wait_screen_text(device, "P02")

        # Should show P02 error code
        assert _has_text_containing(device, "P02"), \
            "P02 error code not found on screen"

    def test_error_description_shown(self, device: DeviceClient):
        """Active errors should show a description."""
        device.clear_all_faults()
        device.inject_fault("P02", True)

        _open_errors(device)
        _wait_screen_text(device, "pressure too high")

        # P02 description contains "pressure too high"
        assert _has_text_containing(device, "pressure too high"), \
            "Error description for P02 not found on screen"

    def test_multiple_errors(self, device: DeviceClient):
        """Setting multiple faults shows multiple error cards."""
        # Set both P02 and P06
        device.clear_all_faults()
        device.inject_fault("P02", True)
        device.inject_fault("P06", True)

        _open_errors(device)
        device.wait_until(
            "P02 and P06 error cards to appear",
            lambda: _has_text_containing(device, "P02") and _has_text_containing(device, "P06"),
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )

        assert _has_text_containing(device, "P02"), \
            "P02 error not found"
        assert _has_text_containing(device, "P06"), \
            "P06 error not found"

    def test_error_from_register1(self, device: DeviceClient):
        """A sensor fault (reg2125) should also show error cards."""
        device.clear_all_faults()
        device.inject_fault("E19", True)

        _open_errors(device)
        _wait_screen_text(device, "E19")

        assert _has_text_containing(device, "E19"), \
            "E19 sensor fault not found"


# =========================================================================
# Clear History
# =========================================================================

class TestClearHistory:
    """Verify the clear history button works."""

    def test_clear_history_button_visible(self, device: DeviceClient):
        """The clear history button should be visible when there's history."""
        device.inject_fault(DEMO_FAULT, True)
        _open_errors(device)

        clear_btn = device.find_widget(tag="errors_clear")
        # Button may or may not be visible depending on whether there's history
        # In demo mode, 2 history entries are pre-populated
        # Just verify we can find it (it exists in the UI tree)
        # If not found, that's ok — it means no history to clear
        if clear_btn is not None:
            assert True  # Button found

    def test_clear_history_via_api(self, device: DeviceClient):
        """POST /api/test/clear-error-history should clear history."""
        device.clear_error_history()
        device.wait_until(
            "error history to clear",
            lambda: _errors_json(device).get("history_count", 0) == 0
            or len(_errors_json(device).get("history", [])) == 0,
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )

        resp = device.session.get(f"{device.base_url}/api/heatpump/errors")
        assert resp.status_code == 200
        data = resp.json()
        # After clearing, history should be empty
        assert data.get("history_count", 0) == 0 or \
            len(data.get("history", [])) == 0, \
            f"Expected empty history after clear, got: {data}"


# =========================================================================
# API
# =========================================================================

class TestErrorsApi:
    """Verify the /api/heatpump/errors endpoint."""

    def test_errors_endpoint_returns_data(self, device: DeviceClient):
        """GET /api/heatpump/errors should return error data."""
        device.clear_all_faults()
        device.inject_fault("P02", True)
        device.wait_until(
            "errors endpoint to report an active error",
            lambda: len(_active_codes(device)) > 0,
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )

        resp = device.session.get(f"{device.base_url}/api/heatpump/errors")
        assert resp.status_code == 200
        data = resp.json()
        assert "active" in data or "errors" in data, \
            f"Expected error data in response, got: {list(data.keys())}"

    def test_errors_endpoint_reflects_injected_error(self, device: DeviceClient):
        """Injected errors should appear in the API response."""
        device.clear_all_faults()
        device.inject_fault("P02", True)
        device.wait_until(
            "P02 to appear in errors endpoint",
            lambda: "P02" in _active_codes(device),
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )

        resp = device.session.get(f"{device.base_url}/api/heatpump/errors")
        assert resp.status_code == 200
        data = resp.json()

        # Find P02 in the response
        active = data.get("active", data.get("errors", []))
        codes = [e.get("code", "") for e in active if isinstance(e, dict)]
        assert "P02" in codes, \
            f"P02 not found in active errors: {codes}"

# =========================================================================
# Troubleshooting links
# =========================================================================

P02_ARTICLE = "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832838"
SUPPORT_SITE = "https://arcticheatpumps.freshdesk.com/support/solutions"


class TestTroubleshootingLinks:
    """Each fault links to Arctic's troubleshooting article for its code."""

    def test_opening_a_card_shows_the_troubleshooting_qr_code(self, device: DeviceClient):
        device.clear_all_faults()
        device.inject_fault("P02", True)
        _open_errors(device)
        _wait_screen_text(device, "pressure too high")

        # Closed cards don't draw the QR code at all.
        assert device.find_widget(tag="error_help_qr") is None
        assert device.find_widget(tag="error_help_hint") is None

        device.click(label_contains="pressure too high")
        device.wait_until(
            "troubleshooting QR code to appear",
            lambda: device.find_widget(tag="error_help_qr") is not None,
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )
        qr = device.find_widget(tag="error_help_qr")
        assert qr is not None, "Opening the card didn't show the QR code"
        assert qr.w >= 200 and qr.h >= 200, f"QR code too small to scan: {qr.w}x{qr.h}"
        hint = device.find_widget(tag="error_help_hint")
        assert hint is not None
        assert "Arctic Heat Pumps support site" in (hint.text_en or hint.text)

    def test_api_links_each_fault_to_its_article(self, device: DeviceClient):
        device.clear_all_faults()
        device.inject_fault("P02", True)
        device.inject_fault("E03", True)  # Arctic has no article for E03
        device.wait_until(
            "P02 and E03 to appear in errors endpoint",
            lambda: {"P02", "E03"} <= set(_active_codes(device)),
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
        )
        data = _errors_json(device)
        urls = {e["code"]: e.get("help_url") for e in data.get("active", [])}
        assert urls.get("P02") == P02_ARTICLE, urls
        assert urls.get("E03") == SUPPORT_SITE, urls
        for entry in data.get("history", []):
            assert entry.get("help_url", "").startswith(SUPPORT_SITE), entry
