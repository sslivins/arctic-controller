"""Test: Localization — French & Spanish labels update without a reboot."""

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
MODE_HEATING = 1
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
                 "DEFROST": "DESHIELO", "HEATING": "CALEFACCIÓN",
                 "DISCONNECTED": "DESCONECTADO"},
}

HERO_MODES = {
    "English":  {MODE_HEATING: "HEATING", MODE_COOLING: "COOLING",
                 MODE_HOT_WATER: "HOT WATER"},
    "Français": {MODE_HEATING: "CHAUFFAGE", MODE_COOLING: "REFROIDISSEMENT",
                 MODE_HOT_WATER: "EAU CHAUDE"},
    "Español":  {MODE_HEATING: "CALEFACCIÓN", MODE_COOLING: "ENFRIAMIENTO",
                 MODE_HOT_WATER: "AGUA CALIENTE"},
}

COMPONENT_DOTS = {
    "English":  ["Compressor", "Fan", "Pump", "Aux Heat"],
    "Français": ["Compresseur", "Ventilateur", "Pompe", "Appoint"],
    "Español":  ["Compresor", "Ventilador", "Bomba", "Apoyo"],
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
    "Español":  "Temperatura del depósito",
}

FOOTER_NAV = {
    "English":  ["Status", "Control", "Events"],
    "Français": ["État", "Contrôle", "Événements"],
    "Español":  ["Estado", "Control", "Eventos"],
}

HOME_PANEL_HEADERS = {
    "English":  ["Temperatures", "Compressor", "Energy"],
    "Français": ["Températures", "Compresseur", "Énergie"],
    "Español":  ["Temperaturas", "Compresor", "Energía"],
}

DEMO_BANNER = {
    "English":  "Demo Mode Enabled",
    "Français": "Mode démo activé",
    "Español":  "Modo demo activado",
}

STATUS_TAB_LABELS = {
    "Français": ["Températures", "Fréquence"],
    "Español":  ["Temperaturas", "Frecuencia"],
}

CONTROL_TAB_LABELS = {
    "Français": ["Sélection du mode", "CHAUFFAGE", "Consignes"],
    "Español":  ["Selección de modo", "CALEFACCIÓN", "Consignas"],
}

EVENTS_TAB_LABELS = {
    "Français": ["Rechercher...", "Filtres"],
    "Español":  ["Buscar...", "Filtros"],
}

SETTINGS_MENU_LABELS = {
    "Français": ["Paramètres", "Mise à jour", "Langue", "Température"],
    "Español":  ["Ajustes", "Actualizar", "Idioma", "Temperatura"],
}

SETTINGS_SUBSCREEN_LABELS = {
    "Français": [
        ("settings_wifi", "wifi", "WiFi"),
        ("settings_firmware", "firmware", "Mise à jour du firmware"),
        ("settings_time", "time", "Format d'affichage"),
        ("settings_language", "language", "Langue"),
        ("settings_display", "display", "Luminosité"),
        ("settings_home_assistant", "home_assistant", "Associé"),
        ("settings_security", "security", "Sécurité"),
        ("settings_web", "web", "Interface Web"),
    ],
    "Español": [
        ("settings_wifi", "wifi", "WiFi"),
        ("settings_firmware", "firmware", "Actualización de firmware"),
        ("settings_time", "time", "Formato de visualización"),
        ("settings_language", "language", "Idioma"),
        ("settings_display", "display", "Brillo"),
        ("settings_home_assistant", "home_assistant", "Emparejado"),
        ("settings_security", "security", "Seguridad"),
        ("settings_web", "web", "Interfaz web"),
    ],
}

ERRORS_OVERLAY_LABELS = {
    "Français": ["Historique des erreurs"],
    "Español":  ["Historial de errores"],
}

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


def _has_text_containing(device: DeviceClient, expected: str) -> bool:
    return any(expected in w.text for w in device.widgets if w.text)


def _assert_visible_text(device: DeviceClient, expected: str):
    assert _has_text_containing(device, expected), \
        f"Expected visible text containing {expected!r}; saw {[w.text for w in device.widgets if w.text]}"


