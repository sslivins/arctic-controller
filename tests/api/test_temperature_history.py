"""Static regression guards for persistent temperature history."""

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main"


def test_telemetry_region_retains_at_least_fourteen_days():
    header = (MAIN / "history_storage.h").read_text(encoding="utf-8")
    source = (MAIN / "history_storage.cpp").read_text(encoding="utf-8")

    assert "#define HISTORY_EVENT_REGION_SIZE (256 * 1024)" in header
    assert "#define HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC 30" in header
    assert "#define HISTORY_TELEMETRY_RETENTION_DAYS 14" in header
    assert "static_assert(sizeof(telemetry_record_t) == TELEMETRY_RECORD_SIZE" in source

    partition_bytes = 2 * 1024 * 1024
    telemetry_bytes = partition_bytes - 256 * 1024
    sectors = telemetry_bytes // 4096
    records_per_sector = (4096 - 32) // 32
    required_samples = 14 * 24 * 60 * 60 // 30

    assert sectors * records_per_sector >= required_samples


def test_history_partition_does_not_overlap_event_region():
    source = (MAIN / "history_storage.cpp").read_text(encoding="utf-8")
    assert "TELEMETRY_REGION_OFFSET = HISTORY_EVENT_REGION_SIZE" in source
    assert "telemetry_sector_offset" in source
    assert "history_storage_prepare_factory_reset" in source
    assert source.index("if (s_factory_reset_pending)") < source.index(
        "if (s_telemetry_initialized)"
    )
    assert "esp_partition_read(s_partition, telemetry_sector_offset(sector)," in source
    assert "sector_data, FLASH_SECTOR_SIZE" in source


def test_recorder_uses_validity_flags_and_thirty_second_cadence():
    source = (MAIN / "telemetry_history.cpp").read_text(encoding="utf-8")
    assert "HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC * 1000" in source
    for flag in (
        "HISTORY_TELEMETRY_INLET_VALID",
        "HISTORY_TELEMETRY_OUTLET_VALID",
        "HISTORY_TELEMETRY_SETPOINT_VALID",
        "HISTORY_TELEMETRY_COMPRESSOR_VALID",
        "HISTORY_TELEMETRY_COMPRESSOR_RUNNING",
    ):
        assert flag in source


def test_device_history_is_eight_hour_custom_draw_page():
    source = (MAIN / "heatpump_history_screen.cpp").read_text(encoding="utf-8")
    assert "WINDOW_SECONDS = 8 * 60 * 60" in source
    assert "LV_EVENT_DRAW_MAIN_END" in source
    assert "lv_draw_rect" in source
    assert "lv_draw_line" in source
    assert "lv_canvas" not in source
    assert source.count("WINDOW_SECONDS") >= 5
    assert "NAV_BAR_H + 18" in source

    status = (MAIN / "heatpump_temps_screen.cpp").read_text(encoding="utf-8")
    assert '"temperature_history_open"' in status
    assert "heatpump_history_show" in status


def test_temperature_history_strings_exist_in_all_languages():
    source = (MAIN / "i18n" / "i18n.cpp").read_text(encoding="utf-8")
    for name in (
        "STR_HISTORY_TITLE",
        "STR_HISTORY_PREVIOUS",
        "STR_HISTORY_NEXT",
        "STR_HISTORY_NO_DATA",
    ):
        assert len(re.findall(rf"\[{name}\]\s*=", source)) == 3


def test_web_api_exposes_temperature_history_endpoint():
    source = (MAIN / "api_server.cpp").read_text(encoding="utf-8")
    assert '"/api/heatpump/temperature-history"' in source
    assert "heatpump_temperature_history_get_handler" in source
    # Serves the data straight from the persistent telemetry store.
    assert "history_storage_query_telemetry" in source
    # Streamed in chunks so ~960 samples never build a huge in-RAM buffer.
    assert "httpd_resp_sendstr_chunk" in source
    # Guarded by the same API auth as the rest of the heatpump API.
    assert "check_api_auth" in source


def test_web_dashboard_has_temperature_history_view():
    web = (MAIN / "web" / "index.html").read_text(encoding="utf-8")
    # Registered as a first-class nav route and page.
    assert '["history", ' in web
    assert "history: historyPage" in web
    assert "function historyPage" in web
    assert "function loadHistory" in web
    assert "function historyChartSvg" in web
    # Fetches from the streaming endpoint above.
    assert "/api/heatpump/temperature-history" in web
    # Same 8-hour window / whole-hour axis rounding as the device chart.
    assert "getHours() % 2 !== 0" in web

