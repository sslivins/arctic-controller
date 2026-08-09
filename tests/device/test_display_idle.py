from device_client import DeviceClient


def test_dimmed_display_consumes_first_touch(device: DeviceClient):
    """The first touch wakes the display without activating its target."""
    device.wait_for_screen("main", timeout=5.0)

    dimmed = device.set_display_idle("dim")
    assert dimmed["dimmed"] is True
    assert dimmed["saved_brightness"] > 10

    wake = device.click(tag="settings")
    assert wake["consumed"] is True
    assert device.get_ui_state()["screen"] == "main"

    status = device.set_display_idle("status")
    assert status["dimmed"] is False

    opened = device.click(tag="settings")
    assert opened["consumed"] is False
    device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_close")
    device.wait_for_screen("main", timeout=5.0)
