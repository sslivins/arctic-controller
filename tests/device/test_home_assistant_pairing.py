"""Physical Home Assistant pairing-screen tests."""

import re
import time
from urllib.parse import urlsplit

from device_client import DeviceClient


def _open_pairing_screen(device: DeviceClient) -> None:
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_home_assistant")
    assert device.wait_for_screen("home_assistant", timeout=5.0)
    time.sleep(0.5)


def test_pairing_screen_shows_identity_and_one_time_code(
    device: DeviceClient,
):
    _open_pairing_screen(device)

    fingerprint = device.find_widget(tag="home_assistant_fingerprint")
    assert fingerprint is not None
    assert fingerprint.text is not None
    assert len(re.sub(r"\s", "", fingerprint.text)) == 64

    device.click(tag="home_assistant_pair")
    code = device.find_widget(tag="home_assistant_code")
    countdown = device.find_widget(tag="home_assistant_countdown")
    assert code is not None
    assert code.text is not None
    assert re.fullmatch(r"\d{6}", code.text)
    assert countdown is not None
    assert countdown.text is not None
    assert re.search(r"\d+:\d{2}", countdown.text)


def test_leaving_pairing_screen_closes_window(device: DeviceClient):
    _open_pairing_screen(device)
    device.click(tag="home_assistant_pair")
    code = device.find_widget(tag="home_assistant_code")
    assert code is not None and code.text is not None

    device.click(tag="home_assistant_back")
    assert device.wait_for_screen("settings", timeout=5.0)

    host = urlsplit(device.base_url).hostname
    response = device.session.post(
        f"https://{host}:8443/api/v1/pair",
        json={"code": code.text},
        timeout=10,
    )
    assert response.status_code == 403


def test_controller_code_completes_pairing(device: DeviceClient):
    _open_pairing_screen(device)
    device.click(tag="home_assistant_pair")
    code = device.find_widget(tag="home_assistant_code")
    assert code is not None and code.text is not None

    host = urlsplit(device.base_url).hostname
    response = device.session.post(
        f"https://{host}:8443/api/v1/pair",
        json={"code": code.text},
        timeout=10,
    )
    response.raise_for_status()
    assert len(response.json()["token"]) == 64

    time.sleep(1.5)
    status = device.find_widget(tag="home_assistant_status")
    assert status is not None
    assert status.text == "Paired"
