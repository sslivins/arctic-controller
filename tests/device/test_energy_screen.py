"""Device tests for the History tab's Energy segment and the Cycles power view."""

from device_client import DeviceClient


def _disabled(device: DeviceClient, tag: str) -> bool:
    w = device.find_widget(tag=tag)
    return w is not None and bool(w.disabled)


def _enabled(device: DeviceClient, tag: str) -> bool:
    w = device.find_widget(tag=tag)
    return w is not None and not w.disabled


def _open_energy(device: DeviceClient) -> None:
    device.click(tag="nav_history")
    device.wait_for_widget(tag="history_seg_energy")
    device.click(tag="history_seg_energy")
    device.wait_for_widget(tag="energy_main_view")
    assert device.wait_for_screen("energy", timeout=5.0)


def test_energy_segment_pages_and_opens_a_day(device: DeviceClient):
    device.populate_temperature_history()
    _open_energy(device)
    for tag in ("energy_tile_today", "energy_tile_yesterday", "energy_tile_window",
                "energy_chart", "energy_range"):
        device.wait_for_widget(tag=tag)

    # The window opens on the latest days: forward is disabled and Latest hidden.
    device.wait_until("next window disabled at latest",
                      lambda: _disabled(device, "energy_next_window"), timeout=5.0)
    assert not device.has_widget(tag="energy_latest")
    device.click(tag="energy_prev_window")
    device.wait_until("next window enabled after paging back",
                      lambda: _enabled(device, "energy_next_window"), timeout=5.0)
    device.wait_for_widget(tag="energy_latest")
    device.click(tag="energy_latest")
    device.wait_until("Latest jumps back and hides itself",
                      lambda: not device.has_widget(tag="energy_latest"), timeout=5.0)

    # Today's tile opens the day view; Back returns to the chart.
    device.click(tag="energy_tile_today")
    device.wait_for_widget(tag="energy_day_view")
    for tag in ("energy_day_title", "energy_day_elec", "energy_day_heat",
                "energy_day_cop", "energy_day_outdoor", "energy_day_facts",
                "energy_runs_title"):
        device.wait_for_widget(tag=tag)
    device.click(tag="energy_day_back")
    device.wait_for_widget(tag="energy_main_view")
    assert not device.has_widget(tag="energy_day_view")

    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)


def test_energy_run_opens_cycles_and_power_view(device: DeviceClient):
    device.populate_temperature_history()
    _open_energy(device)
    device.click(tag="energy_tile_today")
    device.wait_for_widget(tag="energy_day_view")
    # The populated history has a run every two hours over the last eight, so
    # today has one unless the test runs just after midnight.
    device.wait_until(
        "runs list loaded",
        lambda: device.has_widget(tag="energy_run")
        or device.has_widget(tag="energy_runs_status"),
        timeout=10.0,
    )
    if device.has_widget(tag="energy_run"):
        device.click(tag="energy_run")
        device.wait_for_widget(tag="temperature_history_screen")
        assert device.wait_for_screen("history", timeout=5.0)
        # Centred on an earlier run, not the latest window.
        device.wait_for_widget(tag="temperature_history_latest")
    else:
        device.click(tag="history_seg_cycles")
        device.wait_for_widget(tag="temperature_history_screen")

    device.click(tag="history_view_power")
    device.wait_for_widget(tag="history_power_total")
    device.click(tag="history_view_temps")
    device.wait_until("power total hidden on the temperatures view",
                      lambda: not device.has_widget(tag="history_power_total"),
                      timeout=5.0)

    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)


def test_home_today_energy_opens_energy_segment(device: DeviceClient):
    device.set_demo_fields(compressor_freq=0, fan_on=0, fan_speed=0, pump_on=1, unit_on=1)
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)
    # Idle, the strip's middle slot shows today's energy and opens Energy.
    device.wait_until(
        "strip shows today's energy",
        lambda: (w := device.find_widget(tag="home_strip_1")) is not None
        and "kWh" in (w.text or ""),
        timeout=10.0,
    )
    device.click(tag="home_strip_1")
    device.wait_for_widget(tag="energy_main_view")
    assert device.wait_for_screen("energy", timeout=5.0)
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)
