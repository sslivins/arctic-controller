"""Guard the opaque-Macon boundary in the controller (main/).

The controller must carry ZERO Tuya/Macon protocol knowledge: no register-map
header, no REG_* register symbols, and no OEM fault-code string literals. All of
that lives behind the arctic-macon library's typed/opaque APIs.

Scope of the gate is the controller sources under ``main/`` only, excluding:
  * ``main/tuya/`` — the Macon master transport shim (relocated into the library
    in a later phase; not part of the opaque-consumer contract yet).
  * ``advanced_params.{cpp,h}`` — the advanced-parameters subsystem is a
    separate future phase and legitimately references raw registers.
"""

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main"
THIS_FILE = Path(__file__).resolve()

SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".c", ".cc"}

# Directories (relative to main/) that are out of scope for the gate.
EXCLUDED_DIR_PARTS = {"tuya"}
# Individual files (relative to main/) that are out of scope for the gate.
EXCLUDED_FILES = {"advanced_params.cpp", "advanced_params.h"}
# Generated font / image blobs — huge and never contain protocol knowledge.
EXCLUDED_DIR_PARTS |= {"fonts"}

# The register-map header must never be included by the controller.
FORBIDDEN_INCLUDE = re.compile(r'#\s*include\s*[<"]\s*macon_registers\.h\s*[>"]')

# Macon register-name symbols (REG_FAULT, REG_TEMP_OUTLET, ...). The word
# boundary keeps this from matching MACON_FAULT_REGS / *_REGS_COUNT etc.
FORBIDDEN_REG_SYMBOL = re.compile(r"\bREG_[A-Z][A-Z0-9_]*")

# OEM fault-code string literals as shown on the mainboard LCD (P02, E19, r06,
# PC, ...). These are library-owned; the controller must reference codes only as
# opaque runtime strings, never bake them in.
FORBIDDEN_CODE_LITERAL = re.compile(r'"(?:[PE]\d{2}|r\d{2}|PC)"')


def _controller_sources():
    for path in MAIN.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if path.resolve() == THIS_FILE:
            continue
        rel_parts = path.relative_to(MAIN).parts
        if any(part in EXCLUDED_DIR_PARTS for part in rel_parts):
            continue
        if path.name in EXCLUDED_FILES:
            continue
        yield path


def _scan(pattern):
    violations = []
    for path in _controller_sources():
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in pattern.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            violations.append(f"{path.relative_to(ROOT)}:{line}: {m.group(0)}")
    return violations


def test_controller_does_not_include_register_map():
    violations = _scan(FORBIDDEN_INCLUDE)
    assert not violations, (
        "main/ must not include the Macon register-map header:\n"
        + "\n".join(violations)
    )


def test_controller_has_no_register_symbols():
    violations = _scan(FORBIDDEN_REG_SYMBOL)
    assert not violations, (
        "main/ must not reference REG_* register symbols (protocol leak):\n"
        + "\n".join(violations)
    )


def test_controller_has_no_oem_fault_code_literals():
    violations = _scan(FORBIDDEN_CODE_LITERAL)
    assert not violations, (
        "main/ must not hardcode OEM fault-code string literals (protocol leak):\n"
        + "\n".join(violations)
    )