def _assert_all_text_drawable(device: DeviceClient, where: str):
    """No visible text on this screen uses a character its font can't draw.

    Catches widgets drawn with LVGL's built-in ASCII-only fonts, where an
    accented letter renders as an empty box (e.g. the old events search
    placeholder "Rechercher des ▯v▯nements"). The static check in
    tests/test_ui_glyphs.py can't see which font a widget ends up using.
    """
    bad = [
        (w.tag or w.type, w.text if w.missing_glyphs else w.placeholder)
        for w in device.widgets
        if w.missing_glyphs or w.placeholder_missing_glyphs
    ]
    assert not bad, f"{where}: text drawn with a font missing its characters: {bad}"


def _open_settings(device: DeviceClient):
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)


def _close_settings(device: DeviceClient):
    device.click(tag="settings_close")
    assert device.wait_for_screen("main", timeout=5.0)


def _switch_language(device: DeviceClient, lang_name: str):
    """Navigate to Settings → Language → select language → return to main."""
    _open_settings(device)

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

    _close_settings(device)

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
    _set_running(device, working_mode=MODE_HEATING)
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
        _set_idle(device, working_mode=MODE_HEATING)
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
        """Heating selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_HEATING)
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

    def test_static_home_labels_refresh_french(self, device: DeviceClient):
        """Home labels that are created once still refresh after the language switch."""
        for label in (
            COMPONENT_DOTS["Français"] +
            PERF_STRIP_LABELS["Français"] +
            HOME_PANEL_HEADERS["Français"] +
            [DEMO_BANNER["Français"]]
        ):
            _assert_visible_text(device, label)

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
        _set_idle(device, working_mode=MODE_HEATING)
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
        """Heating selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_HEATING)
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

    def test_static_home_labels_refresh_spanish(self, device: DeviceClient):
        """Home labels that are created once still refresh after the language switch."""
        for label in (
            COMPONENT_DOTS["Español"] +
            PERF_STRIP_LABELS["Español"] +
            HOME_PANEL_HEADERS["Español"] +
            [DEMO_BANNER["Español"]]
        ):
            _assert_visible_text(device, label)

    def test_error_card_no_errors_spanish(self, device: DeviceClient):
        """Error card shows Spanish 'no errors' text."""
        device.clear_all_faults()
        _wait_widget_text(device, "error_label", ERROR_CARD_NO_ERRORS["Español"],
                          contains=True)
        w = device.find_widget(tag="error_label")
        assert w is not None
        assert ERROR_CARD_NO_ERRORS["Español"] in w.text, \
            f"Expected '{ERROR_CARD_NO_ERRORS['Español']}' in error label, got '{w.text}'"


@pytest.mark.parametrize("lang_name", ["Français", "Español"])
def test_tabs_settings_and_overlays_refresh_after_touch_language_change(
    device: DeviceClient, lang_name: str
):
    """Representative labels on every main tab and settings screen update live."""
    _switch_language(device, lang_name)

    for label in (TANK_DESCRIPTION[lang_name], *FOOTER_NAV[lang_name]):
        _assert_visible_text(device, label)
    _assert_all_text_drawable(device, "home")

    device.click(tag="nav_status")
    assert device.wait_for_screen("status", timeout=5.0)
    for label in STATUS_TAB_LABELS[lang_name]:
        _assert_visible_text(device, label)
    _assert_all_text_drawable(device, "status tab")

    device.click(tag="nav_control")
    assert device.wait_for_screen("control", timeout=5.0)
    for label in CONTROL_TAB_LABELS[lang_name]:
        _assert_visible_text(device, label)
    _assert_all_text_drawable(device, "control tab")

    device.click(tag="nav_events")
    assert device.wait_for_screen("event_log", timeout=5.0)
    for label in EVENTS_TAB_LABELS[lang_name]:
        _assert_visible_text(device, label)
    _assert_all_text_drawable(device, "events tab")

    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)
    device.clear_all_faults()
    device.click(tag="error_label")
    assert device.wait_for_screen("errors", timeout=5.0)
    for label in ERRORS_OVERLAY_LABELS[lang_name]:
        _assert_visible_text(device, label)
    _assert_all_text_drawable(device, "errors overlay")
    device.click(tag="errors_close")
    assert device.wait_for_screen("main", timeout=5.0)

    _open_settings(device)
    for label in SETTINGS_MENU_LABELS[lang_name]:
        _assert_visible_text(device, label)
    _assert_all_text_drawable(device, "settings menu")

    for row_tag, screen, label in SETTINGS_SUBSCREEN_LABELS[lang_name]:
        device.click(tag=row_tag)
        assert device.wait_for_screen(screen, timeout=5.0)
        _assert_visible_text(device, label)
        _assert_all_text_drawable(device, f"settings {screen}")
        device.click(tag=f"{screen}_back")
        assert device.wait_for_screen("settings", timeout=5.0)

    _close_settings(device)


