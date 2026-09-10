"""
Test: Settings Sub-Screen Navigation

Verifies that each settings menu row navigates to its sub-screen
and the back button returns to the settings menu.

The inventory itself lives in screens.py, shared with conftest's
_return_to_main. test_settings_rows_match_the_registry keeps that registry
honest against what the firmware actually renders.
"""

import re

import pytest
from device_client import DeviceClient
from screens import NON_SUB_SCREEN_ROWS, SUB_SCREENS


SETTINGS_ROW_RE = re.compile(r"^settings_[a-z0-9_]+$")


@pytest.mark.parametrize("screen", SUB_SCREENS, ids=[s.name for s in SUB_SCREENS])
def test_open_sub_screen(device: DeviceClient, screen):
    """Clicking a settings row should open the corresponding sub-screen."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0), \
        f"Settings did not open — on '{device.screen}'"
    assert device.wait_for_widget(tag=screen.row_tag, timeout=5.0)

    device.click(tag=screen.row_tag)
    assert device.wait_for_screen(screen.name, timeout=5.0), \
        f"Expected '{screen.name}' screen, got '{device.screen}'"


@pytest.mark.parametrize("screen", SUB_SCREENS, ids=[s.name for s in SUB_SCREENS])
def test_back_from_sub_screen(device: DeviceClient, screen):
    """Pressing back from a sub-screen should return to the settings menu."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    assert device.wait_for_widget(tag=screen.row_tag, timeout=5.0)

    device.click(tag=screen.row_tag)
    assert device.wait_for_screen(screen.name, timeout=5.0)
    assert device.wait_for_widget(tag=screen.back_tag, timeout=5.0)

    device.click(tag=screen.back_tag)
    assert device.wait_for_screen("settings", timeout=5.0), \
        f"Did not return to settings — on '{device.screen}'"


def test_settings_rows_match_the_registry(device: DeviceClient):
    """Every settings row the firmware renders must be in the registry.

    This is the tripwire that stops screens.py drifting from the firmware. A
    newly added settings row fails here, loudly and by name, instead of
    silently stranding _return_to_main on a screen it does not know how to
    leave -- which is how #158 turned into hours of debugging an unrelated
    test.
    """
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0), \
        f"Settings did not open — on '{device.screen}'"

    rendered = {
        w.tag
        for w in device.widgets
        if w.tag and SETTINGS_ROW_RE.match(w.tag) and w.tag not in NON_SUB_SCREEN_ROWS
    }
    assert rendered, (
        "No settings_* rows were found on the settings screen. Either the "
        "screen failed to render or the row tags were renamed."
    )

    registered = {s.row_tag for s in SUB_SCREENS}
    unregistered = sorted(rendered - registered)
    missing = sorted(registered - rendered)

    assert not unregistered, (
        f"Settings row(s) {unregistered} are not in tests/device/screens.py. "
        "Add them to SUB_SCREENS (with a cleanup hook if the screen is backed "
        "by a mock), or _return_to_main will strand the harness there."
    )
    assert not missing, (
        f"Registry lists row(s) {missing} that the firmware no longer renders. "
        "Remove them from tests/device/screens.py."
    )
