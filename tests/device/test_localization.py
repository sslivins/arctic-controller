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

# Hero state words. The hero line may append a detail after STATE_SEP
# ("Défaut · P02", "En attente · à la consigne"), so tests compare the word.
HERO_STATES = {
    "English":  {"IDLE": "Idle",          "FAULT": "Fault",   "STANDBY": "Standby",
                 "DEFROST": "Defrosting", "HEATING": "Heating",
                 "COOLING": "Cooling",    "DISCONNECTED": "Disconnected"},
    "Français": {"IDLE": "En attente",    "FAULT": "Défaut",  "STANDBY": "Veille",
                 "DEFROST": "Dégivrage",  "HEATING": "Chauffage",
                 "COOLING": "Refroidissement", "DISCONNECTED": "Déconnecté"},
    "Español":  {"IDLE": "En espera",     "FAULT": "Fallo",   "STANDBY": "En reposo",
                 "DEFROST": "Descongelando", "HEATING": "Calefacción",
                 "COOLING": "Enfriamiento", "DISCONNECTED": "Desconectado"},
}

STATE_SEP = " · "

# Labels on the Home component pills (the aux heater pill is hidden while off).
COMPONENT_PILLS = {
    "English":  ["Compressor", "Fan", "Pump"],
    "Français": ["Compresseur", "Ventilateur", "Pompe"],
    "Español":  ["Compresor", "Ventilador", "Bomba"],
}

# Tile captions shown in every state (ΔT/COP/compressor tiles only while running).
TILE_CAPTIONS = {
    "English":  ["SUPPLY", "RETURN", "POWER"],
    "Français": ["DÉPART", "RETOUR", "PUISSANCE"],
    "Español":  ["IMPULSIÓN", "RETORNO", "POTENCIA"],
}

# Bottom strip labels while the compressor is not running.
HOME_STRIP_LABELS = {
    "English":  ["Last run", "Today"],
    "Français": ["Dernier cycle", "Aujourd'hui"],
    "Español":  ["Último ciclo", "Hoy"],
}

# Caption under the hero tank temperature ("Tank", "Tank · target", ...).
TANK_DESCRIPTION = {
    "English":  "Tank",
    "Français": "Ballon",
    "Español":  "Tanque",
}

FOOTER_NAV = {
    "English":  ["Status", "Control", "Events"],
    "Français": ["État", "Contrôle", "Événements"],
    "Español":  ["Estado", "Control", "Eventos"],
}

DEMO_BANNER = {
    "English":  "Demo mode",
    "Français": "Mode démo",
    "Español":  "Modo demo",
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
        # The status reads differently when paired, so accept either state.
        ("settings_home_assistant", "home_assistant", ("Associé", "Non associé")),
        ("settings_security", "security", "Sécurité"),
        ("settings_web", "web", "Interface Web"),
    ],
    "Español": [
        ("settings_wifi", "wifi", "WiFi"),
        ("settings_firmware", "firmware", "Actualización de firmware"),
        ("settings_time", "time", "Formato de visualización"),
        ("settings_language", "language", "Idioma"),
        ("settings_display", "display", "Brillo"),
        ("settings_home_assistant", "home_assistant", ("Emparejado", "Sin emparejar")),
        ("settings_security", "security", "Seguridad"),
        ("settings_web", "web", "Interfaz web"),
    ],
}

ERRORS_OVERLAY_LABELS = {
    "Français": ["Historique des erreurs"],
    "Español":  ["Historial de errores"],
}

# Fault card severity words (#304). The JSON API keeps the English words.
SEVERITY_WORDS = {
    "Français": {"info", "avertissement", "erreur", "critique"},
    "Español":  {"info", "advertencia", "error", "crítico"},
}
ENGLISH_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _hero_word(device: DeviceClient):
    """The hero state word, without any " · detail" suffix (None if absent)."""
    w = device.find_widget(tag="hero_state")
    if w is None or w.text is None:
        return None
    return w.text.split(STATE_SEP)[0]


def _assert_hero_state(device: DeviceClient, expected: str):
    """Wait for, then assert, the hero state word."""
    device.wait_until(f"hero_state word == {expected!r}",
                      lambda: _hero_word(device) == expected,
                      timeout=5.0, expect_within=UI_SETTLE,
                      raise_on_timeout=False)
    word = _hero_word(device)
    assert word == expected, f"Expected hero state {expected!r}, got {word!r}"


