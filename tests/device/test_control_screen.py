import re
"""
Test: Control (Advanced) Screen

Navigates to the control sub-screen and verifies the power button,
mode selector, and setpoint controls are displayed correctly in demo mode.

The control screen shows:
- Power ON/OFF button (single click to turn on, 3s hold to turn off)
- 4 mode buttons (Cooling, Heating, Hot Water, Hot Water / Cooling); modes
  2-4 have no button and are reported in a "Current mode" note instead
- 3 setpoint rows (Cooling, Heating, Hot Water) with values
- Advanced Parameter rows (AP13, AP24, AP38, etc.)
"""

import pytest
from device_client import DeviceClient

# Working modes (reg2096 values, see heatpump_types.h)
MODE_COOLING = 0
MODE_HEATING = 1
MODE_FREEZE_PROTECTION = 2
MODE_HOT_WATER = 5
MODE_HOT_WATER_COOLING = 6

# Mode button labels (i18n English)
MODE_LABELS = ["COOLING", "HEATING", "HOT WATER", "HOT WATER / COOLING"]

# Demo default setpoints (from heatpump_params_screen.cpp)
DEMO_SETPOINTS = {
    "Cooling Setpoint": "18 °C",
    "Heating Setpoint": "45 °C",
    "Hot Water Setpoint": "50 °C",
}


def _open_control(device: DeviceClient):
    """Navigate from main to the control screen."""
    device.click(tag="nav_control")
    assert device.wait_for_screen("control", timeout=5.0), \
        f"Expected 'control' screen, got '{device.screen}'"


def _close_control(device: DeviceClient):
    """Return to the Home (main) tab via the persistent nav bar."""
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0), \
        f"Expected 'main' screen after close, got '{device.screen}'"


def _has_text_containing(device: DeviceClient, substring: str) -> bool:
    """Check if any widget's text contains the given substring."""
    sub_lower = substring.lower()
    for w in device.widgets:
        t = w.text_en or w.text
        if t and sub_lower in t.lower():
            return True
    return False


def _wait_for_text(device: DeviceClient, substring: str, timeout: float = 5.0):
    """Wait until some widget's text contains ``substring``."""
    device.wait_until(
        f"text containing '{substring}' visible",
        lambda: _has_text_containing(device, substring),
        timeout=timeout,
    )


@pytest.fixture(autouse=True)
def _restore_control_defaults(device: DeviceClient):
    """Restore demo control state after each test."""
    yield
    device.set_demo_fields(
        unit_on=1,
        working_mode=MODE_HEATING,
        cooling_setpoint=18,
        heating_setpoint=45,
        hot_water_setpoint=50,
    )


# =========================================================================
# Navigation
# =========================================================================

class TestControlNavigation:
    """Open/close control screen from the main screen footer."""

    def test_open_control_screen(self, device: DeviceClient):
        """Clicking the Control nav button opens the control screen."""
        _open_control(device)

    def test_close_control_screen(self, device: DeviceClient):
        """Selecting the Home tab returns to the main screen."""
        _open_control(device)
        _close_control(device)


# =========================================================================
# Power Button
# =========================================================================

class TestPowerButton:
    """Verify the power button is present and reflects unit state."""

    def test_power_button_shows_on(self, device: DeviceClient):
        """When unit is ON, power button should show POWERED ON."""
        device.set_demo_fields(unit_on=1)
        _open_control(device)
        _wait_for_text(device, "POWERED ON")

        assert _has_text_containing(device, "POWERED ON"), \
            "POWERED ON text not found on control screen"

    def test_power_button_shows_off(self, device: DeviceClient):
        """When unit is OFF, power button should show POWERED OFF."""
        device.set_demo_fields(unit_on=0)
        _open_control(device)
        _wait_for_text(device, "POWERED OFF")

        assert _has_text_containing(device, "POWERED OFF"), \
            "POWERED OFF text not found on control screen"


# =========================================================================
# Mode Buttons
# =========================================================================

