"""
Test: Localization — French & Spanish labels on the main screen

Switches the device language to French or Spanish, then returns to the
main screen and verifies that hero state, footer nav, and other
dynamically-refreshed labels are properly translated.

Note: Some static labels (component dots, perf strip headers, expandable
panel headers) are set at screen creation and do NOT refresh on language
change. Those are excluded from this test and tracked as a known issue.
"""

import pytest
from device_client import DeviceClient

# ---------------------------------------------------------------------------
# Component/fault state helpers.
#
# Run state is decoded natively by arctic-macon from the real Tuya registers —
# there is no fictional "status1" bitfield. Faults are injected by their Macon
# code so the library owns the code->register,bit mapping.
# ---------------------------------------------------------------------------
FAN_MED = 450  # fan RPM (reg2003 raw ×10) -> 2 bars

# Default demo fault (matches initDemoState()).
DEMO_FAULT = "P02"


def _set_running(device: DeviceClient, **overrides):
    """Compressor + fan + pump running, unit on. Clears faults unless overridden."""
    fields = dict(compressor_freq=60, fan_on=1, fan_speed=FAN_MED, pump_on=1,
                  unit_on=1, cooling_on=0)
    fields.update(overrides)
    device.set_demo_fields(**fields)


def _set_idle(device: DeviceClient, **overrides):
    """Unit on, compressor off (idle)."""
    fields = dict(compressor_freq=0, fan_on=0, fan_speed=0, pump_on=1, unit_on=1)
    fields.update(overrides)
    device.set_demo_fields(**fields)


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

def _wait_widget_text(device: DeviceClient, tag: str, expected: str, *,
                      contains: bool = False, timeout: float = 5.0):
    """Wait for widget ``tag`` to display ``expected`` before asserting.

    Dynamically-refreshed labels (hero state, error card) update asynchronously
    after a demo-state or language change. Rather than sleep a fixed UI_SETTLE
    guess and hope the refresh landed, poll for the exact text. Best-effort
    (never raises): the caller's own asserts make the final authoritative check
    with a clear message, so a genuine mismatch still fails loudly.
    """
    def _ready() -> bool:
        w = device.find_widget(tag=tag)
        if w is None or w.text is None:
            return False
        return (expected in w.text) if contains else (w.text == expected)

    op = "contains" if contains else "=="
    device.wait_until(f"{tag} text {op} {expected!r}", _ready,
                      timeout=timeout, expect_within=UI_SETTLE,
                      raise_on_timeout=False)


