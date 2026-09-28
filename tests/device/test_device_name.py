"""Tests for the friendly controller name shown on Home."""

import pytest
import requests

from device_client import DeviceClient


NAME = "Heat Pump 1 – Radiant Floor"
ACCENTED_NAME = "Pompe à chaleur – Étage"
MAX_LENGTH_NAME = "Radiant Floor Heat Pump 123456"


@pytest.fixture(autouse=True)
def _restore_device_name(device: DeviceClient):
    try:
        device.update_preferences(device_name="")
    except Exception:
        pass
    yield
    device.update_preferences(device_name="")


def _name_widget(device: DeviceClient):
    return device.find_widget(tag="home_device_name")


def _wait_name(device: DeviceClient, expected: str):
    device.wait_until(
        f"home device name is {expected!r}",
        lambda: (w := _name_widget(device)) is not None and w.text and expected in w.text,
        timeout=5.0,
        expect_within=1.0,
    )
    return _name_widget(device)


class TestDeviceNameHome:
    def test_name_is_hidden_when_blank(self, device: DeviceClient):
        device.update_preferences(device_name="")
        device.wait_until(
            "home device name hidden",
            lambda: _name_widget(device) is None,
            timeout=5.0,
            expect_within=1.0,
        )

    def test_name_is_shown_on_home_and_updates_live(self, device: DeviceClient):
        device.update_preferences(device_name=NAME)
        widget = _wait_name(device, NAME)
        assert widget.font_px >= 32
        assert widget.missing_glyphs in (None, 0)

        device.update_preferences(device_name="Heat Pump 2")
        widget = _wait_name(device, "Heat Pump 2")
        assert widget.font_px >= 32
        assert widget.missing_glyphs in (None, 0)

        device.update_preferences(device_name="")
        device.wait_until(
            "home device name hidden after clearing",
            lambda: _name_widget(device) is None,
            timeout=5.0,
            expect_within=1.0,
        )

    def test_accented_name_has_no_missing_glyphs(self, device: DeviceClient):
        device.update_preferences(device_name=ACCENTED_NAME)
        widget = _wait_name(device, ACCENTED_NAME)
        assert widget.font_px >= 32
        assert widget.missing_glyphs in (None, 0)


class TestDeviceNameSettings:
    def test_settings_row_value_stays_single_line_for_max_length_name(
        self, device: DeviceClient
    ):
        assert len(MAX_LENGTH_NAME) == 30
        device.update_preferences(device_name=MAX_LENGTH_NAME)

        device.click(tag="settings")
        assert device.wait_for_screen("settings", timeout=5.0)
        assert device.wait_for_widget(tag="settings_device_name_value", timeout=5.0)

        row = device.find_widget(tag="settings_device_name")
        value = device.find_widget(tag="settings_device_name_value")
        assert row is not None
        assert value is not None
        assert value.text is not None
        assert "\n" not in value.text
        assert value.h <= 40
        assert value.x + value.w <= row.x + row.w - 45


class TestDeviceNameApiValidation:
    def _patch(self, device: DeviceClient, name: str):
        return device.session.patch(
            f"{device.base_url}/api/preferences",
            json={"device_name": name},
            timeout=device.timeout,
        )

    def test_emoji_is_rejected(self, device: DeviceClient):
        response = self._patch(device, "Heat Pump 😀")
        assert response.status_code == 400
        assert "display cannot show" in response.json()["error"]

    def test_31_characters_are_rejected(self, device: DeviceClient):
        response = self._patch(device, "A" * 31)
        assert response.status_code == 400
        assert "30 characters" in response.json()["error"]

    def test_30_characters_are_accepted(self, device: DeviceClient):
        name = "A" * 30
        response = self._patch(device, name)
        assert response.status_code == 200
        assert response.json()["device_name"] == name
