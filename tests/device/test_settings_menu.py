"""
Test: Settings Menu Navigation

Verifies that pressing the settings button opens the settings menu
and that the settings screen contains the expected UI elements.
"""

import os

import pytest
import requests

from device_client import DeviceClient


def test_open_settings_menu(device: DeviceClient):
    """Clicking the settings button should open the settings screen."""
    # Precondition: we're on the main screen
    assert device.screen == "main", "Expected to start on main screen"

    # Act: click the settings button by tag
    device.click(tag="settings")

    # Assert: settings screen should now be visible
    assert device.wait_for_screen("settings", timeout=3.0), \
        f"Settings screen did not open — still on '{device.screen}'"


def test_settings_menu_has_close_button(device: DeviceClient):
    """The settings screen should have a close (X) button."""
    assert device.wait_for_widget(tag="settings", timeout=5.0)

    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0), \
        f"Settings screen did not open — still on '{device.screen}'"

    # Verify the screen has widgets
    widgets = device.widgets
    assert len(widgets) > 0, "Settings screen has no visible widgets"


def test_close_settings_menu(device: DeviceClient):
    """Clicking the close button should return to the main screen."""
    # Open settings
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=3.0), \
        "Settings screen did not open"

    # Close settings via the close button tag
    device.click(tag="settings_close")

    # Should be back on main
    assert device.wait_for_screen("main", timeout=3.0), \
        f"Did not return to main screen — still on '{device.screen}'"


def test_factory_reset_requires_confirmation_and_can_be_cancelled(
    device: DeviceClient,
):
    """Factory reset is visibly destructive and never runs on the first tap."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=3.0)

    device.click(tag="settings_factory_reset")
    assert device.wait_for_widget(tag="factory_reset_overlay", timeout=5.0)

    assert device.has_widget(tag="factory_reset_overlay")
    assert device.has_widget(tag="factory_reset_panel")
    assert device.has_widget(tag="factory_reset_confirm")
    assert device.has_widget(tag="factory_reset_cancel")

    device.click(tag="factory_reset_cancel")
    device.wait_until(
        "factory reset overlay dismissed",
        lambda: not device.has_widget(tag="factory_reset_overlay"),
        timeout=5.0,
    )
    assert not device.has_widget(tag="factory_reset_overlay")
    assert device.screen == "settings"


# =========================================================================
# Web password reset
#
# Recovery path for a lost web password. Physical presence at the
# touchscreen is the authorization, exactly as it is for the factory reset
# above -- there is deliberately no HTTP endpoint for this.
# =========================================================================

FACTORY_USERNAME = "arctic"
FACTORY_PASSWORD = "arctic"


def _login_succeeds(base_url: str, username: str, password: str) -> bool:
    """Attempt a fresh web login, isolated from the client's own session."""
    session = requests.Session()
    session.verify = False
    try:
        r = session.post(
            f"{base_url}/login",
            json={"username": username, "password": password},
            timeout=10,
        )
        return r.status_code == 200
    except requests.RequestException:
        return False
    finally:
        session.close()


def _open_password_reset_dialog(device: DeviceClient):
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=3.0)

    device.click(tag="settings_password_reset")
    assert device.wait_for_widget(tag="password_reset_overlay", timeout=5.0)


def test_password_reset_requires_confirmation_and_can_be_cancelled(
    device: DeviceClient,
):
    """The reset is destructive, so it never runs on the first tap."""
    _open_password_reset_dialog(device)

    assert device.has_widget(tag="password_reset_panel")
    assert device.has_widget(tag="password_reset_confirm")
    assert device.has_widget(tag="password_reset_cancel")

    device.click(tag="password_reset_cancel")
    device.wait_until(
        "password reset overlay dismissed",
        lambda: not device.has_widget(tag="password_reset_overlay"),
        timeout=5.0,
    )
    assert not device.has_widget(tag="password_reset_overlay")
    assert device.screen == "settings"


@pytest.fixture
def restore_test_credentials(device: DeviceClient):
    """Put the suite's credentials back after a destructive auth test.

    Skips rather than running unrestorable damage: without a known password
    to restore, confirming the reset would leave the controller on the
    factory sign-in for every later suite.
    """
    username = os.environ.get("ARCTIC_USERNAME", FACTORY_USERNAME)
    password = os.environ.get("ARCTIC_PASSWORD", "")
    if not password or password == FACTORY_PASSWORD:
        pytest.skip(
            "ARCTIC_PASSWORD is not provisioned, so the web password could "
            "not be restored after the reset"
        )

    # Prove the restore path works *before* breaking anything. Writing the
    # credentials the controller already has is a no-op, but a non-200 here
    # means the teardown below would have failed too -- and by then the
    # controller would already be on the factory sign-in.
    preflight = device.session.post(
        f"{device.base_url}/api/test/credentials",
        json={"username": username, "password": password},
        timeout=device.timeout,
    )
    if preflight.status_code != 200:
        pytest.skip(
            f"/api/test/credentials is not usable (HTTP "
            f"{preflight.status_code}), so the reset could not be undone"
        )

    yield

    r = device.session.post(
        f"{device.base_url}/api/test/credentials",
        json={"username": username, "password": password},
        timeout=device.timeout,
    )
    assert r.status_code == 200, (
        f"Could not restore test credentials (HTTP {r.status_code}); the "
        f"controller may be left on the factory sign-in"
    )


def test_password_reset_restores_the_factory_signin(
    device: DeviceClient, restore_test_credentials
):
    """Confirming the reset really does restore arctic/arctic.

    This is the whole point of the feature: a controller whose web password
    has been lost is recoverable from the touchscreen, without the factory
    reset that would also erase WiFi, Home Assistant pairing, certificates
    and history.
    """
    assert not _login_succeeds(device.base_url, FACTORY_USERNAME, FACTORY_PASSWORD), \
        "Factory sign-in already works; the test cannot prove the reset did it"

    _open_password_reset_dialog(device)
    device.click(tag="password_reset_confirm")

    device.wait_until(
        "factory sign-in accepted after reset",
        lambda: _login_succeeds(
            device.base_url, FACTORY_USERNAME, FACTORY_PASSWORD),
        timeout=10.0,
    )

    device.click(tag="password_reset_cancel")
    device.wait_until(
        "password reset overlay dismissed",
        lambda: not device.has_widget(tag="password_reset_overlay"),
        timeout=5.0,
    )
