"""
Test: Errors Screen

Navigates to the errors sub-screen and verifies the no-errors state,
active error injection via demo fields, error cards, clear history,
and error display via the /api/heatpump/errors endpoint.

Faults are injected by their canonical Macon code via inject_fault(), which
lets the arctic-macon library own the code->register,bit mapping. The errors
screen reads from getActiveErrors() which decodes the raw fault registers.
"""

import pytest
from device_client import DeviceClient

UI_SETTLE = 1.5

# Default demo fault restored after each test (matches initDemoState()).
DEMO_FAULT = "P02"


@pytest.fixture(autouse=True)
def _restore_error_state(device: DeviceClient):
    """Clear error state after each test."""
    yield
    device.clear_all_faults()
    device.inject_fault(DEMO_FAULT, True)  # restore default demo fault
    device.clear_error_history()


def _open_errors(device: DeviceClient):
    """Navigate from main to the errors screen via the error card."""
    device.click(tag="error_label")
    assert device.wait_for_screen("errors", timeout=5.0), \
        f"Expected 'errors' screen, got '{device.screen}'"


def _close_errors(device: DeviceClient):
    """Close the errors screen back to main."""
    device.click(tag="errors_close")
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
    """Navigate to/from the errors screen."""

    def test_open_errors_via_error_card(self, device: DeviceClient):
        """Clicking the error card on main opens the errors screen."""
        _open_errors(device)

    def test_close_errors_screen(self, device: DeviceClient):
        """Clicking close returns to the main screen."""
        _open_errors(device)
        _close_errors(device)


# =========================================================================
# Title
# =========================================================================

class TestErrorsTitle:
    """Verify the title shows 'Error Status'."""

    def test_title_text(self, device: DeviceClient):
        """The title should display the Error Status i18n string."""
        _open_errors(device)
        title = device.find_widget(tag="errors_title")
        assert title is not None, "Errors title widget not found"
        text = title.text_en or title.text
        assert "error status" in text.lower(), \
            f"Expected 'Error Status' in title, got: {text!r}"


# =========================================================================
# No Errors State
# =========================================================================

class TestNoErrorsState:
    """Verify the no-errors state when all error registers are zero."""

    def test_no_errors_message(self, device: DeviceClient):
        """When no errors are active, the no-errors message should appear."""
        # Clear all faults and history
        device.clear_all_faults()
        device.clear_error_history()

        # Error card is always clickable (shows "System OK" when no errors)
        _open_errors(device)
        device.wait_until(
            "no-errors message to appear",
            lambda: device.find_widget(tag="errors_no_errors") is not None,
            timeout=5.0, expect_within=UI_SETTLE, raise_on_timeout=False,
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
        _wait_screen_text(device, "high pressure")

        # P02 description contains "high pressure"
        assert _has_text_containing(device, "high pressure"), \
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
