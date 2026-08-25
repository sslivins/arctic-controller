"""Physical Web Interface settings-screen tests (QR code + dashboard URL)."""

import time

from device_client import DeviceClient


def _open_web_screen(device: DeviceClient) -> None:
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.5)
    device.click(tag="settings_web")
    assert device.wait_for_screen("web", timeout=5.0)
    time.sleep(0.5)


def test_web_screen_shows_url(device: DeviceClient):
    """The Web Interface screen exposes the dashboard URL as plain text."""
    _open_web_screen(device)

    url = device.find_widget(tag="web_url")
    assert url is not None
    assert url.text is not None and url.text != ""


def test_web_screen_shows_qr_code(device: DeviceClient):
    """When the device has a hostname, the QR code and its URL are shown.

    The device under test is reachable over the network, so it has an mDNS
    hostname and the URL should be a scannable ``http(s)://...local`` address.
    """
    _open_web_screen(device)

    url = device.find_widget(tag="web_url")
    assert url is not None and url.text is not None
    assert ".local" in url.text
    assert url.text.startswith("http")

    qr = device.find_widget(tag="web_qrcode")
    assert qr is not None


def test_web_screen_back_returns_to_settings(device: DeviceClient):
    _open_web_screen(device)
    device.click(tag="web_back")
    assert device.wait_for_screen("settings", timeout=5.0)
