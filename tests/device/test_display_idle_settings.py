import time

from device_client import DeviceClient


def _open_display(device: DeviceClient):
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_display")
    assert device.wait_for_screen("display", timeout=5.0)
    time.sleep(0.5)


def test_configure_staged_display_timeouts(device: DeviceClient):
    """Dim and off timers are independent, additive device settings."""
    _open_display(device)

    dim = device.set_roller("display_dim_timeout", 2)
    off = device.set_roller("display_off_timeout", 3)
    assert dim["selected_text"] == "2 minutes"
    assert off["selected_text"] == "3 minutes"

    status = device.set_display_idle("status")
    assert status["dim_minutes"] == 2
    assert status["off_minutes"] == 3

    device.set_roller("display_dim_timeout", 1)
    device.set_roller("display_off_timeout", 4)


def test_never_is_available_for_each_timeout(device: DeviceClient):
    """Both staged timers allow Never without adding Web UI settings."""
    _open_display(device)

    dim = device.set_roller("display_dim_timeout", 0)
    off = device.set_roller("display_off_timeout", 0)
    assert dim["selected_text"] == "Never"
    assert off["selected_text"] == "Never"

    status = device.set_display_idle("status")
    assert status["dim_minutes"] == 0
    assert status["off_minutes"] == 0

    device.set_roller("display_dim_timeout", 1)
    device.set_roller("display_off_timeout", 4)
