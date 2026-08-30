from device_client import DeviceClient


def _open_display(device: DeviceClient):
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_display")
    assert device.wait_for_screen("display", timeout=5.0)
    assert device.wait_for_widget(tag="display_dim_timeout", timeout=5.0)


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


def test_off_timeout_runs_when_dimming_is_never(device: DeviceClient):
    """Never dimming still allows the independent off timeout to run."""
    _open_display(device)
    device.set_roller("display_dim_timeout", 0)
    device.set_roller("display_off_timeout", 1)
    device.click(tag="display_back")
    device.click(tag="settings_close")
    assert device.wait_for_screen("main", timeout=5.0)

    # The off timeout is set to 1 minute with dimming Never, so the screen
    # turns off after ~60s of inactivity. Poll the idle status (the "status"
    # action does not register activity, so it will not reset the timer)
    # until the off timeout fires, instead of a fixed sleep that could flake
    # on timing jitter.
    device.wait_until(
        "display off timeout fires after ~1 minute idle",
        lambda: device.set_display_idle("status").get("off") is True,
        timeout=90.0,
        poll=3.0,
    )
    status = device.set_display_idle("status")
    assert status["dimmed"] is False
    assert status["off"] is True

    wake = device.click(tag="settings")
    assert wake["consumed"] is True
    assert device.screen == "main"

    _open_display(device)
    device.set_roller("display_dim_timeout", 1)
    device.set_roller("display_off_timeout", 4)
