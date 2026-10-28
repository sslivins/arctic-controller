"""Device tests for the eight-hour temperature history graph."""

from device_client import DeviceClient


def _open_cycles(device: DeviceClient) -> None:
    """Open the History tab on its Cycles segment (the tab remembers the last
    segment, so select it explicitly)."""
    device.click(tag="nav_history")
    device.wait_for_widget(tag="history_seg_cycles")
    device.click(tag="history_seg_cycles")


def test_temperature_history_page_and_navigation(device: DeviceClient):
    fixture = device.populate_temperature_history()
    assert fixture["samples"] == 960

    _open_cycles(device)
    device.wait_for_widget(tag="temperature_history_screen")
    assert device.wait_for_screen("history", timeout=5.0)
    device.wait_for_widget(tag="temperature_history_previous")
    device.wait_for_widget(tag="temperature_history_next")

    # At open the window sits at the latest 8h, so "next" is disabled and the
    # Latest jump is hidden. Paging back enables "next" and shows Latest.
    assert not device.has_widget(tag="temperature_history_latest")
    # Waiting on that button state replaces fixed settles and also asserts the
    # pagination actually took effect.
    device.click(tag="temperature_history_previous")
    device.wait_until(
        "history next button enabled after paging back",
        lambda: (w := device.find_widget(tag="temperature_history_next"))
        is not None and not w.disabled,
        timeout=5.0,
    )
    device.wait_for_widget(tag="temperature_history_latest")
    device.click(tag="temperature_history_latest")
    device.wait_until(
        "Latest jumps back and hides itself",
        lambda: not device.has_widget(tag="temperature_history_latest"),
        timeout=5.0,
    )
    device.click(tag="temperature_history_previous")
    device.wait_until(
        "history next button enabled after paging back again",
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


def test_history_zoom_presets(device: DeviceClient):
    device.populate_temperature_history()
    _open_cycles(device)
    device.wait_for_widget(tag="temperature_history_screen")
    for tag in ("history_zoom_8h", "history_zoom_4h", "history_zoom_2h",
                "history_zoom_1h"):
        device.wait_for_widget(tag=tag)

    def step_labels() -> set[str]:
        return {w.text.split(" ", 1)[1] for w in device.widgets
                if w.text and w.text[0] in "\uf053\uf054" and " " in w.text}

    def highlighted(tag: str) -> bool:
        on = device.find_widget(tag=tag)
        off = device.find_widget(tag="history_zoom_4h")
        return on is not None and off is not None and on.bg_color != off.bg_color

    try:
        assert highlighted("history_zoom_8h")
        assert step_labels() >= {"8 h", "1 h"}, step_labels()
        device.click(tag="history_zoom_1h")
        device.wait_until(
            "1 h zoom highlighted with 15 min steps",
            lambda: highlighted("history_zoom_1h") and step_labels() >= {"1 h", "15 min"},
            timeout=5.0,
        )
        # Zooming keeps the live window, and the fine steps still page.
        w = device.find_widget(tag="temperature_history_next")
        assert w is not None and w.disabled
        device.click(tag="temperature_history_back_hour")
        device.wait_until(
            "15 min forward enabled after stepping back",
            lambda: (w := device.find_widget(tag="temperature_history_forward_hour"))
            is not None and not w.disabled,
            timeout=5.0,
        )
        # The zoom survives leaving and reopening the page.
        device.click(tag="nav_home")
        assert device.wait_for_screen("main", timeout=5.0)
        _open_cycles(device)
        device.wait_for_widget(tag="history_zoom_1h")
        device.wait_until("1 h zoom kept", lambda: highlighted("history_zoom_1h"),
                          timeout=5.0)
    finally:
        if device.has_widget(tag="history_zoom_8h"):
            device.click(tag="history_zoom_8h")
        device.click(tag="nav_home")
        device.wait_for_screen("main", timeout=5.0)


def test_history_chart_drag_pans_and_hold_scrubs(device: DeviceClient):
    """A quick sideways drag pans the window back in time; press-and-hold
    first, then drag, only moves the cursor."""
    device.populate_temperature_history()
    _open_cycles(device)
    device.wait_for_widget(tag="temperature_history_screen")
    device.wait_for_widget(tag="temperature_history_chart")

    def next_enabled() -> bool:
        w = device.find_widget(tag="temperature_history_next")
        return w is not None and not w.disabled

    try:
        device.wait_until("chart opens on the latest window",
                          lambda: not next_enabled(), timeout=5.0)
        # Hold past the long-press time, then drag: a scrub, not a pan.
        device.drag(150, 200, 450, 200, tag="temperature_history_chart",
                    hold_ms=700, duration_ms=300)
        assert not next_enabled(), "hold-then-drag panned the chart"
        assert not device.has_widget(tag="temperature_history_latest")

        # Quick drag right reaches back in time.
        device.drag(150, 200, 450, 200, tag="temperature_history_chart")
        device.wait_until("drag panned back in time", next_enabled, timeout=5.0)
        device.wait_for_widget(tag="temperature_history_latest")

        # Dragging left returns to the live window and no further.
        device.drag(500, 200, 50, 200, tag="temperature_history_chart")
        device.wait_until("drag panned back to latest",
                          lambda: not next_enabled(), timeout=5.0)
    finally:
        device.click(tag="nav_home")
        device.wait_for_screen("main", timeout=5.0)


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
    _open_cycles(device)
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
