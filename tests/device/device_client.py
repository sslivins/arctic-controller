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
    option_count: Optional[int] = None
    selected_text: Optional[str] = None
    bg_color: Optional[str] = None


class DeviceError(Exception):
    """Raised when the device returns an error response."""
    pass


class DeviceClient:
    """HTTP client for the Arctic Controller test instrumentation API."""

    def __init__(self, base_url: str = "http://arctic.local", timeout: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()
        self._session_id: Optional[str] = None

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
        """Current screen name (e.g. 'main', 'settings').

        Uses the lightweight /api/test/screen endpoint (no widget tree walk).
        """
        r = self.session.get(
            f"{self.base_url}/api/test/screen", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()["screen"]

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

    def set_roller(self, tag: str, index: int) -> dict:
        """POST /api/test/set-roller — set a roller's selected index by tag.

        Returns {success, value, selected_text}.
        """
        r = self.session.post(
            f"{self.base_url}/api/test/set-roller",
            json={"tag": tag, "index": index},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Set roller failed ({r.status_code}): {msg}")
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

    # ------------------------------------------------------------------
    # Firmware mock
    # ------------------------------------------------------------------

    def firmware_mock(self, version: str, update_available: bool = True) -> dict:
        """POST /api/test/firmware-mock — inject fake firmware check result.

        Sets the firmware screen to show the given version and update state,
        bypassing the real GitHub check.
        """
        r = self.session.post(
            f"{self.base_url}/api/test/firmware-mock",
            json={"version": version, "update_available": update_available},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Firmware mock failed ({r.status_code}): {msg}")
        return r.json()

    def firmware_mock_reset(self) -> dict:
        """POST /api/test/firmware-mock-reset — clear mock firmware state."""
        r = self.session.post(
            f"{self.base_url}/api/test/firmware-mock-reset",
            json={},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Firmware mock reset failed ({r.status_code}): {msg}")
        return r.json()

    def notification_mock(self, notification_type: int, message: str = None) -> dict:
        """POST /api/test/notification-mock — add a notification to the status bar.

        Args:
            notification_type: Notification type (0=firmware update, 1=wifi unstable, 2=low battery)
            message: Optional message text
        """
        payload = {"type": notification_type}
        if message is not None:
            payload["message"] = message

        r = self.session.post(
            f"{self.base_url}/api/test/notification-mock",
            json=payload,
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Notification mock failed ({r.status_code}): {msg}")
        return r.json()

    def notification_mock_reset(self) -> dict:
        """POST /api/test/notification-mock-reset — clear all notifications."""
        r = self.session.post(
            f"{self.base_url}/api/test/notification-mock-reset",
            json={},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Notification mock reset failed ({r.status_code}): {msg}")
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

    def get_heatpump_status(self) -> dict:
        """GET /api/heatpump/status — returns heat pump state including temps, components, errors."""
        r = self.session.get(
            f"{self.base_url}/api/heatpump/status", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()

    def set_demo_fields(self, **fields) -> dict:
        """POST /api/test/set-demo-field — set demo state fields.

        Example: device.set_demo_fields(water_tank_temp=50, fan_speed=0)
        Only works when demo mode is enabled.
        """
        r = self.session.post(
            f"{self.base_url}/api/test/set-demo-field",
            json=fields,
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Set demo fields failed ({r.status_code}): {msg}")
        return r.json()

    def clear_error_history(self) -> dict:
        """POST /api/test/clear-error-history — clear the error history ring buffer."""
        r = self.session.post(
            f"{self.base_url}/api/test/clear-error-history",
            timeout=self.timeout,
        )
        r.raise_for_status()
        return r.json()

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

    # ------------------------------------------------------------------
    # Session Lock
    # ------------------------------------------------------------------

    def lock(self, ttl_seconds: int = 900) -> dict:
        """POST /api/test/lock — acquire exclusive device lock.

        Generates a unique session ID and sends it as X-Session-Id header
        on all subsequent requests.
        """
        import uuid
        self._session_id = str(uuid.uuid4())
        self.session.headers["X-Session-Id"] = self._session_id
        r = self.session.post(
            f"{self.base_url}/api/test/lock",
            json={"session_id": self._session_id, "ttl_seconds": ttl_seconds},
            timeout=self.timeout,
        )
        if r.status_code == 423:
            data = r.json()
            raise DeviceError(
                f"Device is locked by another session: {data.get('locked_by', '?')} "
                f"(expires in {data.get('remaining_seconds', '?')}s)"
            )
        r.raise_for_status()
        return r.json()

    def unlock(self, force: bool = False) -> dict:
        """POST /api/test/unlock — release device lock."""
        payload = {"session_id": self._session_id or ""}
        if force:
            payload["force"] = True
        r = self.session.post(
            f"{self.base_url}/api/test/unlock",
            json=payload,
            timeout=self.timeout,
        )
        r.raise_for_status()
        self._session_id = None
        self.session.headers.pop("X-Session-Id", None)
        return r.json()

    def check_lock(self) -> dict:
        """GET /api/test/lock — check if device is locked."""
        r = self.session.get(
            f"{self.base_url}/api/test/lock", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()

    def screenshot(self, path: str = "screenshot.png") -> str:
        """GET /api/test/screenshot — capture display as PNG and save to path.

        Returns the path the screenshot was saved to.
        """
        r = self.session.get(
            f"{self.base_url}/api/test/screenshot", timeout=30.0
        )
        r.raise_for_status()
        with open(path, "wb") as f:
            f.write(r.content)
        return path
