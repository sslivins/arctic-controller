"""
Pytest fixtures for Arctic Controller device UI tests.

Set ARCTIC_URL env var to override the default device address.
Example: ARCTIC_URL=http://192.168.1.42 pytest
"""

import os
import pytest
from device_client import DeviceClient


@pytest.fixture(scope="session")
def device() -> DeviceClient:
    """Shared device client for the entire test session."""
    url = os.environ.get("ARCTIC_URL", "http://arctic.local")
    client = DeviceClient(base_url=url)
    # Sanity check — can we reach the device?
    try:
        client.get_ui_state()
    except Exception as e:
        pytest.skip(f"Device not reachable at {url}: {e}")
    return client


@pytest.fixture(autouse=True)
def ensure_main_screen(device: DeviceClient):
    """Before each test, make sure we're on the main screen.
    
    If the settings menu is open, click the back/close button to return.
    This keeps tests independent of each other.
    """
    import time

    try:
        current = device.screen
    except Exception:
        # Device may be momentarily busy — give it a moment
        time.sleep(2)
        try:
            current = device.screen
        except Exception:
            return  # Can't reach device, let the test itself handle it

    if current == "main":
        # Even if screen name is "main", widget tree may still be transitioning
        device.wait_for_widget(tag="settings", timeout=3.0)
        return

    # If on a sub-screen (display, wifi, etc.), go back to settings first
    if current in ("display", "wifi", "firmware", "time", "language"):
        # Reset WiFi mock mode if we were on the WiFi screen
        if current == "wifi":
            try:
                device.wifi_mock_reset()
            except Exception:
                pass
        try:
            device.click(tag=f"{current}_back")
        except Exception:
            try:
                device.click(symbol="LEFT")
            except Exception:
                pass
        device.wait_for_screen("settings", timeout=5.0)
        time.sleep(0.5)

    # Now close settings to return to main
    try:
        device.click(tag="settings_close")
    except Exception:
        try:
            device.click(symbol="CLOSE")
        except Exception:
            pass
    device.wait_for_screen("main", timeout=5.0)
    # Wait until the main screen widget tree is fully rendered
    # (screen name transitions before overlay animation completes)
    device.wait_for_widget(tag="settings", timeout=5.0)
