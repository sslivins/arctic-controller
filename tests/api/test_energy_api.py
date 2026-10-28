"""Static regression guards for the energy ledger endpoints."""

from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main"


def _source() -> str:
    return (MAIN / "api_server.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")


def _handler_body(source: str, name: str) -> str:
    start = source.index(f"static esp_err_t {name}(httpd_req_t* req)\n{{")
    return source[start:source.index("\n}\n", start)]


def test_energy_endpoints_registered_with_auth():
    source = _source()
    for uri, handler in (
        ("/api/energy/daily", "energy_daily_get_handler"),
        ("/api/energy/totals", "energy_totals_get_handler"),
        ("/api/energy/runs", "energy_runs_get_handler"),
    ):
        assert f'"{uri}"' in source, uri
        assert "check_api_auth" in _handler_body(source, handler), handler


def test_daily_includes_open_day_and_stored_days():
    body = _handler_body(_source(), "energy_daily_get_handler")
    assert "getEnergyToday(" in body
    assert "read_days(" in body


def test_temperature_history_reports_power():
    source = _source()
    body = _handler_body(source, "heatpump_temperature_history_get_handler")
    assert "HISTORY_TELEMETRY_POWER_VALID" in body
    assert '\\"p\\"' in body


def test_openapi_documents_energy():
    spec = (ROOT / "docs" / "openapi.yaml").read_text(encoding="utf-8")
    assert "/api/energy/daily:" in spec
    assert "/api/energy/totals:" in spec
    assert "/api/energy/runs:" in spec
    assert "    EnergyRun:" in spec
    assert "    EnergyDay:" in spec
    assert "covered_electrical_wh:" in spec
