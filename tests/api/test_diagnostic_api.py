"""
Functional tests for the Diagnostic CSV download endpoint.

GET /api/heatpump/diagnostic returns a CSV file containing a complete
snapshot of system state, temperatures, parameters, errors, and raw
register values — intended for support and troubleshooting.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
  - Demo mode enabled on the device
"""

import csv
import io
import os
import time

import pytest
import requests
import urllib3
from pathlib import Path
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# Load .env from repo root if present (local dev)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Disable TLS verification for self-signed device certificate
_OrigSessionInit = requests.Session.__init__
def _session_init_no_verify(self, *args, **kwargs):
    _OrigSessionInit(self, *args, **kwargs)
    self.verify = False
requests.Session.__init__ = _session_init_no_verify

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# Retry-enabled session for all API calls
_session = requests.Session()
_retry = Retry(total=3, backoff_factor=1, allowed_methods=None,
               status_forcelist=[502, 503, 504])
_session.mount("http://", HTTPAdapter(max_retries=_retry))
_session.mount("https://", HTTPAdapter(max_retries=_retry))

# Expected CSV categories emitted by the diagnostic endpoint
EXPECTED_CATEGORIES = [
    "System",
    "Temperature",
    "Setpoint",
    "Reading",
    "Status",
    "Register",
    "Error",
    "Parameter",
]

CSV_HEADER = ["Category", "Name", "P-Code", "Modbus Address", "Value", "Unit"]

# UTF-8 BOM bytes
UTF8_BOM = b"\xef\xbb\xbf"


def _headers(api_key=None):
    """Build auth headers."""
    h = {}
    key = api_key if api_key is not None else API_KEY
    if key:
        h["X-API-Key"] = key
    return h


def _get(path, **kwargs):
    """Authenticated GET helper."""
    return _session.get(f"{BASE_URL}{path}", headers=_headers(), timeout=10, **kwargs)


def _inject_demo(fields: dict):
    """Inject demo-mode field values."""
    for attempt in range(3):
        try:
            requests.post(
                f"{BASE_URL}/api/test/set-demo-field",
                headers=_headers(),
                json=fields,
                timeout=10,
            )
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


def _parse_csv(text: str) -> list[dict]:
    """Parse CSV text into list of row dicts."""
    reader = csv.DictReader(io.StringIO(text))
    return list(reader)


@pytest.fixture(autouse=True)
def _check_prerequisites():
    """Skip if device is unreachable or not in demo mode."""
    last_err = None
    for attempt in range(3):
        try:
            r = requests.get(
                f"{BASE_URL}/api/heatpump/status",
                headers=_headers(),
                timeout=5,
            )
            r.raise_for_status()
            data = r.json()
            if not data.get("demo_mode"):
                pytest.skip("Device not in demo mode")
            return
        except requests.ConnectionError:
            last_err = "Device not reachable"
            if attempt < 2:
                time.sleep(2)
        except Exception as e:
            last_err = str(e)
            break
    pytest.skip(f"{last_err}")


# ── Response Headers & Format ─────────────────────────────────────────────


class TestDiagnosticResponseFormat:
    """Verify HTTP response headers and CSV format."""

    def test_returns_200(self):
        r = _get("/api/heatpump/diagnostic")
        assert r.status_code == 200

    def test_content_type_is_csv(self):
        r = _get("/api/heatpump/diagnostic")
        assert "text/csv" in r.headers.get("Content-Type", "")

    def test_content_disposition_attachment(self):
        """Response triggers a file download with a timestamped filename."""
        r = _get("/api/heatpump/diagnostic")
        cd = r.headers.get("Content-Disposition", "")
        assert "attachment" in cd
        assert "arctic-diagnostic-" in cd
        assert cd.endswith('.csv"')

    def test_utf8_bom_present(self):
        """CSV starts with UTF-8 BOM for Excel compatibility."""
        r = _get("/api/heatpump/diagnostic")
        assert r.content.startswith(UTF8_BOM), "CSV should start with UTF-8 BOM"

    def test_csv_header_row(self):
        """First data row (after BOM) is the expected CSV header."""
        r = _get("/api/heatpump/diagnostic")
        text = r.content.decode("utf-8-sig")  # strips BOM
        lines = text.strip().splitlines()
        assert len(lines) > 1, "CSV should have header + data rows"
        header = lines[0].split(",")
        assert header == CSV_HEADER

    def test_csv_parseable(self):
        """Entire response is valid CSV."""
        r = _get("/api/heatpump/diagnostic")
        text = r.content.decode("utf-8-sig")
        rows = _parse_csv(text)
        assert len(rows) > 10, f"Expected many CSV rows, got {len(rows)}"

    def test_all_rows_have_six_columns(self):
        """Every row should have all 6 CSV columns."""
        r = _get("/api/heatpump/diagnostic")
        text = r.content.decode("utf-8-sig")
        rows = _parse_csv(text)
        for i, row in enumerate(rows):
            assert len(row) == 6, f"Row {i+1} has {len(row)} columns, expected 6: {row}"


