"""Static contracts: device securing is decoupled from Home Assistant.

Setting the administrator password must be reachable without pairing Home
Assistant. These checks read the sources directly so the decoupling cannot
silently regress.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _credential_change_page(index_html: str) -> str:
    start = index_html.index('data-form="change-credentials"')
    end = index_html.index("</form>", start)
    return index_html[start:end]


def test_web_securing_copy_points_at_security_screen_not_home_assistant() -> None:
    index_html = (ROOT / "main" / "web" / "index.html").read_text(
        encoding="utf-8"
    )
    page = _credential_change_page(index_html)

    assert "Settings" in page and "Security" in page
    assert "Home Assistant" not in page
    assert "fingerprint" not in page.lower()


def test_security_screen_uses_neutral_setup_primitive() -> None:
    screen = (
        ROOT / "main" / "settings" / "settings_security_screen.cpp"
    ).read_text(encoding="utf-8")

    assert "setup_pairing_start" in screen
    assert "auth_mgr_credentials_change_required" in screen
    # Securing must not depend on any Home Assistant integration state.
    assert "ha_integration" not in screen
    assert "integration_token" not in screen


def test_setup_pairing_primitive_has_no_home_assistant_dependency() -> None:
    header = (ROOT / "main" / "setup_pairing.h").read_text(encoding="utf-8")
    source = (ROOT / "main" / "setup_pairing.cpp").read_text(encoding="utf-8")

    # The physical-presence primitive is HA-neutral; only its consumers decide
    # whether a Home Assistant token is issued.
    assert "ha_integration.h" not in header
    assert "ha_integration.h" not in source
