"""
Test: Language Switching

Verifies that switching between English, French, and Spanish updates
the UI text on the language screen itself and on the settings menu.
Confirms the active language via the preferences API.
"""

import pytest
from device_client import DeviceClient


# ── Translation tables for verification ──────────────────────────────────────

# Language button tags indexed by language name
LANG_TAGS = {
    "English":  "lang_english",
    "Français": "lang_french",
    "Español":  "lang_spanish",
}

# Expected translations for settings menu labels per language
# These are the text strings that should appear on-screen
SETTINGS_LABELS = {
    "English":  ["WiFi", "Update", "Language", "Time & Location", "Display"],
    "Français": ["WiFi", "Mise à jour", "Langue", "Heure et lieu", "Affichage"],
    "Español":  ["WiFi", "Actualizar", "Idioma", "Hora y ubicación", "Pantalla"],
}


# ── Helpers ──────────────────────────────────────────────────────────────────

def _navigate_to_language_screen(device: DeviceClient):
    """Open settings → language sub-screen."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    assert device.wait_for_widget(tag="settings_language", timeout=5.0)

    device.click(tag="settings_language")
    assert device.wait_for_screen("language", timeout=5.0)
    assert device.wait_for_widget(tag="lang_english", timeout=5.0)


def _select_language(device: DeviceClient, lang_name: str):
    """Click a language button on the language screen by its tag."""
    tag = LANG_TAGS[lang_name]
    device.click(tag=tag)
    device.wait_until(
        f"language preference is {lang_name}",
        lambda: device.get_preferences().get("language") == lang_name,
        timeout=5.0,
    )


# ── Tests ────────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("lang_name", ["Français", "Español", "English"])
def test_switch_language_and_verify_api(device: DeviceClient, lang_name: str):
    """Switching language updates the preference reported by the API."""
    _navigate_to_language_screen(device)
    _select_language(device, lang_name)

    # Verify via API
    prefs = device.get_preferences()
    assert prefs["language"] == lang_name, \
        f"Expected language='{lang_name}', got '{prefs['language']}'"


@pytest.mark.parametrize("lang_name", ["Français", "Español", "English"])
def test_switch_language_and_verify_settings_menu(device: DeviceClient, lang_name: str):
    """After switching language, the settings menu labels are translated."""
    _navigate_to_language_screen(device)
    _select_language(device, lang_name)

    # Go back to settings menu
    device.click(tag="language_back")
    assert device.wait_for_screen("settings", timeout=5.0)

    # Check that every expected translated label appears somewhere on screen
    expected_labels = SETTINGS_LABELS[lang_name]
    device.wait_until(
        f"settings menu translated to {lang_name}",
        lambda: all(device.has_widget(text=t) for t in expected_labels),
        timeout=5.0,
        raise_on_timeout=False,
    )
    for expected_text in expected_labels:
        assert device.has_widget(text=expected_text), \
            f"[{lang_name}] Expected label '{expected_text}' not found on settings screen"


def test_restore_english(device: DeviceClient):
    """Ensure English is restored at the end so other tests aren't affected."""
    prefs = device.get_preferences()
    if prefs["language"] != "English":
        _navigate_to_language_screen(device)
        _select_language(device, "English")

    prefs = device.get_preferences()
    assert prefs["language"] == "English", \
        f"Expected language='English', got '{prefs['language']}'"
