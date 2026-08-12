"""Physical Security-screen tests (Home-Assistant-independent securing)."""

import re
import time

from device_client import DeviceClient


def _open_security_screen(device: DeviceClient) -> None:
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_security")
    assert device.wait_for_screen("security", timeout=5.0)
    time.sleep(0.5)


def test_security_screen_shows_status_without_home_assistant(
    device: DeviceClient,
):
    _open_security_screen(device)

    status = device.find_widget(tag="security_status")
    assert status is not None
    assert status.text in ("Secured", "Not secured")


def test_security_screen_reveals_setup_code(device: DeviceClient):
    _open_security_screen(device)

    device.click(tag="security_show_code")
    code = device.find_widget(tag="security_code")
    countdown = device.find_widget(tag="security_countdown")
    assert code is not None
    assert code.text is not None
    assert re.fullmatch(r"\d{6}", code.text)
    assert countdown is not None
    assert countdown.text is not None
    assert re.search(r"\d+:\d{2}", countdown.text)


def test_leaving_security_screen_hides_code(device: DeviceClient):
    _open_security_screen(device)
    device.click(tag="security_show_code")
    code = device.find_widget(tag="security_code")
    assert code is not None and code.text is not None

    device.click(tag="security_back")
    assert device.wait_for_screen("settings", timeout=5.0)
