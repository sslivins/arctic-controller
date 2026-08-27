"""
Arctic Controller — Device UI test client.

Wraps the /api/test/* instrumentation endpoints so tests read like plain English:

    device.click(tag="settings")
    assert device.screen == "settings"
"""

import os
import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry
import urllib3
import time
from dataclasses import dataclass, field
from typing import Optional

# Suppress InsecureRequestWarning when verify=False (wildcard cert won't match .local)
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


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

        # Disable TLS certificate verification (wildcard cert won't match .local)
        self.session.verify = False

        # Retry transient connection errors automatically (ESP32 can be flaky)
        retry_strategy = Retry(
            total=3,
            backoff_factor=1,        # sleeps 0 s, 1 s, 2 s between retries
            allowed_methods=None,    # retry on all HTTP methods
            status_forcelist=[502, 503, 504],
        )
        adapter = HTTPAdapter(max_retries=retry_strategy)
        self.session.mount("http://", adapter)
        self.session.mount("https://", adapter)

        # Authenticate against production endpoints if an API key is available
        api_key = os.environ.get("ARCTIC_API_KEY", "")
        if api_key:
            self.session.headers["X-API-Key"] = api_key

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

    def _screen_payload(self) -> dict:
        """Raw ``/api/test/screen`` JSON: ``{"screen": ..., "settled": ...}``.

        ``settled`` is ``True`` only when no screen-load transition is in flight
        (LVGL's ``scr_to_load`` cleared) — i.e. the active screen, its widget
        tree, and the firmware's per-module visibility flags all agree. Older
        firmware without the field is treated as always-settled for
        backward-compatibility.
        """
        r = self.session.get(
            f"{self.base_url}/api/test/screen", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()

    @property
    def screen(self) -> str:
        """Current screen name (e.g. 'main', 'settings').

        Uses the lightweight /api/test/screen endpoint (no widget tree walk).
        """
        return self._screen_payload()["screen"]

    @property
    def screen_settled(self) -> bool:
        """Whether the current screen is settled (no load animation in flight)."""
        return bool(self._screen_payload().get("settled", True))

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
        """POST /api/test/click — click a widget by tag, symbol, or label text.

        Returns dict with ``success``, ``clicked_type``, ``clicked_text``, and
        ``render_time_us`` (microseconds spent inside ``lv_obj_send_event``).
        """
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

    def scroll_to(self, tag: str, y: int) -> dict:
        """POST /api/test/scroll — scroll a tagged container vertically."""
        r = self.session.post(
            f"{self.base_url}/api/test/scroll",
            json={"tag": tag, "y": y},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Scroll failed ({r.status_code}): {msg}")
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

    def set_display_idle(self, action: str) -> dict:
        """Force or inspect display idle state through test instrumentation."""
        r = self.session.post(
            f"{self.base_url}/api/test/display-idle",
            json={"action": action},
            timeout=self.timeout,
        )
        r.raise_for_status()
        return r.json()

    def get_preferences(self) -> dict:
        """GET /api/preferences — returns {demo_mode, temp_unit, brightness, language}."""
        r = self.session.get(
            f"{self.base_url}/api/preferences", timeout=self.timeout
        )
        r.raise_for_status()
        return r.json()

    def set_preference(self, **prefs) -> dict:
        """POST /api/test/set-preference — set preferences directly (no UI).

        Example: device.set_preference(demo_mode=True)
        Bypasses the settings UI — no reboot confirmation panel.
        """
        r = self.session.post(
            f"{self.base_url}/api/test/set-preference",
            json=prefs,
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Set preference failed ({r.status_code}): {msg}")
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

    def inject_fault(self, code: str, active: bool = True) -> dict:
        """POST /api/test/inject-fault — set or clear a fault by its Macon code.

        The (code -> register,bit) mapping is owned by the arctic-macon library,
        so tests never hardcode bit positions. A code such as ``E28``/``E05``
        maps to two register-bit sites; the response's ``sites_written`` reports
        how many were touched. Raises DeviceError for an unknown code (HTTP 400).

        Example: device.inject_fault("P02")           # activate P02
                 device.inject_fault("P02", False)    # clear P02
        """
        r = self.session.post(
            f"{self.base_url}/api/test/inject-fault",
            json={"code": code, "active": active},
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Inject fault failed ({r.status_code}): {msg}")
        return r.json()

    def clear_all_faults(self) -> dict:
        """POST /api/test/clear-faults — clear every active fault atomically."""
        r = self.session.post(
            f"{self.base_url}/api/test/clear-faults",
            timeout=self.timeout,
        )
        if r.status_code >= 400:
            try:
                msg = r.json().get("error", r.text)
            except Exception:
                msg = r.text
            raise DeviceError(f"Clear faults failed ({r.status_code}): {msg}")
        return r.json()

    def populate_temperature_history(self) -> dict:
        """Replace telemetry history with an eight-hour graph fixture."""
        r = self.session.post(
            f"{self.base_url}/api/test/populate-temperature-history",
            timeout=30,
        )
        r.raise_for_status()
        return r.json()

    def reboot(self) -> dict:
        """POST /api/ota/reboot — immediately reboot the device.

        The device sends the response then reboots after ~500ms.
        Use wait_for_device() afterwards to wait for it to come back.
        """
        r = self.session.post(
            f"{self.base_url}/api/ota/reboot",
            timeout=self.timeout,
        )
        r.raise_for_status()
        return r.json()

    def wait_for_device(self, timeout: float = 30.0, poll: float = 1.0) -> bool:
        """Poll until the device responds to a health check after a reboot.

        Waits for the HTTP server to come back up. Use after reboot().
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                self.get_ui_state()
                return True
            except Exception:
                pass
            time.sleep(poll)
        return False

    def wait_for_connected(self, timeout: float = 10.0, poll: float = 0.5) -> bool:
        """Poll /api/heatpump/status until connected=true, or timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                status = self.get_heatpump_status()
                if status.get("connected"):
                    return True
            except Exception:
                pass
            time.sleep(poll)
        return False

    def wait_for_screen(self, name: str, timeout: float = 3.0, poll: float = 0.3,
                        raise_on_timeout: bool = True) -> bool:
        """Wait until the current screen matches ``name`` *and* is settled.

        "Settled" means no screen-load animation is in flight, so the active
        screen, its widget tree, and the firmware's visibility flags all agree.
        Waiting only for the name match returned too early during the ~300ms
        load animation — the endpoint reported the *scheduled* screen while
        ``lv_scr_act()`` (and thus the widget tree / click targets) still
        pointed at the previous one, so a follow-up action could 404 or hit the
        old screen. Gating on ``settled`` closes that scheduled-vs-settled race.

        Raises ``DeviceError`` on timeout by default (with the last observed
        screen and widget tags for diagnostics). Pass ``raise_on_timeout=False``
        — or use :meth:`try_wait_screen` — in cleanup/best-effort paths where a
        miss should not fail the test.
        """
        def _on_settled_screen() -> bool:
            payload = self._screen_payload()
            return payload.get("screen") == name and payload.get("settled", True)

        return self.wait_until(
            f"settled screen {name!r}",
            _on_settled_screen,
            timeout=timeout, poll=poll, raise_on_timeout=raise_on_timeout,
        )

    def try_wait_screen(self, name: str, timeout: float = 3.0, poll: float = 0.3) -> bool:
        """Best-effort :meth:`wait_for_screen` — returns bool, never raises."""
        return self.wait_for_screen(name, timeout=timeout, poll=poll, raise_on_timeout=False)

    def wait_for_widget(self, *, tag: Optional[str] = None, text: Optional[str] = None,
                        timeout: float = 5.0, poll: float = 0.3,
                        raise_on_timeout: bool = True) -> bool:
        """Wait until a widget with the given tag or text appears in the tree.

        Raises ``DeviceError`` on timeout by default. Use
        :meth:`try_wait_widget` (or ``raise_on_timeout=False``) in best-effort
        paths.
        """
        what = f"widget tag={tag!r}" if tag else f"widget text={text!r}"
        return self.wait_until(
            what,
            lambda: self.has_widget(tag=tag, text=text),
            timeout=timeout, poll=poll, raise_on_timeout=raise_on_timeout,
        )

    def try_wait_widget(self, *, tag: Optional[str] = None, text: Optional[str] = None,
                        timeout: float = 5.0, poll: float = 0.3) -> bool:
        """Best-effort :meth:`wait_for_widget` — returns bool, never raises."""
        return self.wait_for_widget(tag=tag, text=text, timeout=timeout, poll=poll,
                                    raise_on_timeout=False)

    def wait_until(self, description: str, predicate, timeout: float = 5.0,
                   poll: float = 0.3, raise_on_timeout: bool = True) -> bool:
        """Poll ``predicate()`` until it returns truthy, or ``timeout`` elapses.

        This is the shared condition-based wait primitive that replaces fixed
        ``time.sleep()`` band-aids: instead of sleeping a guessed interval, wait
        for the exact observable condition. Exceptions raised by ``predicate``
        (e.g. a transient HTTP error) are treated as "not yet" and retried.

        On timeout, raises ``DeviceError`` including ``description`` plus a
        best-effort snapshot of the current screen and widget tags — so a
        failure says what it was waiting for and what the device was actually
        showing. Pass ``raise_on_timeout=False`` to get a bool back instead
        (for cleanup/best-effort checks).
        """
        deadline = time.time() + timeout
        while True:
            try:
                if predicate():
                    return True
            except Exception:
                pass
            if time.time() >= deadline:
                break
            time.sleep(poll)
        if raise_on_timeout:
            raise DeviceError(
                f"Timed out after {timeout:.1f}s waiting for {description}. {self._diag()}"
            )
        return False

    def _diag(self) -> str:
        """Best-effort snapshot of screen + widget tags for error messages."""
        try:
            payload = self._screen_payload()
            screen = payload.get("screen")
            settled = payload.get("settled", True)
        except Exception as e:
            return f"(device unreachable: {e})"
        try:
            tags = sorted(w.tag for w in self.widgets if w.tag)
        except Exception:
            tags = []
        return f"Last observed screen={screen!r} (settled={settled}), widget tags={tags}"

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

    def get_heatpump_errors(self) -> dict:
        """GET /api/heatpump/errors — returns active errors and error history.

        Response includes: demo_mode, connected, has_errors, error_count,
        highest_severity, active (array), history (array).
        """
        r = self.session.get(
            f"{self.base_url}/api/heatpump/errors", timeout=self.timeout
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