# Notification text shown on the device (the mocks carry no version/count
# detail, so the device falls back to the generic translated text).
NOTIFY_TEXT = {
    "Français": {"notify_item_firmware": "Mise à jour disponible",
                 "notify_item_brownout": "Baisse de tension détectée"},
    "Español":  {"notify_item_firmware": "Actualización disponible",
                 "notify_item_brownout": "Caída de tensión detectada"},
}


@pytest.mark.parametrize("lang_name", ["Français", "Español"])
def test_search_overlay_and_notifications_are_translated_and_drawable(
    device: DeviceClient, lang_name: str
):
    """Popups outside the tab tree: events search and the notification bell."""
    _switch_language(device, lang_name)

    device.click(tag="nav_events")
    assert device.wait_for_screen("event_log", timeout=5.0)
    device.click(tag="event_search_open")
    assert device.wait_for_widget(tag="event_search_input", timeout=5.0)
    _assert_all_text_drawable(device, "events search overlay")
    device.click(tag="event_search_cancel")
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)

    device.notification_mock_reset()
    try:
        device.notification_mock(0, "Firmware v99.0.0 available")
        device.notification_mock(3, "Brownout detected (2) - check power supply")
        device.click(tag="notifications")
        assert device.wait_for_widget(tag="notify_item_firmware", timeout=5.0)
        for item_tag, expected in NOTIFY_TEXT[lang_name].items():
            assert device.find_widget(tag=item_tag) is not None, f"{item_tag} not shown"
            _assert_visible_text(device, expected)
        _assert_all_text_drawable(device, "notifications dropdown")
        device.click(tag="notifications")
    finally:
        device.notification_mock_reset()


@pytest.mark.parametrize("lang_code,lang_name", [("fr", "Français"), ("es", "Español")])
def test_rest_language_preference_refreshes_visible_ui(
    device: DeviceClient, lang_code: str, lang_name: str
):
    """PATCH /api/preferences updates already-built tab labels without rebooting."""
    assert device.wait_for_screen("main", timeout=5.0)

    response = device.update_preferences(language=lang_code)
    assert response["success"] is True
    device.wait_until(
        f"language preference is {lang_name}",
        lambda: device.get_preferences().get("language") == lang_name,
        timeout=5.0,
    )

    device.wait_until(
        f"home label refreshed to {lang_name}",
        lambda: _has_text_containing(device, DEMO_BANNER[lang_name])
                and _has_text_containing(device, COMPONENT_DOTS[lang_name][0]),
        timeout=5.0,
    )
    for label in (DEMO_BANNER[lang_name], COMPONENT_DOTS[lang_name][0],
                  PERF_STRIP_LABELS[lang_name][0], FOOTER_NAV[lang_name][0]):
        _assert_visible_text(device, label)

# Category chips on the Events tab, in the order they are laid out.
EVENT_CHIPS = {
    "English":  ["Problems", "Equipment", "Changes", "System"],
    "Français": ["Problèmes", "Équipement", "Modifications", "Système"],
    "Español":  ["Problemas", "Equipo", "Cambios", "Sistema"],
}

# Long labels may drop to a smaller font to fit, but never below this.
MIN_LABEL_FONT_PX = 20


def _assert_labels_readable(device: DeviceClient, labels: list[str], where: str):
    found = {w.text: w.font_px for w in device.widgets if w.text in labels}
    missing = [t for t in labels if t not in found]
    assert not missing, f"{where}: labels not shown: {missing}"
    small = {t: px for t, px in found.items() if not px or px < MIN_LABEL_FONT_PX}
    assert not small, f"{where}: labels below {MIN_LABEL_FONT_PX}px: {small}"


@pytest.mark.parametrize("lang_name", ["English", "Français", "Español"])
def test_component_row_and_event_chips_stay_readable(device: DeviceClient, lang_name: str):
    """Full words fit on the Home component row and Events chips without tiny text."""
    _switch_language(device, lang_name)
    _assert_labels_readable(device, COMPONENT_DOTS[lang_name], "home component row")

    device.click(tag="nav_events")
    assert device.wait_for_screen("event_log", timeout=5.0)
    _assert_labels_readable(device, EVENT_CHIPS[lang_name], "events chips")
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)