"""
Test: Firmware Update Screen

Verifies the firmware screen displays version info correctly and
responds to mock firmware check results (update available / up-to-date).

NOTE: These tests never click the "Install Update" button — doing so
would trigger a real OTA download and device reboot.  The Install button
is tested only for *visibility* when an update is available.
"""

import re
import pytest
from device_client import DeviceClient


# Semver-ish pattern: digits.digits.digits (with optional leading text)
_VERSION_RE = re.compile(r"\d+\.\d+\.\d+")


# ── helpers ──────────────────────────────────────────────────────────────

def _open_firmware_screen(device: DeviceClient):
    """Navigate from main → settings → firmware."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.wait_for_widget(tag="settings_firmware", timeout=5.0)
    device.click(tag="settings_firmware")
    assert device.wait_for_screen("firmware", timeout=5.0)
    device.wait_for_widget(tag="firmware_current_version", timeout=5.0)


def _wait_for_check_complete(device: DeviceClient, timeout: float = 15.0):
    """Poll until the firmware screen leaves the 'checking' state.

    The status label will contain a checkmark (OK) or warning symbol
    once the real GitHub check completes (or fails due to no internet).
    We just need it to stop saying "Checking...".

    While the ESP32 is performing the outbound HTTPS request to GitHub the TLS
    handshake can block the HTTP server for several seconds; wait_until treats
    the resulting transient ReadTimeout as "not yet" and keeps polling.
    """
    checking_words = ("checking", "comprobando", "vérification")

    def _done() -> bool:
        status = device.find_widget(tag="firmware_status")
        if status and status.text and status.text.strip():
            return not any(w in status.text.lower() for w in checking_words)
        return False

    return device.wait_until(
        "firmware check leaves checking state",
        _done,
        timeout=timeout,
        poll=0.5,
    )


# ── tests ────────────────────────────────────────────────────────────────

def test_current_version_displayed(device: DeviceClient):
    """The firmware screen should show the current firmware version."""
    _open_firmware_screen(device)

    ver_widget = device.find_widget(tag="firmware_current_version")
    assert ver_widget is not None, "Current version label not found"
    assert ver_widget.text is not None
    assert _VERSION_RE.search(ver_widget.text), \
        f"Expected version string in '{ver_widget.text}'"


def test_github_check_completes(device: DeviceClient):
    """Opening the firmware screen should check GitHub and show a result.

    We don't assert *which* result (update/no-update/failed) — just that
    the check finishes within a reasonable time.
    """
    _open_firmware_screen(device)

    assert _wait_for_check_complete(device, timeout=60.0), \
        "Firmware check did not complete within 60 seconds"

    # After the check, the latest-version label should have content
    latest = device.find_widget(tag="firmware_latest_version")
    assert latest is not None, "Latest version label not found"
    assert latest.text is not None and len(latest.text.strip()) > 0


def test_mock_update_available(device: DeviceClient):
    """Injecting a mock 'update available' should show the Install button."""
    _open_firmware_screen(device)

    # Wait for the real check to settle first, so it doesn't overwrite our mock
    assert _wait_for_check_complete(device, timeout=60.0), \
        "Firmware check must complete before injecting mock"

    device.firmware_mock(version="99.0.0", update_available=True)

    # Poll for the button to appear (UI update may take a moment)
    assert device.wait_for_widget(tag="firmware_update_btn", timeout=3.0), \
        "Install Update button should be visible when update is available"

    # Latest version label should mention 99.0.0
    latest = device.find_widget(tag="firmware_latest_version")
    assert latest is not None
    assert "99.0.0" in latest.text, f"Expected '99.0.0' in '{latest.text}'"

    btn = device.find_widget(tag="firmware_update_btn")
    assert btn is not None

    # Status should indicate an update is available
    status = device.find_widget(tag="firmware_status")
    assert status is not None
    assert status.text is not None and len(status.text.strip()) > 0

    # Clean up
    device.firmware_mock_reset()


def test_mock_no_update(device: DeviceClient):
    """Injecting 'no update' with the same version should hide the Install button."""
    _open_firmware_screen(device)
    assert _wait_for_check_complete(device, timeout=60.0), \
        "Firmware check must complete before injecting mock"

    # Get current version from the label
    ver_widget = device.find_widget(tag="firmware_current_version")
    assert ver_widget is not None
    m = _VERSION_RE.search(ver_widget.text)
    assert m, f"Could not extract version from '{ver_widget.text}'"
    current_ver = m.group(0)

    # Mock with the same version → no update
    device.firmware_mock(version=current_ver, update_available=False)

    # Install button should NOT be visible
    device.wait_until(
        "install button hidden after no-update mock",
        lambda: device.find_widget(tag="firmware_update_btn") is None,
        timeout=5.0,
    )
    btn = device.find_widget(tag="firmware_update_btn")
    assert btn is None, "Install button should be hidden when firmware is up-to-date"

    # Latest version should show the current version
    latest = device.find_widget(tag="firmware_latest_version")
    assert latest is not None
    assert current_ver in latest.text

    device.firmware_mock_reset()


def test_mock_update_button_not_clicked(device: DeviceClient):
    """The Install button is visible but we intentionally do NOT click it.

    This test exists to document the boundary: we verify the button appears
    but never trigger a real OTA download.  See todo.md for future plans.
    """
    _open_firmware_screen(device)
    assert _wait_for_check_complete(device, timeout=60.0), \
        "Firmware check must complete before injecting mock"

    device.firmware_mock(version="99.0.0", update_available=True)

    assert device.wait_for_widget(tag="firmware_update_btn", timeout=3.0), \
        "Install Update button should be visible"
    btn = device.find_widget(tag="firmware_update_btn")
    assert btn.type == "button", f"Expected button type, got '{btn.type}'"

    # Confirm we can see the button dimensions (it's actually rendered)
    assert btn.w > 0 and btn.h > 0, "Button should have non-zero size"

    device.firmware_mock_reset()


# ── notification badge (issue #145) ──────────────────────────────────────

def _firmware_back_to_main(device: DeviceClient):
    """Return main from the firmware screen without resetting the mock."""
    device.click(tag="firmware_back")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.wait_for_widget(tag="settings_close", timeout=5.0)
    device.click(tag="settings_close")
    assert device.wait_for_screen("main", timeout=5.0)


def _firmware_badge_present(device: DeviceClient) -> bool:
    """Open the notification dropdown, report whether the firmware item is
    present, then close it. An empty dropdown never opens (toggle is a no-op),
    so clicking twice is safe whether or not a notification exists."""
    device.click(tag="notifications")
    # If any notification exists the dropdown opens (notify_title appears); if
    # none exist the toggle is a no-op and it never opens. Bounded best-effort
    # wait covers both cases without a fixed sleep.
    device.wait_until(
        "notification dropdown settled",
        lambda: device.has_widget(tag="notify_title"),
        timeout=2.0,
        raise_on_timeout=False,
    )
    present = device.has_widget(tag="notify_item_firmware")
    device.click(tag="notifications")
    device.wait_until(
        "notification dropdown closed",
        lambda: not device.has_widget(tag="notify_title"),
        timeout=2.0,
        raise_on_timeout=False,
    )
    return present


def test_manual_check_sets_and_clears_notification_badge(device: DeviceClient):
    """A manual (mock) check that finds an update must set the status-bar
    notification badge so it isn't forgotten after leaving the screen, and a
    later 'up to date' result must clear it (issue #145)."""
    device.notification_mock_reset()

    # Update available → badge appears.
    _open_firmware_screen(device)
    assert _wait_for_check_complete(device, timeout=60.0), \
        "Firmware check must complete before injecting mock"
    device.firmware_mock(version="99.0.0", update_available=True)
    # Confirm the mock took effect (Install button shown) before navigating away.
    assert device.wait_for_widget(tag="firmware_update_btn", timeout=3.0), \
        "Install button should appear after mocking an available update"
    _firmware_back_to_main(device)

    assert _firmware_badge_present(device), \
        "Firmware badge should be set after a manual check finds an update"

    # Up to date → badge clears.
    _open_firmware_screen(device)
    cur = device.find_widget(tag="firmware_current_version")
    assert cur is not None
    m = _VERSION_RE.search(cur.text)
    assert m, f"Could not extract version from '{cur.text}'"
    device.firmware_mock(version=m.group(0), update_available=False)
    device.wait_until(
        "install button hidden after no-update mock",
        lambda: device.find_widget(tag="firmware_update_btn") is None,
        timeout=3.0,
    )
    device.firmware_mock_reset()
    _firmware_back_to_main(device)

    assert not _firmware_badge_present(device), \
        "Firmware badge should be cleared when a check finds no update"

    device.notification_mock_reset()

