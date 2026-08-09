"""Prevent the removed legacy transport from being reintroduced."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
THIS_FILE = Path(__file__).resolve()
TEXT_SUFFIXES = {".cpp", ".h", ".md", ".py", ".txt", ".yaml", ".yml"}
EXCLUDED_DIRS = {
    ".git",
    "build",
    "managed_components",
}
FORBIDDEN_MARKERS = {
    "modbus::",
    "modbus_manager",
    "ARCTIC_MODBUS_MASTER",
    "espressif/esp-modbus",
    "CONFIG_FMB_MASTER_TIMEOUT_MS_RESPOND",
    "2400 baud",
    "modbus_tests",
    "SimulatorClient",
    "SIMULATOR_URL",
    "modbus_mode",
}


def _project_text_files():
    for path in ROOT.rglob("*"):
        if (
            path.is_file()
            and path.resolve() != THIS_FILE
            and path.suffix.lower() in TEXT_SUFFIXES
            and not any(part in EXCLUDED_DIRS for part in path.parts)
        ):
            yield path


def test_legacy_modbus_transport_is_absent():
    violations = []
    for path in _project_text_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for marker in FORBIDDEN_MARKERS:
            if marker in text:
                violations.append(f"{path.relative_to(ROOT)}: {marker}")

    assert not violations, "Legacy Modbus references found:\n" + "\n".join(violations)


def test_removed_protocol_surfaces_are_absent():
    assert not (ROOT / "docs" / "ARCTIC-MODBUS-PROTOCOL.md").exists()

    api_source = (ROOT / "main" / "api_server.cpp").read_text(encoding="utf-8")
    openapi = (ROOT / "docs" / "openapi.yaml").read_text(encoding="utf-8")
    assert "/api/heatpump/control" not in api_source
    assert "/api/heatpump/control" not in openapi
