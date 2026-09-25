"""Device tests for the eight-hour temperature history graph."""

from device_client import DeviceClient


def _open_status(device: DeviceClient) -> None:
    device.click(tag="nav_status")
    device.wait_for_widget(tag="temperature_history_open")


def test_temperature_history_page_and_navigation(device: DeviceClient):
    fixture = device.populate_temperature_history()
    assert fixture["samples"] == 960

    _open_status(device)
    device.click(tag="temperature_history_open")
    device.wait_for_widget(tag="temperature_history_screen")
    device.wait_for_widget(tag="temperature_history_previous")
    device.wait_for_widget(tag="temperature_history_next")
    device.wait_for_widget(tag="temperature_history_latest")

    # At open the window sits at the latest 8h, so "next" is disabled. Paging
    # back enables it, and paging forward returns to latest (disabled again).
    # Waiting on that button state replaces fixed settles and also asserts the
    # pagination actually took effect.
    device.click(tag="temperature_history_previous")
    device.wait_until(
        "history next button enabled after paging back",
        lambda: (w := device.find_widget(tag="temperature_history_next"))
        is not None and not w.disabled,
        timeout=5.0,
    )
    device.click(tag="temperature_history_next")
    device.wait_until(
        "history next button disabled back at latest window",
        lambda: (w := device.find_widget(tag="temperature_history_next"))
        is not None and bool(w.disabled),
        timeout=5.0,
    )
    device.click(tag="temperature_history_back")
    device.wait_for_widget(tag="temperature_history_open")


def test_status_tab_survives_display_off_while_history_open(device: DeviceClient):
    """Display-off tears down the history overlay from outside the Status tab.

    Regression: the Status content stayed hidden afterwards, so the tab
    rendered blank until reboot.
    """
    _open_status(device)
    device.click(tag="temperature_history_open")
    device.wait_for_widget(tag="temperature_history_screen")

    off = device.set_display_idle("off")
    assert off["off"] is True
    device.set_display_idle("wake")
    assert device.screen == "main"
    assert not device.has_widget(tag="temperature_history_screen")

    _open_status(device)