def _tank_caption_shown(device: DeviceClient, lang_name: str) -> bool:
    w = device.find_widget(tag="hero_sub")
    return (w is not None and w.text is not None
            and w.text.startswith(TANK_DESCRIPTION[lang_name]))


def _open_errors_overlay(device: DeviceClient):
    """Open the errors overlay via the Home fault banner.

    The banner is hidden while there are no faults, so make sure the demo fault
    is active first.
    """
    device.inject_fault(DEMO_FAULT, True)
    assert device.wait_for_widget(tag="error_label", timeout=5.0), \
        "fault banner did not appear after injecting the demo fault"
    device.click(tag="error_label")
    assert device.wait_for_screen("errors", timeout=5.0)


def _has_text_containing(device: DeviceClient, expected: str) -> bool:
    return any(expected in w.text for w in device.widgets if w.text)


def _assert_visible_text(device: DeviceClient, expected):
    """``expected`` may be a tuple of alternatives; any one of them must show."""
    options = expected if isinstance(expected, tuple) else (expected,)
    assert any(_has_text_containing(device, o) for o in options), \
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
        lambda: _tank_caption_shown(device, lang_name),
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
        """Idle → En attente in French."""
        device.clear_all_faults()
        _set_idle(device, working_mode=MODE_HEATING)
        _assert_hero_state(device, HERO_STATES["Français"]["IDLE"])

    def test_fault_french(self, device: DeviceClient):
        """Fault → Défaut in French."""
        device.clear_all_faults()
        device.inject_fault("P02", True)
        _assert_hero_state(device, HERO_STATES["Français"]["FAULT"])

    def test_standby_french(self, device: DeviceClient):
        """Standby → Veille in French."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=0)
        _assert_hero_state(device, HERO_STATES["Français"]["STANDBY"])

    def test_floor_heat_selection_shows_heating_french(self, device: DeviceClient):
        """Heating selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_HEATING)
        _assert_hero_state(device, HERO_STATES["Français"]["HEATING"])

    def test_cooling_french(self, device: DeviceClient):
        """Cooling → Refroidissement in French."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_COOLING, cooling_on=1)
        _assert_hero_state(device, HERO_STATES["Français"]["COOLING"])

    def test_hot_water_selection_shows_heating_french(self, device: DeviceClient):
        """Hot-water selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_HOT_WATER)
        _assert_hero_state(device, HERO_STATES["Français"]["HEATING"])


# =========================================================================
# French — Tank caption, Footer, Static Home Labels
# =========================================================================

class TestFrenchMainLabels:
    """Verify tank caption, footer nav, and static Home labels in French."""

    @pytest.fixture(autouse=True)
    def _switch_to_french(self, device: DeviceClient):
        _switch_language(device, "Français")

    def test_tank_description_french(self, device: DeviceClient):
        """Tank description label translates to French."""
        assert _tank_caption_shown(device, "Français"), \
            f"French tank description '{TANK_DESCRIPTION['Français']}' not found"

    def test_footer_nav_french(self, device: DeviceClient):
        """Footer nav buttons are translated to French."""
        for label in FOOTER_NAV["Français"]:
            found = any(label in w.text for w in device.widgets if w.text)
            assert found, f"French footer label '{label}' not found"

    def test_static_home_labels_refresh_french(self, device: DeviceClient):
        """Home labels that are created once still refresh after the language switch."""
        for label in (
            COMPONENT_PILLS["Français"] +
            TILE_CAPTIONS["Français"] +
            HOME_STRIP_LABELS["Français"] +
            [DEMO_BANNER["Français"]]
        ):
            _assert_visible_text(device, label)


# =========================================================================
# Spanish — Hero States
# =========================================================================

class TestSpanishHeroStates:
    """Verify hero state labels translate to Spanish."""

    @pytest.fixture(autouse=True)
    def _switch_to_spanish(self, device: DeviceClient):
        _switch_language(device, "Español")

    def test_idle_spanish(self, device: DeviceClient):
        """Idle → En espera in Spanish."""
        device.clear_all_faults()
        _set_idle(device, working_mode=MODE_HEATING)
        _assert_hero_state(device, HERO_STATES["Español"]["IDLE"])

    def test_fault_spanish(self, device: DeviceClient):
        """Fault → Fallo in Spanish."""
        device.clear_all_faults()
        device.inject_fault("P02", True)
        _assert_hero_state(device, HERO_STATES["Español"]["FAULT"])

    def test_standby_spanish(self, device: DeviceClient):
        """Standby → En reposo in Spanish."""
        device.clear_all_faults()
        device.set_demo_fields(unit_on=0)
        _assert_hero_state(device, HERO_STATES["Español"]["STANDBY"])

    def test_floor_heat_selection_shows_heating_spanish(self, device: DeviceClient):
        """Heating selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_HEATING)
        _assert_hero_state(device, HERO_STATES["Español"]["HEATING"])

    def test_cooling_spanish(self, device: DeviceClient):
        """Cooling → Enfriamiento in Spanish."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_COOLING, cooling_on=1)
        _assert_hero_state(device, HERO_STATES["Español"]["COOLING"])

    def test_hot_water_selection_shows_heating_spanish(self, device: DeviceClient):
        """Hot-water selection still reports the actual heating operation."""
        device.clear_all_faults()
        _set_running(device, working_mode=MODE_HOT_WATER)
        _assert_hero_state(device, HERO_STATES["Español"]["HEATING"])