def _switch_language(device: DeviceClient, lang_name: str):
    """Navigate to Settings → Language → select language → return to main."""
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)

    device.click(tag="settings_language")
    assert device.wait_for_screen("language", timeout=5.0)

    device.click(tag=LANG_TAGS[lang_name])
    # No screen transition here, so wait on the real observable: the API
    # preference reflecting the newly selected language, before we navigate
    # away and re-render the (now translated) screens.
    device.wait_until(
        f"language preference is {lang_name}",
        lambda: device.get_preferences().get("language") == lang_name,
        timeout=5.0,
    )

    # Navigate back: language → settings → main
    device.click(tag="language_back")
    assert device.wait_for_screen("settings", timeout=5.0)

    device.click(tag="settings_close")
    assert device.wait_for_screen("main", timeout=5.0)

    # wait_for_screen only gates on the screen being *settled* — the main
    # screen's translated labels (tank description, footer nav) are refreshed
    # separately on the ~1s state timer, so they can lag the settle. Gate on
    # the tank description actually showing the target language before
    # returning, so callers never race the re-render. Best-effort (never
    # raises): each test's own assert still makes the authoritative check with
    # a clear message if a label genuinely fails to translate.
    device.wait_until(
        f"main screen tank description in {lang_name}",
        lambda: device.has_widget(text=TANK_DESCRIPTION[lang_name]),
        timeout=5.0,
        raise_on_timeout=False,
    )


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def _restore_english_and_demo(device: DeviceClient):
    """Restore English and default demo state after each test."""
    yield
    # Restore demo defaults
    device.clear_all_faults()
    device.inject_fault(DEMO_FAULT, True)
    _set_running(device, working_mode=MODE_FLOOR_HEATING)
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
        device.clear_all_faults()
        _set_idle(device, working_mode=MODE_FLOOR_HEATING)
        _wait_widget_text(device, "hero_state", HERO_STATES["Français"]["IDLE"])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["IDLE"]

    def test_fault_french(self, device: DeviceClient):
        """FAULT → PANNE in French."""
        device.clear_all_faults()
        device.inject_fault("P02", True)
        _wait_widget_text(device, "hero_state", HERO_STATES["Français"]["FAULT"])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["FAULT"]

    def test_standby_french(self, device: DeviceClient):
        """STANDBY → EN VEILLE in French."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=0)
        _wait_widget_text(device, "hero_state", HERO_STATES["Français"]["STANDBY"])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["STANDBY"]

    def test_floor_heat_selection_shows_heating_french(self, device: DeviceClient):
        """Floor-heat selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_FLOOR_HEATING)
        _wait_widget_text(device, "hero_state", HERO_STATES["Français"]["HEATING"])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Français"]["HEATING"]

    def test_cooling_french(self, device: DeviceClient):
        """COOLING → REFROIDISSEMENT in French."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_COOLING, cooling_on=1)
        _wait_widget_text(device, "hero_state", HERO_MODES["Français"][MODE_COOLING])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_MODES["Français"][MODE_COOLING]

    def test_hot_water_selection_shows_heating_french(self, device: DeviceClient):
        """Hot-water selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_HOT_WATER)
        _wait_widget_text(device, "hero_state", HERO_STATES["Français"]["HEATING"])
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
        device.clear_all_faults()
        _wait_widget_text(device, "error_label", ERROR_CARD_NO_ERRORS["Français"],
                          contains=True)
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
        device.clear_all_faults()
        _set_idle(device, working_mode=MODE_FLOOR_HEATING)
        _wait_widget_text(device, "hero_state", HERO_STATES["Español"]["IDLE"])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["IDLE"]

    def test_fault_spanish(self, device: DeviceClient):
        """FAULT → FALLO in Spanish."""
        device.clear_all_faults()
        device.inject_fault("P02", True)
        _wait_widget_text(device, "hero_state", HERO_STATES["Español"]["FAULT"])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["FAULT"]

    def test_standby_spanish(self, device: DeviceClient):
        """STANDBY → EN ESPERA in Spanish."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=0)
        _wait_widget_text(device, "hero_state", HERO_STATES["Español"]["STANDBY"])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["STANDBY"]

    def test_floor_heat_selection_shows_heating_spanish(self, device: DeviceClient):
        """Floor-heat selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_FLOOR_HEATING)
        _wait_widget_text(device, "hero_state", HERO_STATES["Español"]["HEATING"])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_STATES["Español"]["HEATING"]

    def test_cooling_spanish(self, device: DeviceClient):
        """COOLING → ENFRIAMIENTO in Spanish."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_COOLING, cooling_on=1)
        _wait_widget_text(device, "hero_state", HERO_MODES["Español"][MODE_COOLING])
        w = device.find_widget(tag="hero_state")
        assert w is not None
        assert w.text == HERO_MODES["Español"][MODE_COOLING]

    def test_hot_water_selection_shows_heating_spanish(self, device: DeviceClient):
        """Hot-water selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_HOT_WATER)
        _wait_widget_text(device, "hero_state", HERO_STATES["Español"]["HEATING"])
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
        device.clear_all_faults()
        _wait_widget_text(device, "error_label", ERROR_CARD_NO_ERRORS["Español"],
                          contains=True)
        w = device.find_widget(tag="error_label")
        assert w is not None
        assert ERROR_CARD_NO_ERRORS["Español"] in w.text, \
            f"Expected '{ERROR_CARD_NO_ERRORS['Español']}' in error label, got '{w.text}'"