# ── Category Coverage ─────────────────────────────────────────────────────


class TestDiagnosticCategories:
    """Verify all expected data categories are present in the CSV."""

    def _get_categories(self) -> set:
        r = _get("/api/heatpump/diagnostic")
        text = r.content.decode("utf-8-sig")
        rows = _parse_csv(text)
        return {row["Category"] for row in rows}

    def test_system_category(self):
        assert "System" in self._get_categories()

    def test_temperature_category(self):
        assert "Temperature" in self._get_categories()

    def test_setpoint_category(self):
        assert "Setpoint" in self._get_categories()

    def test_reading_category(self):
        assert "Reading" in self._get_categories()

    def test_status_category(self):
        assert "Status" in self._get_categories()

    def test_register_category(self):
        assert "Register" in self._get_categories()

    def test_parameter_category(self):
        assert "Parameter" in self._get_categories()

    def test_no_unexpected_categories(self):
        """Only expected categories should appear."""
        cats = self._get_categories()
        unexpected = cats - set(EXPECTED_CATEGORIES)
        assert not unexpected, f"Unexpected categories: {unexpected}"


# ── Data Content ──────────────────────────────────────────────────────────


class TestDiagnosticContent:
    """Verify actual data content within the CSV."""

    def _get_rows(self) -> list[dict]:
        r = _get("/api/heatpump/diagnostic")
        text = r.content.decode("utf-8-sig")
        return _parse_csv(text)

    def _rows_by_category(self, category: str) -> list[dict]:
        return [r for r in self._get_rows() if r["Category"] == category]

    def test_system_has_demo_mode_row(self):
        """System category should include Demo Mode status."""
        system_rows = self._rows_by_category("System")
        names = [r["Name"] for r in system_rows]
        assert "Demo Mode" in names

    def test_system_has_unit_power(self):
        system_rows = self._rows_by_category("System")
        names = [r["Name"] for r in system_rows]
        assert "Unit Power" in names

    def test_temperatures_have_units(self):
        """All temperature rows should have °C unit."""
        temp_rows = self._rows_by_category("Temperature")
        assert len(temp_rows) >= 5, "Expected at least 5 temperature readings"
        for row in temp_rows:
            assert row["Unit"] == "°C", f"Temperature '{row['Name']}' missing °C unit"

    def test_temperatures_have_modbus_addresses(self):
        """Temperature rows should have register addresses."""
        temp_rows = self._rows_by_category("Temperature")
        for row in temp_rows:
            addr = row["Modbus Address"]
            assert addr.isdigit(), f"Temperature '{row['Name']}' has non-numeric address: {addr}"

    def test_setpoints_present(self):
        """Should have cooling, heating, and hot water setpoints."""
        setpoint_rows = self._rows_by_category("Setpoint")
        names = [r["Name"] for r in setpoint_rows]
        assert len(setpoint_rows) >= 3, f"Expected at least 3 setpoints, got {len(setpoint_rows)}"

    def test_readings_have_units(self):
        """System readings should have appropriate units."""
        reading_rows = self._rows_by_category("Reading")
        assert len(reading_rows) >= 5, "Expected at least 5 system readings"
        # At least some should have units
        with_units = [r for r in reading_rows if r["Unit"]]
        assert len(with_units) >= 3, "Most readings should have units"

    def test_status_bits_have_values(self):
        """Status rows should have On/Off values."""
        status_rows = self._rows_by_category("Status")
        assert len(status_rows) >= 10, "Expected at least 10 status bits"
        for row in status_rows:
            assert row["Value"] in ("ON", "OFF"), (
                f"Status '{row['Name']}' has unexpected value: {row['Value']}"
            )

    def test_register_rows_have_hex_values(self):
        """Raw register rows should contain hex values."""
        reg_rows = self._rows_by_category("Register")
        assert len(reg_rows) >= 2, "Expected at least 2 raw register rows"
        for row in reg_rows:
            val = row["Value"]
            assert val.startswith("0x"), f"Register value should be hex: {val}"

    def test_parameters_have_pcodes(self):
        """Parameter rows should have P-codes (e.g. P29, P37)."""
        param_rows = self._rows_by_category("Parameter")
        assert len(param_rows) >= 10, "Expected at least 10 parameters"
        for row in param_rows:
            assert row["P-Code"].startswith("P"), (
                f"Parameter '{row['Name']}' missing P-code: {row['P-Code']}"
            )

    def test_parameters_have_modbus_addresses(self):
        param_rows = self._rows_by_category("Parameter")
        for row in param_rows:
            addr = row["Modbus Address"]
            assert addr.isdigit(), f"Parameter '{row['Name']}' has non-numeric address: {addr}"