class TestModeButtons:
    """Verify the 4 selectable mode buttons are present."""

    @pytest.mark.parametrize("mode_label", MODE_LABELS)
    def test_mode_button_present(self, device: DeviceClient, mode_label):
        """Each mode button should be visible on the control screen."""
        _open_control(device)
        _wait_for_text(device, mode_label)

        assert _has_text_containing(device, mode_label), \
            f"Mode button '{mode_label}' not found on screen"

    def test_no_button_for_unselectable_modes(self, device: DeviceClient):
        """Freeze protection and modes 3-4 are never offered as buttons."""
        device.set_demo_fields(working_mode=MODE_HEATING)
        _open_control(device)
        _wait_for_text(device, "HOT WATER / COOLING")

        for hidden in ("FREEZE PROTECTION", "MODE 3", "MODE 4"):
            assert not _has_text_containing(device, hidden), \
                f"Unexpected mode text '{hidden}' on control screen"

    def test_unselectable_mode_is_reported(self, device: DeviceClient):
        """When the unit is in freeze protection the screen says so without a button."""
        device.set_demo_fields(working_mode=MODE_FREEZE_PROTECTION)
        _open_control(device)
        _wait_for_text(device, "Current mode: FREEZE PROTECTION")

        device.set_demo_fields(working_mode=MODE_HEATING)
        device.wait_until(
            "current-mode note hidden",
            lambda: not _has_text_containing(device, "Current mode:"),
            timeout=5.0,
        )


# =========================================================================
# Setpoints
# =========================================================================

