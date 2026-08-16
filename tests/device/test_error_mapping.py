"""
Test: Error Code Mapping — verify every fault code shows correctly in the UI.

Injects each fault individually (by its canonical Macon code, via the
arctic-macon library's code->register,bit mapping) and checks:
  1. The main-screen error card shows the right error code.
  2. Opening the error panel shows the code with its description.

Faults are injected atomically through /api/test/inject-fault so the test
never hardcodes register bit positions — the library owns that mapping.
Tests run with all other faults cleared so each code is isolated.
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
# Canonical fault table — mirrors arctic-macon MACON_FAULT_BITS.
# (code, partial_label) where partial_label is a substring of the library
# label shown in the error panel. The INFO-severity RUN indicator is excluded
# because it is not a fault and is not decoded as one.
# ---------------------------------------------------------------------------
FAULT_DEFS = [
    ("P15", "Temp difference too large"),
    ("P16", "Outlet temp too low"),
    ("FE",  "FE protection"),
    ("FF",  "FF protection"),
    ("E28", "EEPROM"),
    ("E19", "Inlet water temp sensor"),
    ("E18", "Outlet water temp sensor"),
    ("E13", "Cool coil temp sensor"),
    ("E03", "E03 protection"),
    ("E27", "Driver communication"),
    ("E21", "Controller communication"),
    ("r02", "Compressor start failure"),
    ("E26", "Indoor/outdoor communication"),
    ("r01", "IPM fault"),
    ("E01", "Discharge temp sensor"),
    ("E09", "Suction temp sensor"),
    ("E05", "Coil temp sensor"),
    ("E22", "Ambient temp sensor"),
    ("P19", "AC current protection"),
    ("r06", "Compressor phase current"),
    ("r10", "AC voltage protection"),
    ("r11", "DC bus voltage protection"),
    ("r05", "IPM temperature protection"),
    ("P11", "High discharge temp"),
    ("P02", "High pressure protection"),
    ("P06", "Low pressure protection"),
    ("P27", "Coil overheat"),
    ("PC",  "Ambient too high/low"),
    ("P10", "P10 protection"),
    ("P30", "Antifreeze protection"),
    ("P01", "Water flow protection"),
]

# Default demo fault restored after each test (matches initDemoState()).
DEMO_DEFAULT_FAULT = "P02"


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def _restore_demo_faults(device: DeviceClient):
    """Restore the default demo fault state after each test."""
    yield
    device.clear_all_faults()
    device.inject_fault(DEMO_DEFAULT_FAULT, True)
    device.set_demo_fields(unit_on=1)
    _wait()


# =========================================================================
# Per-fault tests on the main screen error card
# =========================================================================

class TestFaultCardCodes:
    """Verify each fault code shows on the main-screen error card."""

    @pytest.mark.parametrize(
        "code, label",
        FAULT_DEFS,
        ids=[d[0] for d in FAULT_DEFS],
    )
    def test_fault_code_on_card(self, device: DeviceClient, code, label):
        device.clear_all_faults()
        device.inject_fault(code, True)
        _wait()
        widget = device.find_widget(tag="error_label")
        assert widget is not None, "error_label widget not found"
        assert code in widget.text, \
            f"Expected '{code}' in error card, got '{widget.text}'"


# =========================================================================
# Error panel — batch tests (all faults injected at once)
# =========================================================================

class TestFaultPanel:
    """Inject all faults and verify every code + description in the panel."""

    def test_all_codes_in_panel(self, device: DeviceClient):
        """Inject every fault and open the panel — every code should appear."""
        device.clear_all_faults()
        for code, _label in FAULT_DEFS:
            device.inject_fault(code, True)
        _wait()

        # Click the error card to open the errors panel
        device.click(tag="error_label")
        time.sleep(1.0)  # panel animation

        all_text = " ".join(w.text or "" for w in device.widgets)

        for code, _label in FAULT_DEFS:
            assert code in all_text, \
                f"Fault code '{code}' not found in error panel"

        # Close panel
        device.click(symbol="CLOSE")
        time.sleep(0.5)

    def test_all_descriptions_in_panel(self, device: DeviceClient):
        """Each fault description fragment should appear in the panel."""
        device.clear_all_faults()
        for code, _label in FAULT_DEFS:
            device.inject_fault(code, True)
        _wait()

        device.click(tag="error_label")
        time.sleep(1.0)

        all_text = " ".join(w.text or "" for w in device.widgets)

        for code, label in FAULT_DEFS:
            assert label in all_text, \
                f"Description '{label}' for {code} not found in panel"

        device.click(symbol="CLOSE")
        time.sleep(0.5)
