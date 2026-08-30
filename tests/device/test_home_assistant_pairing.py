"""Physical Home Assistant pairing-screen tests."""

import re
from urllib.parse import urlsplit

import pytest

from device_client import DeviceClient


@pytest.fixture(autouse=True)
def _revoke_integration_token(device: DeviceClient):
    """Leave the controller unpaired after each test.

    Several tests pair the controller (issuing an integration token). Without
    cleanup the device is left in a "Paired" state with the revoke button
    visible, which misrepresents a fresh device on the next boot/CI run.
    """
    yield
    try:
        device.session.delete(f"{device.base_url}/api/test/ha-token", timeout=10)
    except Exception:
        pass


def _open_pairing_screen(device: DeviceClient) -> None:
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_home_assistant")
    assert device.wait_for_screen("home_assistant", timeout=5.0)
    assert device.wait_for_widget(tag="home_assistant_device_name", timeout=5.0)


def test_pairing_screen_shows_identity_and_one_time_code(
    device: DeviceClient,
):
    _open_pairing_screen(device)

    device_name = device.find_widget(tag="home_assistant_device_name")
    assert device_name is not None
    assert device_name.text is not None
    assert re.fullmatch(
        r"Macon Heat Pump Controller [0-9A-F]{4}", device_name.text
    )

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

    device.wait_until(
        "home_assistant status shows Paired",
        lambda: (w := device.find_widget(tag="home_assistant_status")) is not None
        and w.text == "Paired",
        timeout=5.0,
    )
    status = device.find_widget(tag="home_assistant_status")
    assert status is not None
    assert status.text == "Paired"


def test_revoke_button_hidden_until_paired(device: DeviceClient):
    """The revoke button only appears once an integration token exists."""
    device.session.delete(f"{device.base_url}/api/test/ha-token", timeout=10)
    _open_pairing_screen(device)
    assert device.find_widget(tag="home_assistant_revoke") is None

    device.session.post(f"{device.base_url}/api/test/ha-token", timeout=10)
    device.click(tag="home_assistant_back")
    assert device.wait_for_screen("settings", timeout=5.0)
    device.click(tag="settings_home_assistant")
    assert device.wait_for_screen("home_assistant", timeout=5.0)
    assert device.wait_for_widget(tag="home_assistant_revoke", timeout=5.0)
    assert device.find_widget(tag="home_assistant_revoke") is not None


def test_revoke_modal_confirm_label_is_concise(device: DeviceClient):
    """Revoke modal confirm button uses the short 'Revoke' label so the text
    does not overflow the fixed-width button."""
    device.session.post(f"{device.base_url}/api/test/ha-token", timeout=10)
    _open_pairing_screen(device)
    device.click(tag="home_assistant_revoke")
    assert device.wait_for_widget(text="Revoke", timeout=5.0)
    # The full "Revoke Home Assistant" wording remains on the background
    # screen button; the modal confirm button must be the concise action.
    assert device.find_widget(text="Revoke") is not None