class TestSetpoints:
    """Verify setpoint labels and values are displayed."""

    @pytest.mark.parametrize("label,expected_value",
                             list(DEMO_SETPOINTS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_SETPOINTS])
    def test_setpoint_label_present(self, device: DeviceClient, label, expected_value):
        """Each setpoint label should be visible."""
        _open_control(device)
        _wait_for_text(device, label)

        assert _has_text_containing(device, label), \
            f"Setpoint label '{label}' not found on screen"

    @pytest.mark.parametrize("label,expected_value",
                             list(DEMO_SETPOINTS.items()),
                             ids=[k.replace(" ", "_") for k in DEMO_SETPOINTS])
    def test_setpoint_value_present(self, device: DeviceClient, label, expected_value):
        """Each setpoint value should be displayed."""
        _open_control(device)
        _wait_for_text(device, expected_value)

        assert _has_text_containing(device, expected_value), \
            f"Setpoint value '{expected_value}' for '{label}' not found"

    def test_editor_actions_are_above_nav_bar(self, device: DeviceClient):
        """Cancel and Save must remain fully visible above persistent navigation."""
        _open_control(device)
        device.click(label="Cooling Setpoint")

        try:
            nav = device.find_widget(tag="nav_bar")
            cancel = device.find_widget(tag="edit_cancel")
            save = device.find_widget(tag="edit_save")

            assert nav is not None, "Persistent nav bar not found"
            assert cancel is not None, "Editor Cancel button not found"
            assert save is not None, "Editor Save button not found"
            assert cancel.y + cancel.h <= nav.y, \
                f"Cancel button bottom {cancel.y + cancel.h} overlaps nav top {nav.y}"
            assert save.y + save.h <= nav.y, \
                f"Save button bottom {save.y + save.h} overlaps nav top {nav.y}"
        finally:
            device.click(tag="edit_cancel")

    def test_setpoint_editor_uses_slider_without_range_text(self, device: DeviceClient):
        """Setpoints use a bounded slider instead of repetitive +/- taps."""
        _open_control(device)
        device.click(label="Cooling Setpoint")

        try:
            slider = device.find_widget(tag="edit_slider")
            assert slider is not None, "Setpoint slider not found"
            assert (slider.min, slider.max) == (4, 30)
            assert not _has_text_containing(device, "Range:"), \
                "Slider editor should not repeat its enforced range as text"
        finally:
            device.click(tag="edit_cancel")

    def test_wide_numeric_parameter_uses_full_range_slider(self, device: DeviceClient):
        """AP29 compressor runtime exposes its complete 0-90 minute range."""
        _open_control(device)
        device.click(label="AP29")

        try:
            slider = device.find_widget(tag="edit_slider")
            assert slider is not None, "AP29 slider not found"
            assert (slider.min, slider.max) == (0, 90)

            device.set_slider("edit_slider", 90)
            value = device.find_widget(tag="edit_value")
            assert value is not None and value.text == "90 min"
        finally:
            device.click(tag="edit_cancel")

    def test_ap28_uses_metadata_driven_ten_minute_steps(self, device: DeviceClient):
        """AP28 shows minutes and snaps to Macon's declared display increment."""
        _open_control(device)
        device.click(label="AP28")

        try:
            slider = device.find_widget(tag="edit_slider")
            assert slider is not None, "AP28 slider not found"
            assert (slider.min, slider.max) == (0, 990)

            device.set_slider("edit_slider", 34)
            slider = device.find_widget(tag="edit_slider")
            value = device.find_widget(tag="edit_value")
            assert slider is not None and slider.value == 30
            assert value is not None and value.text == "30 min"
        finally:
            device.click(tag="edit_cancel")

    @pytest.mark.parametrize(
        "label_fragment,option_count",
        [("AP14", 8), ("AP44", 3)],
    )
    def test_discrete_parameter_uses_roller(
        self, device: DeviceClient, label_fragment: str, option_count: int
    ):
        """Discrete AP choices use a named roller rather than a numeric slider."""
        _open_control(device)
        device.click(label=label_fragment)

        try:
            roller = device.find_widget(tag="edit_roller")
            assert roller is not None, f"{label_fragment} roller not found"
            assert roller.option_count == option_count
            assert roller.h >= 220
            assert device.find_widget(tag="edit_slider") is None
            assert not _has_text_containing(device, "Range:")
        finally:
            device.click(tag="edit_cancel")

    def test_ap40_glycol_loop_toggle(self, device: DeviceClient):
        """AP40 edits the sign as a Glycol loop switch and the slider as a magnitude."""
        url = f"{device.base_url}/api/heatpump/advanced/AP40"
        headers = {"Content-Type": "text/plain"}
        original = device.session.get(url, timeout=device.timeout).json()["value"]
        device.session.put(url, data="15", headers=headers, timeout=device.timeout).raise_for_status()
        try:
            _open_control(device)
            _wait_for_text(device, "Water")
            device.click(label="AP40")
            slider = device.find_widget(tag="edit_slider")
            assert slider is not None and (slider.min, slider.max) == (1, 45)
            switch = device.find_widget(tag="edit_sign_flag_switch")
            assert switch is not None and switch.checked is False
            assert _has_text_containing(device, "Glycol loop")
            save = device.find_widget(tag="edit_save")
            assert save is not None and save.disabled

            assert device.toggle("edit_sign_flag_switch")["checked"] is True
            device.click(tag="edit_save")
            device.wait_until(
                "AP40 written as glycol",
                lambda: device.session.get(url, timeout=device.timeout).json()["value"] == -15,
                timeout=10.0,
            )
            _wait_for_text(device, "Glycol")
        finally:
            if device.find_widget(tag="edit_cancel") is not None:
                device.click(tag="edit_cancel")
            device.session.put(url, data=str(original), headers=headers, timeout=device.timeout)

    def test_ap40_use_default_clears_glycol(self, device: DeviceClient):
        """Use default restores Water + 15 °C in the editor; Cancel discards it."""
        url = f"{device.base_url}/api/heatpump/advanced/AP40"
        headers = {"Content-Type": "text/plain"}
        original = device.session.get(url, timeout=device.timeout).json()["value"]
        device.session.put(url, data="-1", headers=headers, timeout=device.timeout).raise_for_status()
        try:
            _open_control(device)
            _wait_for_text(device, "Glycol")
            device.click(label="AP40")
            switch = device.find_widget(tag="edit_sign_flag_switch")
            slider = device.find_widget(tag="edit_slider")
            assert switch is not None and switch.checked is True
            assert slider is not None and slider.value == 1

            device.click(tag="edit_use_default")
            device.wait_until(
                "editor shows the default",
                lambda: (device.find_widget(tag="edit_sign_flag_switch").checked is False
                         and device.find_widget(tag="edit_slider").value == 15),
                timeout=5.0,
            )
            assert device.find_widget(tag="edit_value").text == "15 °C"
            assert device.find_widget(tag="edit_use_default").disabled
            assert not device.find_widget(tag="edit_save").disabled

            device.click(tag="edit_cancel")
            assert device.session.get(url, timeout=device.timeout).json()["value"] == -1
        finally:
            if device.find_widget(tag="edit_cancel") is not None:
                device.click(tag="edit_cancel")
            device.session.put(url, data=str(original), headers=headers, timeout=device.timeout)


# =========================================================================
# Advanced Parameters Section
# =========================================================================

class TestAdvancedParameters:
    """Verify Advanced Parameter (AP##) rows are visible."""

    def test_setpoints_section_header(self, device: DeviceClient):
        """The Setpoints section header should be present."""
        _open_control(device)
        _wait_for_text(device, "Setpoints")

        assert _has_text_containing(device, "Setpoints"), \
            "Setpoints section header not found"

    def test_advanced_parameter_visible(self, device: DeviceClient):
        """At least one Advanced Parameter row should be visible (e.g. AP13)."""
        _open_control(device)
        # Rows lead with an "AP##" code pill; the name no longer carries it.
        device.wait_until(
            "an Advanced Parameter (AP##) row is visible",
            lambda: any(
                re.fullmatch(r"AP\d+", (w.text or "").strip()) for w in device.widgets
            ),
            timeout=5.0,
        )
        assert not any("(AP" in (w.text_en or w.text or "") for w in device.widgets), \
            "AP names should not repeat the code in parentheses"
