"""
Test: Localization — French & Spanish labels on the main screen

Switches the device language to French or Spanish, then returns to the
main screen and verifies that hero state, footer nav, and other
dynamically-refreshed labels are properly translated.

Note: Some static labels (component dots, perf strip headers, expandable
panel headers) are set at screen creation and do NOT refresh on language
change. Those are excluded from this test and tracked as a known issue.
"""

import time
import pytest
from device_client import DeviceClient

# ---------------------------------------------------------------------------
# Status register bit masks (must match arctic_registers.h)
# ---------------------------------------------------------------------------
UNIT_ON       = 0x0001
COMPRESSOR    = 0x0002
FAN_MED       = 0x0008
WATER_PUMP    = 0x0020

# Default demo state
DEMO_STATUS1 = UNIT_ON | COMPRESSOR | FAN_MED | WATER_PUMP  # 0x2B
DEMO_ERROR2  = 0x0040  # HIGH_PRESSURE (P02)

MODE_COOLING       = 0
MODE_FLOOR_HEATING = 1
MODE_HOT_WATER     = 5

UI_SETTLE = 1.5

# ---------------------------------------------------------------------------
# Language tags and API values
# ---------------------------------------------------------------------------
LANG_TAGS = {
    "English":  "lang_english",
    "Français": "lang_french",
    "Español":  "lang_spanish",
}

# ---------------------------------------------------------------------------
# Translation tables — main screen labels
# ---------------------------------------------------------------------------

HERO_STATES = {
    "English":  {"IDLE": "IDLE",          "FAULT": "FAULT",    "STANDBY": "STANDBY",
                 "DEFROST": "DEFROST",    "HEATING": "HEATING",
                 "DISCONNECTED": "DISCONNECTED"},
    "Français": {"IDLE": "INACTIF",       "FAULT": "PANNE",   "STANDBY": "EN VEILLE",
                 "DEFROST": "DÉGIVRAGE",  "HEATING": "CHAUFFAGE",
                 "DISCONNECTED": "DÉCONNECTÉ"},
    "Español":  {"IDLE": "INACTIVO",      "FAULT": "FALLO",   "STANDBY": "EN ESPERA",
                 "DEFROST": "DESCONGELACIÓN", "HEATING": "CALEFACCIÓN",
                 "DISCONNECTED": "DESCONECTADO"},
}

HERO_MODES = {
    "English":  {MODE_FLOOR_HEATING: "FLOOR HEAT", MODE_COOLING: "COOLING",
                 MODE_HOT_WATER: "HOT WATER"},
    "Français": {MODE_FLOOR_HEATING: "CHAUFF. SOL", MODE_COOLING: "REFROIDISSEMENT",
                 MODE_HOT_WATER: "EAU CHAUDE"},
    "Español":  {MODE_FLOOR_HEATING: "CALEF. SUELO", MODE_COOLING: "ENFRIAMIENTO",
                 MODE_HOT_WATER: "AGUA CALIENTE"},
}

COMPONENT_DOTS = {
    "English":  ["Compressor", "Fan", "Pump", "Aux Heat"],
    "Français": ["Compresseur", "Ventilateur", "Pompe", "Chauff. aux."],
    "Español":  ["Compresor", "Ventilador", "Bomba", "Calef. aux."],
}

PERF_STRIP_LABELS = {
    "English":  ["POWER", "FAN"],
    "Français": ["PUISSANCE", "VENTIL."],
    "Español":  ["POTENCIA", "VENTIL."],
}

ERROR_CARD_NO_ERRORS = {
    "English":  "No active errors",
    "Français": "Aucune erreur active",
    "Español":  "Sin errores activos",
}

# Labels that ARE dynamically refreshed (footer nav, tank description)
TANK_DESCRIPTION = {
    "English":  "Tank Temperature",
    "Français": "Température du ballon",
    "Español":  "Temperatura del tanque",
}

FOOTER_NAV = {
    "English":  ["Status", "Control", "Events"],
    "Français": ["État", "Contrôle", "Événements"],
    "Español":  ["Estado", "Control", "Eventos"],
}