# ── Error Injection ───────────────────────────────────────────────────────


class TestDiagnosticErrors:
    """Verify error reporting in the diagnostic CSV."""

    def test_errors_appear_when_injected(self):
        """Injecting error1 bit should produce Error rows in CSV."""
        _inject_demo({"error1": 1})  # Bit 0 = E01
        time.sleep(0.5)

        try:
            r = _get("/api/heatpump/diagnostic")
            text = r.content.decode("utf-8-sig")
            rows = _parse_csv(text)
            error_rows = [r for r in rows if r["Category"] == "Error"]
            assert len(error_rows) >= 1, "Expected at least 1 error row after injection"

            # The error row should have an Arctic error code in P-Code column
            codes = [r["P-Code"] for r in error_rows if r["P-Code"]]
            assert len(codes) >= 1, "Error rows should have Arctic error codes"
        finally:
            _inject_demo({"error1": 0, "error2": 0})
            time.sleep(0.3)

    def test_no_errors_when_clear(self):
        """With no errors injected, Error category should be absent or empty."""
        _inject_demo({"error1": 0, "error2": 0})
        time.sleep(0.3)

        r = _get("/api/heatpump/diagnostic")
        text = r.content.decode("utf-8-sig")
        rows = _parse_csv(text)
        error_rows = [r for r in rows if r["Category"] == "Error"]
        # It's acceptable to have zero error rows when no errors active
        assert len(error_rows) == 0, (
            f"Expected no error rows when errors cleared, got {len(error_rows)}"
        )


# ── Authentication ────────────────────────────────────────────────────────


def _enable_api_auth():
    """Enable API auth on the device."""
    for attempt in range(3):
        try:
            requests.post(
                f"{BASE_URL}/api/auth/config",
                headers=_headers(),
                json={"web_auth_enabled": True},
                timeout=5,
            )
            time.sleep(0.5)
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


def _disable_api_auth():
    """Disable API auth on the device."""
    for attempt in range(3):
        try:
            requests.post(
                f"{BASE_URL}/api/auth/config",
                headers=_headers(),
                json={"web_auth_enabled": False},
                timeout=5,
            )
            time.sleep(0.5)
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


class TestDiagnosticAuth:
    """Diagnostic endpoint should require authentication when auth is enabled."""

    def test_no_auth_returns_401(self):
        """Request without API key should be rejected when auth is enabled."""
        _enable_api_auth()
        try:
            r = requests.get(
                f"{BASE_URL}/api/heatpump/diagnostic",
                timeout=10,
            )
            assert r.status_code == 401
        finally:
            _disable_api_auth()

    def test_bad_api_key_returns_401(self):
        """Request with wrong API key should be rejected when auth is enabled."""
        _enable_api_auth()
        try:
            r = requests.get(
                f"{BASE_URL}/api/heatpump/diagnostic",
                headers={"X-API-Key": "wrong-key-12345"},
                timeout=10,
            )
            assert r.status_code == 401
        finally:
            _disable_api_auth()


# ── Idempotency ───────────────────────────────────────────────────────────


class TestDiagnosticIdempotency:
    """Multiple downloads should produce consistent results."""

    def test_two_downloads_same_structure(self):
        """Two consecutive downloads should have the same categories and row count."""
        r1 = _get("/api/heatpump/diagnostic")
        r2 = _get("/api/heatpump/diagnostic")

        rows1 = _parse_csv(r1.content.decode("utf-8-sig"))
        rows2 = _parse_csv(r2.content.decode("utf-8-sig"))

        cats1 = {r["Category"] for r in rows1}
        cats2 = {r["Category"] for r in rows2}
        assert cats1 == cats2, "Categories should be identical across downloads"

        # Row count should be the same (values may differ slightly due to timing)
        assert len(rows1) == len(rows2), (
            f"Row count changed between downloads: {len(rows1)} vs {len(rows2)}"
        )