# =========================================================================
# Spanish — Tank caption, Footer, Static Home Labels
# =========================================================================

class TestSpanishMainLabels:
    """Verify tank caption, footer nav, and static Home labels in Spanish."""

    @pytest.fixture(autouse=True)
    def _switch_to_spanish(self, device: DeviceClient):
        _switch_language(device, "Español")

    def test_tank_description_spanish(self, device: DeviceClient):
        """Tank description label translates to Spanish."""
        assert _tank_caption_shown(device, "Español"), \
            f"Spanish tank description '{TANK_DESCRIPTION['Español']}' not found"

    def test_footer_nav_spanish(self, device: DeviceClient):
        """Footer nav buttons are translated to Spanish."""
        for label in FOOTER_NAV["Español"]:
            found = any(label in w.text for w in device.widgets if w.text)
            assert found, f"Spanish footer label '{label}' not found"

    def test_static_home_labels_refresh_spanish(self, device: DeviceClient):
        """Home labels that are created once still refresh after the language switch."""
        for label in (
            COMPONENT_PILLS["Español"] +
            TILE_CAPTIONS["Español"] +
            HOME_STRIP_LABELS["Español"] +
            [DEMO_BANNER["Español"]]
        ):
            _assert_visible_text(device, label)


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
    _open_errors_overlay(device)
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


@pytest.mark.parametrize("lang_name", ["Français", "Español"])
def test_fault_cards_translate_severity_and_month(device: DeviceClient, lang_name: str):
    """Severity and start date on fault cards follow the device language (#304)."""
    # The demo fault gives at least one card with a real start time.
    _switch_language(device, lang_name)
    _open_errors_overlay(device)
    try:
        device.wait_until(
            "fault cards rendered",
            lambda: device.find_widget(tag="error_started") is not None,
            timeout=5.0,
        )
        widgets = device.widgets
        severities = [w.text for w in widgets if w.tag == "error_severity"]
        started = [w.text for w in widgets if w.tag == "error_started"]
        assert severities, "no severity labels on the fault cards"
        unknown = [s for s in severities if s not in SEVERITY_WORDS[lang_name]]
        assert not unknown, f"untranslated severity in {lang_name}: {unknown}"

        dated = [s for s in started if any(ch.isdigit() for ch in s)]
        assert dated, f"no dated fault card; saw {started}"
        english = [s for s in dated if any(m in s for m in ENGLISH_MONTHS)]
        assert not english, f"English month on {lang_name} fault cards: {english}"
        _assert_all_text_drawable(device, "errors overlay")
    finally:
        device.click(tag="errors_close")
        assert device.wait_for_screen("main", timeout=5.0)

    # The API must stay English; Home Assistant depends on it.
    faults = device.get_heatpump_errors()
    api_sev = {e.get("severity") for e in faults.get("active", [])}
    assert api_sev and api_sev <= {"info", "warning", "error", "critical"}, \
        f"API severity must stay English in {lang_name}: {api_sev}"


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
                and _has_text_containing(device, COMPONENT_PILLS[lang_name][0]),
        timeout=5.0,
    )
    for label in (DEMO_BANNER[lang_name], COMPONENT_PILLS[lang_name][0],
                  TILE_CAPTIONS[lang_name][0], FOOTER_NAV[lang_name][0]):
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
    _assert_labels_readable(device, COMPONENT_PILLS[lang_name], "home component row")

    device.click(tag="nav_events")
    assert device.wait_for_screen("event_log", timeout=5.0)
    _assert_labels_readable(device, EVENT_CHIPS[lang_name], "events chips")
    device.click(tag="nav_home")
    assert device.wait_for_screen("main", timeout=5.0)