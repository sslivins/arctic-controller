"""
Test: WiFi Password Dialog

Uses mock WiFi networks to verify:
- Tapping a locked network shows the password dialog
- Password text is masked by default (password mode)
- Show/hide toggle reveals and re-hides the password
- Cancel button dismisses the dialog
- Tapping an open network does NOT show the password dialog
"""

import pytest
from device_client import DeviceClient


LOCKED_NETWORK = {"ssid": "TestLocked", "rssi": -45, "authmode": 3}
OPEN_NETWORK = {"ssid": "TestOpen", "rssi": -60, "authmode": 0}
TEST_PASSWORD = "s3cret!42"


def _pw_input(device: DeviceClient):
    """Current WiFi password textarea widget, or None when the dialog is closed."""
    return device.find_widget(tag="wifi_password_input")


@pytest.fixture()
def wifi_screen(device: DeviceClient):
    """Navigate to WiFi screen and inject mock networks.

    Yields the device client, then resets mock mode on teardown.
    """
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)

    device.click(tag="settings_wifi")
    assert device.wait_for_screen("wifi", timeout=5.0)

    # Inject fake networks, then wait for them to render in the list
    result = device.wifi_mock([LOCKED_NETWORK, OPEN_NETWORK])
    assert result["success"] is True
    device.wait_for_widget(text=LOCKED_NETWORK["ssid"], timeout=5.0)

    yield device

    # Teardown: reset mock mode
    try:
        device.wifi_mock_reset()
    except Exception:
        pass


# ── Locked network tests ────────────────────────────────────────────────────


def test_locked_network_shows_password_dialog(wifi_screen: DeviceClient):
    """Tapping a locked network should show the password dialog."""
    device = wifi_screen

    # Click the locked network by its SSID label
    device.click(label=LOCKED_NETWORK["ssid"])
    device.wait_for_widget(tag="wifi_password_ssid", timeout=5.0)

    # Password dialog should be visible — check for its tagged widgets
    ssid_label = device.find_widget(tag="wifi_password_ssid")
    assert ssid_label is not None, "Password dialog SSID label not found"
    assert ssid_label.text == LOCKED_NETWORK["ssid"], \
        f"Expected SSID '{LOCKED_NETWORK['ssid']}', got '{ssid_label.text}'"

    password_input = device.find_widget(tag="wifi_password_input")
    assert password_input is not None, "Password textarea not found"

    # Cancel to close
    device.click(tag="wifi_cancel_btn")
    device.wait_until(
        "password dialog dismissed",
        lambda: device.find_widget(tag="wifi_password_ssid") is None,
        timeout=5.0,
    )


def test_password_masked_by_default(wifi_screen: DeviceClient):
    """Password field should be in password mode (masked) by default."""
    device = wifi_screen

    device.click(label=LOCKED_NETWORK["ssid"])
    device.wait_for_widget(tag="wifi_password_input", timeout=5.0)

    # Type a password
    result = device.type_text("wifi_password_input", TEST_PASSWORD)
    assert result["success"] is True
    assert result["password_mode"] is True, "Password should be masked by default"
    assert result["text"] == TEST_PASSWORD

    # The widget should report password_mode=True
    device.wait_until(
        "typed password reflected in widget",
        lambda: getattr(_pw_input(device), "text", None) == TEST_PASSWORD,
        timeout=5.0,
    )
    ta = _pw_input(device)
    assert ta is not None
    assert ta.password_mode is True, "Widget should report password_mode=True"

    device.click(tag="wifi_cancel_btn")
    device.wait_until(
        "password dialog dismissed",
        lambda: device.find_widget(tag="wifi_password_ssid") is None,
        timeout=5.0,
    )


def test_show_password_toggle(wifi_screen: DeviceClient):
    """Toggling the eye button should reveal and re-hide the password."""
    device = wifi_screen

    device.click(label=LOCKED_NETWORK["ssid"])
    device.wait_for_widget(tag="wifi_password_input", timeout=5.0)

    # Type a password (starts masked)
    device.type_text("wifi_password_input", TEST_PASSWORD)
    device.wait_until(
        "typed password reflected in widget",
        lambda: getattr(_pw_input(device), "text", None) == TEST_PASSWORD,
        timeout=5.0,
    )

    # Click eye button to reveal
    device.click(tag="wifi_show_password")
    device.wait_until(
        "password revealed (plaintext)",
        lambda: getattr(_pw_input(device), "password_mode", None) is False,
        timeout=5.0,
    )

    # Now the textarea should show plaintext (password_mode off)
    ta = _pw_input(device)
    assert ta is not None
    assert ta.password_mode is False, \
        "After clicking show, password_mode should be False"
    assert ta.text == TEST_PASSWORD, \
        f"After reveal, expected plaintext '{TEST_PASSWORD}', got '{ta.text}'"

    # Click eye button again to re-hide
    device.click(tag="wifi_show_password")
    device.wait_until(
        "password re-hidden (masked)",
        lambda: getattr(_pw_input(device), "password_mode", None) is True,
        timeout=5.0,
    )

    ta = _pw_input(device)
    assert ta is not None
    assert ta.password_mode is True, \
        "After re-hide, password_mode should be True again"

    device.click(tag="wifi_cancel_btn")
    device.wait_until(
        "password dialog dismissed",
        lambda: device.find_widget(tag="wifi_password_ssid") is None,
        timeout=5.0,
    )


# ── Open network test ────────────────────────────────────────────────────────


def test_open_network_no_password_dialog(wifi_screen: DeviceClient):
    """Tapping an open network should NOT show the password dialog."""
    device = wifi_screen

    # Click the open network
    device.click(label=OPEN_NETWORK["ssid"])

    # Negative assertion: the password dialog must NOT appear. try_wait_widget
    # polls up to the timeout and returns False if it never shows (failing fast
    # if it does), which is more robust than a fixed sleep + single check.
    assert not device.try_wait_widget(tag="wifi_password_ssid", timeout=2.0), \
        "Password dialog should not appear for open networks"
