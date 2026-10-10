"""Static regression guards for the web home page / home-history endpoint."""

from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main"


def _handler_body(source: str) -> str:
    start = source.index("static esp_err_t heatpump_home_history_get_handler(httpd_req_t* req)\n{")
    return source[start:source.index("\n}\n", start)]


def test_api_exposes_home_history_endpoint():
    source = (MAIN / "api_server.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    assert '"/api/heatpump/home-history"' in source
    body = _handler_body(source)
    assert "check_api_auth" in body
    # Same helpers as the device home chart, so web and device always agree.
    assert "home_stats_tank_series(" in body
    assert "home_stats_runs(" in body
    assert "fault_history_query(" in body
    assert "history_storage_query_telemetry(" in body


def test_status_exposes_home_fields():
    source = (MAIN / "api_server.cpp").read_text(encoding="utf-8")
    assert '"active_setpoint"' in source
    assert '"energy_today_wh"' in source


def test_openapi_documents_home_history():
    spec = (ROOT / "docs" / "openapi.yaml").read_text(encoding="utf-8")
    assert "/api/heatpump/home-history:" in spec
    assert "active_setpoint:" in spec
    assert "energy_today_wh:" in spec


def test_web_home_mirrors_device_home():
    web = (MAIN / "web" / "index.html").read_text(encoding="utf-8")
    for name in ("homePage", "homeHeroState", "loadHomeHistory", "homeChart", "homeChartLayout", "homeHover"):
        assert f"function {name}(" in web, name
    assert '"/api/heatpump/home-history"' in web
    assert "home: homePage" in web or "home: () => homePage" in web
