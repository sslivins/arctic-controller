"""Guard the opaque-Macon boundary in the controller (main/).

The controller must carry ZERO Tuya/Macon protocol knowledge: no register-map
header, no REG_* register symbols, and no OEM fault-code string literals. All of
that lives behind the arctic-macon library's typed/opaque APIs.

Scope of the gate is the controller sources under ``main/`` only, excluding:
  * ``main/tuya/`` — the Macon master transport shim (relocated into the library
    in a later phase; not part of the opaque-consumer contract yet).
  * ``advanced_params.{cpp,h}`` — the advanced-parameters *quarantine wrapper*.
    It is the sole controller file allowed to touch raw AP registers/wire codes,
    translating the library's opaque option ids into wire writes (#118).
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

# Macon wire register NUMBERS named in code/comments/strings (reg2003, reg 2094,
# register 2096, regs 2000, ...). The controller must describe values
# semantically and never reference a wire register address. The 2000/2100
# ranges are the Macon holding/telemetry windows.
FORBIDDEN_REG_NUMBER = re.compile(r"(?i)reg(?:ister)?s?\s*2[01]\d\d")

# Any #include "name" / <name>; group 1 is the raw included path.
INCLUDE_RE = re.compile(r'#\s*include\s*[<"]\s*([A-Za-z0-9_./]+)\s*[>"]')

# Advanced-parameter wire/register metadata that must stay behind the library's
# opaque option-id API (#118). In-scope consumers (api_server.cpp,
# heatpump_params_screen.cpp, web/index.html handling) must consume stable option
# ids + semantic accessors, never the raw AdvancedParam.reg register address, the
# AdvEnumOption.wire RS485 code, the enum_vals/enum_count arrays, or the
# ADV_REG_UNKNOWN sentinel. The advanced_params.{cpp,h} quarantine wrapper is the
# sole excluded consumer (see EXCLUDED_FILES) — it owns the id<->wire mapping.
FORBIDDEN_ADV_WIRE_META = re.compile(
    r"\benum_vals\b|\benum_count\b|(?:->|\.)wire\b|(?:->|\.)reg\b|\bADV_REG_UNKNOWN\b"
)

# The arctic-macon library's public headers.
MACON_INCLUDE = ROOT / "components" / "arctic-macon" / "include"


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


def test_controller_has_no_register_number_references():
    violations = _scan(FORBIDDEN_REG_NUMBER)
    assert not violations, (
        "main/ must not reference Macon wire register numbers in code, comments,\n"
        "or strings (protocol leak) — describe the value semantically instead:\n"
        + "\n".join(violations)
    )


def test_controller_has_no_advanced_param_wire_metadata():
    violations = _scan(FORBIDDEN_ADV_WIRE_META)
    assert not violations, (
        "main/ must not read advanced-parameter wire/register metadata "
        "(AdvancedParam.reg, AdvEnumOption.wire, enum_vals/enum_count, "
        "ADV_REG_UNKNOWN). Consume the opaque option-id API "
        "(advanced_enum_option_index_for_wire / advanced_param_write_option) and "
        "semantic accessors (advanced_param_reg_known, advanced_register_address) "
        "instead (#118). The advanced_params.{cpp,h} quarantine wrapper is the "
        "only file allowed to touch these:\n" + "\n".join(violations)
    )


def _headers_included_by_main():
    """Header basenames directly #included by in-scope controller sources."""
    names = set()
    for path in _controller_sources():
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in INCLUDE_RE.finditer(text):
            names.add(Path(m.group(1)).name)
    return names


def _header_pulls_register_map(header, seen):
    """True if `header` includes macon_registers.h, directly or transitively
    through other arctic-macon public headers."""
    if header in seen or not header.is_file():
        return False
    seen.add(header)
    text = header.read_text(encoding="utf-8", errors="replace")
    if FORBIDDEN_INCLUDE.search(text):
        return True
    for m in INCLUDE_RE.finditer(text):
        nxt = MACON_INCLUDE / Path(m.group(1)).name
        if _header_pulls_register_map(nxt, seen):
            return True
    return False


def test_main_facing_macon_headers_do_not_pull_register_map():
    """A `main/` source that never directly includes macon_registers.h can still
    compile it in transitively through a public library header (e.g. an image or
    state header). Close that compile-boundary hole too."""
    violations = []
    for name in sorted(_headers_included_by_main()):
        if name == "macon_registers.h":
            continue  # a direct include is covered by the dedicated test
        header = MACON_INCLUDE / name
        if header.is_file() and _header_pulls_register_map(header, set()):
            violations.append(name)
    assert not violations, (
        "main/ transitively compiles the Macon register map through these "
        "public headers — remove the macon_registers.h dependency from them:\n"
        + "\n".join(violations)
    )
