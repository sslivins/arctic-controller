"""Device tests for the eight-hour temperature history graph."""

import time

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

    device.click(tag="temperature_history_previous")
    time.sleep(0.5)
    device.click(tag="temperature_history_next")
    time.sleep(0.5)
    device.click(tag="temperature_history_back")
    device.wait_for_widget(tag="temperature_history_open")
