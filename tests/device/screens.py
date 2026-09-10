"""Single source of truth for the settings sub-screen inventory.

Before this module the inventory was maintained in two hand-written copies:
``SUB_SCREENS`` in ``test_navigation.py`` and a bare ``if current in (...)``
tuple inside ``_return_to_main`` in ``conftest.py``. Adding a screen to one but
not the other raised no error -- ``_return_to_main`` simply fell through every
branch and stranded the device on that screen, so the *next* test failed on its
first ``click(tag="settings")`` with a 404. The failure surfaced on an
unrelated screen, which is why #158 cost hours to diagnose.

Both consumers now read this registry. The registry itself is kept honest by
``test_navigation.py::test_settings_rows_match_the_registry``, which reads the
rows the firmware actually renders and fails if they do not match -- so the
firmware stays the source of truth rather than this file becoming a third
hand-maintained list.

Not included: the tab-shell panels (``status``, ``control``, ``event_log``,
returned to via ``nav_home``) and the standalone ``errors`` overlay. They have
a different navigation contract and are not duplicated anywhere.
"""

from dataclasses import dataclass
from typing import Optional, Tuple


@dataclass(frozen=True)
class SubScreen:
    """One settings sub-screen and how the harness gets into and out of it."""

    name: str
    #: Settings-menu row that opens the screen.
    row_tag: str
    #: Back button within the screen. Defaults to ``<name>_back``.
    back_tag: str
    #: Name of a DeviceClient method to call before leaving the screen, for
    #: screens backed by a mock whose state would otherwise leak into the next
    #: test. ``None`` means nothing to undo.
    cleanup: Optional[str] = None


def _screen(name: str, cleanup: Optional[str] = None) -> SubScreen:
    return SubScreen(
        name=name,
        row_tag=f"settings_{name}",
        back_tag=f"{name}_back",
        cleanup=cleanup,
    )


SUB_SCREENS: Tuple[SubScreen, ...] = (
    _screen("wifi", cleanup="wifi_mock_reset"),
    _screen("firmware", cleanup="firmware_mock_reset"),
    _screen("time", cleanup="geocoding_mock_reset"),
    _screen("language"),
    _screen("display"),
    _screen("home_assistant"),
    _screen("security"),
    _screen("web"),
)

SUB_SCREENS_BY_NAME = {s.name: s for s in SUB_SCREENS}

#: Settings-menu rows that carry a ``settings_*`` tag but do not open a
#: sub-screen, so they are not part of the navigate-in/back-out contract above.
#:
#: These are listed explicitly rather than filtered by a naming convention
#: because the point of the registry test is that a NEW row fails loudly. A
#: pattern like "ignore anything ending in _mode" would silently swallow the
#: next screen someone adds, which is the exact failure this module exists to
#: prevent. Adding a row here should be a deliberate statement that it has no
#: sub-screen -- checked against the firmware, not assumed from the name.
NON_SUB_SCREEN_ROWS = frozenset(
    {
        # Closes the settings menu.
        "settings_close",
        # Toggle rows: the row holds a switch (``demo_mode_switch`` /
        # ``temp_unit_switch``) and clicking it navigates nowhere. See
        # create_toggle_row and the temperature-units block in
        # main/settings/settings_menu.cpp.
        "settings_demo_mode",
        "settings_temp_unit",
        # Opens a modal confirmation overlay, not a screen:
        # show_factory_reset_confirmation() in settings_menu.cpp leaves
        # state.sub_screen_active false and never sets active_sub_screen, so
        # there is no screen for _return_to_main to back out of. The overlay's
        # own dismiss path is exercised by the factory-reset tests.
        "settings_factory_reset",
    }
)
