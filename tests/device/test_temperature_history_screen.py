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
    assert device.wait_for_screen("history", timeout=5.0)
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
    # One-hour steps: back enables "1 h ›", forward returns to latest.
    device.click(tag="temperature_history_back_hour")
    device.wait_until(
        "1 h forward enabled after stepping back an hour",
        lambda: (w := device.find_widget(tag="temperature_history_forward_hour"))
        is not None and not w.disabled,
        timeout=5.0,
    )
    device.click(tag="temperature_history_forward_hour")
    device.wait_until(
        "1 h forward disabled back at latest window",
        lambda: (w := device.find_widget(tag="temperature_history_forward_hour"))
        is not None and bool(w.disabled),
        timeout=5.0,
    )
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)


def test_history_tab_switches_segments_and_remembers_last(device: DeviceClient):
    device.populate_temperature_history()
    device.click(tag="nav_history")
    device.wait_for_widget(tag="history_seg_cycles")
    device.click(tag="history_seg_cycles")
    device.wait_for_widget(tag="temperature_history_screen")

    # The chart is freed while Events is showing.
    assert device.open_event_log()
    assert not device.has_widget(tag="temperature_history_screen")

    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)
    device.click(tag="nav_history")
    assert device.wait_for_screen("event_log", timeout=5.0)

    device.click(tag="history_seg_cycles")
    device.wait_for_widget(tag="temperature_history_screen")
    assert device.wait_for_screen("history", timeout=5.0)
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)


def test_history_tab_rebuilds_chart_after_display_off(device: DeviceClient):
    """Display-off returns home from the History tab; reopening History must
    rebuild the chart rather than show an empty Cycles segment."""
    _open_status(device)
    device.click(tag="temperature_history_open")
    device.wait_for_widget(tag="temperature_history_screen")

    off = device.set_display_idle("off")
    assert off["off"] is True
    device.set_display_idle("wake")
    assert device.screen == "main"
    assert not device.has_widget(tag="temperature_history_screen")

    device.click(tag="nav_history")
    device.wait_for_widget(tag="temperature_history_screen")
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)