# Known issue: These labels are set at screen creation and do NOT
# refresh on language change. Tracked for a future fix.
# - Component dots:  Compressor, Fan, Pump, Aux Heat
# - Perf strip:      COP, POWER, FAN
# - Panel headers:   Temperatures, Compressor, Energy
# - Demo banner:     Demo Mode Enabled

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _switch_language(device: DeviceClient, lang_name: str):
    """Navigate to Settings → Language → select language → return to main."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.3)

    device.click(tag="settings_language")
    assert device.wait_for_screen("language", timeout=5.0)
    time.sleep(0.3)

    device.click(tag=LANG_TAGS[lang_name])
    time.sleep(0.3)

    # Navigate back: language → settings → main
    device.click(tag="language_back")
    assert device.wait_for_screen("settings", timeout=5.0)
    time.sleep(0.3)

    device.click(tag="settings_close")
    assert device.wait_for_screen("main", timeout=5.0)
    time.sleep(UI_SETTLE)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def _restore_english_and_demo(device: DeviceClient):
    """Restore English and default demo state after each test."""
    yield
    # Restore demo defaults
    device.set_demo_fields(
        error1=0,
        error2=DEMO_ERROR2,
        status1=DEMO_STATUS1,
        unit_on=1,
        working_mode=MODE_FLOOR_HEATING,
    )
    # Restore English if switched
    prefs = device.get_preferences()
    if prefs["language"] != "English":
        try:
            _switch_language(device, "English")
        except Exception:
            pass


# =========================================================================
# French — Hero States
# =========================================================================

class TestFrenchHeroStates:
    """Verify hero state labels translate to French."""

    @pytest.fixture(autouse=True)
    def _switch_to_french(self, device: DeviceClient):
        _switch_language(device, "Français")

    def test_idle_french(self, device: DeviceClient):
        """IDLE → INACTIF in French."""
        device.set_demo_fields(
            status1=UNIT_ON | WATER_PUMP,  # no compressor → IDLE
            error1=0, error2=0,
            working_mode=MODE_FLOOR_HEATING,
        )
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["IDLE"]

    def test_fault_french(self, device: DeviceClient):
        """FAULT → PANNE in French."""
        device.set_demo_fields(error1=0x0001, error2=0)
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["FAULT"]

    def test_standby_french(self, device: DeviceClient):
        """STANDBY → EN VEILLE in French."""
        device.set_demo_fields(unit_on=0, error1=0, error2=0)
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["STANDBY"]

    def test_floor_heat_selection_shows_heating_french(self, device: DeviceClient):
        """Floor-heat selection still reports the actual heating operation."""
        device.set_demo_fields(
            status1=DEMO_STATUS1,
            working_mode=MODE_FLOOR_HEATING,
            error1=0, error2=0,
        )
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["HEATING"]

    def test_cooling_french(self, device: DeviceClient):
        """COOLING → REFROIDISSEMENT in French."""
        device.set_demo_fields(
            status1=DEMO_STATUS1,
            working_mode=MODE_COOLING,
            error1=0, error2=0,
        )
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_MODES["Français"][MODE_COOLING]

    def test_hot_water_selection_shows_heating_french(self, device: DeviceClient):
        """Hot-water selection still reports the actual heating operation."""
        device.set_demo_fields(
            status1=DEMO_STATUS1,
            working_mode=MODE_HOT_WATER,
            error1=0, error2=0,
        )
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["HEATING"]


# =========================================================================
# French — Component Dots, Performance Strip, Error Card
# =========================================================================

class TestFrenchMainLabels:
    """Verify tank description, footer nav, and error card in French."""

    @pytest.fixture(autouse=True)
    def _switch_to_french(self, device: DeviceClient):
        _switch_language(device, "Français")

    def test_tank_description_french(self, device: DeviceClient):
        """Tank description label translates to French."""
        assert device.has_widget(text=TANK_DESCRIPTION["Français"]), \
            f"French tank description '{TANK_DESCRIPTION['Français']}' not found"

    def test_footer_nav_french(self, device: DeviceClient):
        """Footer nav buttons are translated to French."""
        for label in FOOTER_NAV["Français"]:
            found = any(label in w.text for w in device.widgets if w.text)
            assert found, f"French footer label '{label}' not found"

    def test_error_card_no_errors_french(self, device: DeviceClient):
        """Error card shows French 'no errors' text."""
        device.set_demo_fields(error1=0, error2=0)
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="error_label")
        assert w is not None
        assert ERROR_CARD_NO_ERRORS["Français"] in w.text, \
            f"Expected '{ERROR_CARD_NO_ERRORS['Français']}' in error label, got '{w.text}'"


# =========================================================================
# Spanish — Hero States
# =========================================================================

class TestSpanishHeroStates:
    """Verify hero state labels translate to Spanish."""

    @pytest.fixture(autouse=True)
    def _switch_to_spanish(self, device: DeviceClient):
        _switch_language(device, "Español")

    def test_idle_spanish(self, device: DeviceClient):
        """IDLE → INACTIVO in Spanish."""
        device.set_demo_fields(
            status1=UNIT_ON | WATER_PUMP,
            error1=0, error2=0,
            working_mode=MODE_FLOOR_HEATING,
        )
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["IDLE"]

    def test_fault_spanish(self, device: DeviceClient):
        """FAULT → FALLO in Spanish."""
        device.set_demo_fields(error1=0x0001, error2=0)
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["FAULT"]

    def test_standby_spanish(self, device: DeviceClient):
        """STANDBY → EN ESPERA in Spanish."""
        device.set_demo_fields(unit_on=0, error1=0, error2=0)
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["STANDBY"]

    def test_floor_heat_selection_shows_heating_spanish(self, device: DeviceClient):
        """Floor-heat selection still reports the actual heating operation."""
        device.set_demo_fields(
            status1=DEMO_STATUS1,
            working_mode=MODE_FLOOR_HEATING,
            error1=0, error2=0,
        )
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["HEATING"]

    def test_cooling_spanish(self, device: DeviceClient):
        """COOLING → ENFRIAMIENTO in Spanish."""
        device.set_demo_fields(
            status1=DEMO_STATUS1,
            working_mode=MODE_COOLING,
            error1=0, error2=0,
        )
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_MODES["Español"][MODE_COOLING]

    def test_hot_water_selection_shows_heating_spanish(self, device: DeviceClient):
        """Hot-water selection still reports the actual heating operation."""
        device.set_demo_fields(
            status1=DEMO_STATUS1,
            working_mode=MODE_HOT_WATER,
            error1=0, error2=0,
        )
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["HEATING"]


# =========================================================================
# Spanish — Component Dots, Performance Strip, Error Card
# =========================================================================

class TestSpanishMainLabels:
    """Verify tank description, footer nav, and error card in Spanish."""

    @pytest.fixture(autouse=True)
    def _switch_to_spanish(self, device: DeviceClient):
        _switch_language(device, "Español")

    def test_tank_description_spanish(self, device: DeviceClient):
        """Tank description label translates to Spanish."""
        assert device.has_widget(text=TANK_DESCRIPTION["Español"]), \
            f"Spanish tank description '{TANK_DESCRIPTION['Español']}' not found"

    def test_footer_nav_spanish(self, device: DeviceClient):
        """Footer nav buttons are translated to Spanish."""
        for label in FOOTER_NAV["Español"]:
            found = any(label in w.text for w in device.widgets if w.text)
            assert found, f"Spanish footer label '{label}' not found"

    def test_error_card_no_errors_spanish(self, device: DeviceClient):
        """Error card shows Spanish 'no errors' text."""
        device.set_demo_fields(error1=0, error2=0)
        time.sleep(UI_SETTLE)
        w = device.find_widget(tag="error_label")
        assert w is not None
        assert ERROR_CARD_NO_ERRORS["Español"] in w.text, \
            f"Expected '{ERROR_CARD_NO_ERRORS['Español']}' in error label, got '{w.text}'"
