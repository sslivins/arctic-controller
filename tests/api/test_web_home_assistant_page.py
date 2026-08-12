"""Static contracts: the web UI exposes Home Assistant pairing management.

The web UI must offer full parity with the on-device Home Assistant screen
(status, device name, start/cancel pairing, revoke). These checks read the
sources directly so the feature cannot silently regress. They also pin the
security posture: the management endpoints must be authentication-gated, and
the pairing code must never leak into a status/read response.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _api_server() -> str:
    return (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")


def _index_html() -> str:
    return (ROOT / "main" / "web" / "index.html").read_text(encoding="utf-8")


def test_web_ui_has_home_assistant_settings_page() -> None:
    html = _index_html()
    # Nav entry and render function wired into the settings pages map.
    assert '["home_assistant", "Home Assistant"]' in html
    assert "function homeAssistantSettings(" in html
    assert "home_assistant: homeAssistantSettings" in html


def test_web_ui_exposes_pairing_actions() -> None:
    html = _index_html()
    for action in ("ha-pair", "ha-cancel", "ha-revoke"):
        assert f'data-action="{action}"' in html, action
    assert '/api/ha/pair' in html
    assert '/api/ha/revoke' in html


def test_management_endpoints_are_auth_gated() -> None:
    source = _api_server()
    handlers = (
        "ha_manage_status_get_handler",
        "ha_manage_pair_post_handler",
        "ha_manage_pair_cancel_handler",
        "ha_manage_revoke_post_handler",
    )
    for handler in handlers:
        start = source.index(f"static esp_err_t {handler}(httpd_req_t* req)\n{{")
        body = source[start:start + 400]
        assert "check_api_auth" in body, handler


def test_status_endpoint_does_not_return_the_pairing_code() -> None:
    source = _api_server()
    start = source.index(
        "static esp_err_t ha_manage_status_get_handler(httpd_req_t* req)\n{"
    )
    end = source.index("static esp_err_t ha_manage_pair_post_handler(httpd_req_t* req)\n{", start)
    status_body = source[start:end]
    # The status/read response advertises paired state and pairing_active but
    # never the code itself — only the explicit pair POST returns a code.
    assert '"pairing_active"' in status_body
    assert '"code"' not in status_body
    assert "setup_pairing_start" not in status_body


def test_routes_registered_and_handler_budget_bumped() -> None:
    source = _api_server()
    for uri in ('"/api/ha/status"', '"/api/ha/pair"', '"/api/ha/revoke"'):
        assert uri in source, uri
    # Four new routes were added; the handler budget must cover them.
    assert "const int uri_handlers = 111;" in source
