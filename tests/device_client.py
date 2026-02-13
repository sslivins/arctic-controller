"""
Arctic Controller — Device UI test client.

Wraps the /api/test/* instrumentation endpoints so tests read like plain English:

    device.click(tag="settings")
    assert device.screen == "settings"
"""

import requests
import time
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class Widget:
    """A single LVGL widget reported by /api/test/ui-state."""
    type: str
    x: int
    y: int
    w: int
    h: int
    text: Optional[str] = None
    text_en: Optional[str] = None
    tag: Optional[str] = None
    checked: Optional[bool] = None
    disabled: Optional[bool] = None
    value: Optional[int] = None
    min: Optional[int] = None
    max: Optional[int] = None
    password_mode: Optional[bool] = None


class DeviceError(Exception):
    """Raised when the device returns an error response."""
    pass


class DeviceClient:
    """HTTP client for the Arctic Controller test instrumentation API."""

    def __init__(self, base_url: str = "http://arctic.local", timeout: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()

    # ------------------------------------------------------------------
    # UI State
    # ------------------------------------------------------------------

    def get_ui_state(self) -> dict:
        """GET /api/test/ui-state — returns full widget tree + screen name."""
        r = self.session.get(
            f"{self.base_url}/api/test/ui-state", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()

    @property
    def screen(self) -> str:
        """Current screen name (e.g. 'main', 'settings')."""
        return self.get_ui_state()["screen"]

    @property
    def widgets(self) -> list[Widget]:
        """All visible widgets on the current screen."""
        data = self.get_ui_state()
        return [Widget(**w) for w in data.get("widgets", [])]

    # ------------------------------------------------------------------
    # Click
    # ------------------------------------------------------------------

    def click(
        self,
        *,
        tag: Optional[str] = None,
        symbol: Optional[str] = None,
        label: Optional[str] = None,
        label_contains: Optional[str] = None,
    ) -> dict:
        """POST /api/test/click — click a widget by tag, symbol, or label text."""
        body: dict = {}
        if tag is not None:
            body["tag"] = tag
        elif symbol is not None:
            body["symbol"] = symbol
        elif label is not None:
            body["label"] = label
        elif label_contains is not None:
            body["label_contains"] = label_contains
        else:
            raise ValueError("Provide one of: tag, symbol, label, label_contains")

        r = self.session.post(
            f"{self.base_url}/api/test/click",
            json=body,
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Click failed ({r.status_code}): {msg}")
        return r.json()

    def set_slider(self, tag: str, value: int) -> dict:
        """POST /api/test/set-slider — set a slider's value by tag."""
        r = self.session.post(
            f"{self.base_url}/api/test/set-slider",
            json={"tag": tag, "value": value},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Set slider failed ({r.status_code}): {msg}")
        return r.json()

    def toggle(self, tag: str) -> dict:
        """POST /api/test/toggle — toggle a switch widget by tag. Returns {success, checked}."""
        r = self.session.post(
            f"{self.base_url}/api/test/toggle",
            json={"tag": tag},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Toggle failed ({r.status_code}): {msg}")
        return r.json()

    # ------------------------------------------------------------------
    # Display
    # ------------------------------------------------------------------

    def get_brightness(self) -> int:
        """GET /api/display/brightness — returns the current backlight brightness %."""
        r = self.session.get(
            f"{self.base_url}/api/display/brightness", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()["brightness"]

    def get_preferences(self) -> dict:
        """GET /api/preferences — returns {demo_mode, temp_unit, brightness, language}."""
        r = self.session.get(
            f"{self.base_url}/api/preferences", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()

    # ------------------------------------------------------------------
    # WiFi mock
    # ------------------------------------------------------------------

    def wifi_mock(self, networks: list[dict]) -> dict:
        """POST /api/test/wifi-mock — inject fake networks, pause scan timer.

        Each network dict: {"ssid": "Name", "rssi": -50, "authmode": 3}
        authmode: 0=open, 3=WPA2, 4=WPA3
        """
        r = self.session.post(
            f"{self.base_url}/api/test/wifi-mock",
            json={"networks": networks},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"WiFi mock failed ({r.status_code}): {msg}")
        return r.json()

    def wifi_mock_reset(self) -> dict:
        """POST /api/test/wifi-mock-reset — exit mock mode, resume scanning."""
        r = self.session.post(
            f"{self.base_url}/api/test/wifi-mock-reset",
            json={},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"WiFi mock reset failed ({r.status_code}): {msg}")
        return r.json()

    def type_text(self, tag: str, text: str) -> dict:
        """POST /api/test/type-text — set text in a textarea widget.

        Returns {success, text, password_mode}.
        """
        r = self.session.post(
            f"{self.base_url}/api/test/type-text",
            json={"tag": tag, "text": text},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Type text failed ({r.status_code}): {msg}")
        return r.json()

    # ------------------------------------------------------------------
    # Convenience helpers
    # ------------------------------------------------------------------

    def wait_for_screen(self, name: str, timeout: float = 3.0, poll: float = 0.3) -> bool:
        """Poll until the screen name matches, or timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.screen == name:
                return True
            time.sleep(poll)
        return False

    def wait_for_widget(self, *, tag: Optional[str] = None, text: Optional[str] = None,
                        timeout: float = 5.0, poll: float = 0.3) -> bool:
        """Poll until a widget with the given tag or text appears in the tree."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.has_widget(tag=tag, text=text):
                return True
            time.sleep(poll)
        return False

    def find_widget(self, *, tag: Optional[str] = None, text: Optional[str] = None) -> Optional[Widget]:
        """Find a widget by tag or text in the current tree."""
        for w in self.widgets:
            if tag and w.tag == tag:
                return w
            if text and (w.text == text or w.text_en == text):
                return w
        return None

    def has_widget(self, *, tag: Optional[str] = None, text: Optional[str] = None) -> bool:
        """Check if a widget with the given tag or text exists."""
        return self.find_widget(tag=tag, text=text) is not None
