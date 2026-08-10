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


def test_off_display_consumes_first_touch(device: DeviceClient):
    """The first touch also wakes a fully off display without navigating."""
    device.wait_for_screen("main", timeout=5.0)

    off = device.set_display_idle("off")
    assert off["off"] is True
    assert off["dimmed"] is False

    wake = device.click(tag="settings")
    assert wake["consumed"] is True
    assert device.get_ui_state()["screen"] == "main"

    status = device.set_display_idle("status")
    assert status["off"] is False
    assert status["dimmed"] is False


def test_turning_off_returns_home_from_settings_modal(device: DeviceClient):
    """Turning the display off abandons screens and modals before wake."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_factory_reset")
    assert device.wait_for_widget(tag="factory_reset_overlay", timeout=3.0)

    off = device.set_display_idle("off")
    assert off["off"] is True
    assert device.screen == "main"

    wake = device.click(tag="settings")
    assert wake["consumed"] is True
    assert device.screen == "main"
