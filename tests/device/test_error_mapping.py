"""
Test: Error Code Mapping — verify every error bit shows the correct code in the UI.

Sets each error bit individually and checks:
  1. The main-screen error card shows the right error code.
  2. Opening the error panel shows the code with its description.

Tests run with all other errors cleared so each code is isolated.
"""

import time
import pytest
from device_client import DeviceClient

# ---------------------------------------------------------------------------
# Timing
# ---------------------------------------------------------------------------
UI_SETTLE = 1.5  # seconds — wait for the 1-second main screen timer


def _wait():
    time.sleep(UI_SETTLE)


# ---------------------------------------------------------------------------
# Error definition table — must match heatpump_errors.cpp
# (register, bit_mask, expected_code, partial_description)
# ---------------------------------------------------------------------------
ERROR1_DEFS = [
    (0x0001, "E27", "Indoor unit EEPROM"),
    (0x0002, "E28", "Outdoor unit EEPROM"),
    (0x0004, "E19", "Inlet water temperature sensor"),
    (0x0008, "E18", "Outlet water temperature sensor"),
    (0x0010, "E13", "Cooling coil temperature sensor"),
    (0x0020, "E05", "Heat pump coil temperature sensor"),
    (0x0040, "E01", "Compressor discharge temperature sensor"),
    (0x0080, "E09", "Compressor suction temperature sensor"),
    (0x0100, "E22", "Outdoor ambient temperature sensor"),
    (0x0200, "E10", "Communication error between drive board"),
    (0x0400, "E21", "Wired controller communication"),
    (0x0800, "r02", "Compressor start fault"),
    (0x1000, "E12", "Communication error between indoor"),
    (0x2000, "r01", "IPM module fault"),
    (0x4000, "PA",  "Tank temperature protection"),
    (0x8000, "r10", "AC voltage too high or too low"),
]

ERROR2_DEFS = [
    (0x0001, "P19", "AC current protection"),
    (0x0002, "r06", "Compressor phase current"),
    (0x0004, "FA",  "DC fan motor protection"),
    (0x0008, "r11", "DC bus voltage protection"),
    (0x0010, "r05", "IPM module temperature too high"),
    (0x0020, "P11", "Compressor discharge temperature too high"),
    (0x0040, "P02", "High pressure protection"),
    (0x0080, "P06", "Low pressure protection"),
    (0x0100, "P01", "Water flow switch protection"),
    (0x0200, "P27", "Cooling coil temperature overheating"),
    (0x0400, "E26", "Low ambient temperature"),
    (0x0800, "EC",  "EEV circuit low pressure"),
    (0x1000, "ED",  "Low pressure protection (pressure sensor)"),
    (0x2000, "P15", "Inlet/outlet temperature difference"),
    (0x4000, "P16", "Outlet water temperature too low"),
    (0x8000, "r20", "Compressor protection"),
]

# Default demo state for restoration
DEMO_STATUS1 = 0x002B  # UNIT_ON | COMPRESSOR | FAN_MED | WATER_PUMP
DEMO_ERROR2 = 0x0040   # HIGH_PRESSURE (default demo error)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def _restore_demo_errors(device: DeviceClient):
    """Restore default demo error state after each test."""
    yield
    device.set_demo_fields(
        error1=0,
        error2=DEMO_ERROR2,
        status1=DEMO_STATUS1,
        unit_on=1,
    )
    _wait()


# =========================================================================
# Per-error tests on the main screen error card
# =========================================================================

class TestError1CardCodes:
    """Verify each error1 bit shows the correct code on the main-screen card."""

    @pytest.mark.parametrize(
        "mask, code, desc",
        ERROR1_DEFS,
        ids=[e[1] for e in ERROR1_DEFS],
    )
    def test_error1_code_on_card(self, device: DeviceClient, mask, code, desc):
        device.set_demo_fields(error1=mask, error2=0)
        _wait()
        label = device.find_widget(tag="error_label")
        assert label is not None, "error_label widget not found"
        assert code in label.text, \
            f"Expected '{code}' in error card, got '{label.text}'"


class TestError2CardCodes:
    """Verify each error2 bit shows the correct code on the main-screen card."""

    @pytest.mark.parametrize(
        "mask, code, desc",
        ERROR2_DEFS,
        ids=[e[1] for e in ERROR2_DEFS],
    )
    def test_error2_code_on_card(self, device: DeviceClient, mask, code, desc):
        device.set_demo_fields(error1=0, error2=mask)
        _wait()
        label = device.find_widget(tag="error_label")
        assert label is not None, "error_label widget not found"
        assert code in label.text, \
            f"Expected '{code}' in error card, got '{label.text}'"


# =========================================================================
# Error panel — batch tests (all bits set at once)
# =========================================================================

class TestError1Panel:
    """Set all error1 bits and verify every code + description in the panel."""

    def test_all_error1_codes_in_panel(self, device: DeviceClient):
        """Set all error1 bits and open the panel — every code should appear."""
        device.set_demo_fields(error1=0xFFFF, error2=0)
        _wait()

        # Click the error card to open the errors panel
        device.click(tag="error_label")
        time.sleep(1.0)  # panel animation

        all_text = " ".join(w.text or "" for w in device.widgets)

        for mask, code, desc_fragment in ERROR1_DEFS:
            assert code in all_text, \
                f"Error code '{code}' (mask 0x{mask:04x}) not found in error panel"

        # Close panel
        device.click(symbol="CLOSE")
        time.sleep(0.5)


class TestError2Panel:
    """Set all error2 bits and verify every code + description in the panel."""

    def test_all_error2_codes_in_panel(self, device: DeviceClient):
        """Set all error2 bits and open the panel — every code should appear."""
        device.set_demo_fields(error1=0, error2=0xFFFF)
        _wait()

        # Click the error card to open the errors panel
        device.click(tag="error_label")
        time.sleep(1.0)  # panel animation

        all_text = " ".join(w.text or "" for w in device.widgets)

        for mask, code, desc_fragment in ERROR2_DEFS:
            assert code in all_text, \
                f"Error code '{code}' (mask 0x{mask:04x}) not found in error panel"

        # Close panel
        device.click(symbol="CLOSE")
        time.sleep(0.5)


class TestErrorPanelDescriptions:
    """Verify error descriptions appear alongside codes in the panel."""

    def test_error1_descriptions_in_panel(self, device: DeviceClient):
        """Each error1 description fragment should appear in the panel."""
        device.set_demo_fields(error1=0xFFFF, error2=0)
        _wait()

        device.click(tag="error_label")
        time.sleep(1.0)

        all_text = " ".join(w.text or "" for w in device.widgets)

        for mask, code, desc_fragment in ERROR1_DEFS:
            assert desc_fragment in all_text, \
                f"Description '{desc_fragment}' for {code} not found in panel"

        device.click(symbol="CLOSE")
        time.sleep(0.5)

    def test_error2_descriptions_in_panel(self, device: DeviceClient):
        """Each error2 description fragment should appear in the panel."""
        device.set_demo_fields(error1=0, error2=0xFFFF)
        _wait()

        device.click(tag="error_label")
        time.sleep(1.0)

        all_text = " ".join(w.text or "" for w in device.widgets)

        for mask, code, desc_fragment in ERROR2_DEFS:
            assert desc_fragment in all_text, \
                f"Description '{desc_fragment}' for {code} not found in panel"

        device.click(symbol="CLOSE")
        time.sleep(0.5)
