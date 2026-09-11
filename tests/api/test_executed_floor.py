"""Tests for the executed-test floor gate (`.github/scripts/check_test_floor.py`).

The gate exists because a suite can pass, and mint a release attestation, while
having executed almost nothing -- a conftest error, a bad marker expression, or
an environment precondition skipping wholesale. `device-tests.yml` previously
read the JUnit XML only to paint a badge.

The most important test here is
`test_every_skip_message_in_the_tree_is_classified`: it scans the actual
`pytest.skip()` calls in the suites and fails when a message is neither
recognised as an infrastructure precondition nor as a benign, data-dependent
condition. That forces the decision at review time instead of letting a new
"Device not reachable"-style skip quietly join the 47 skips the API suite
already reports.
"""

import importlib.util
import json
import re
import textwrap
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github" / "scripts" / "check_test_floor.py"
FLOORS = ROOT / "tests" / "executed_floor.json"
WORKFLOW = ROOT / ".github" / "workflows" / "device-tests.yml"


def _load_module():
    spec = importlib.util.spec_from_file_location("check_test_floor", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mod = _load_module()


def _write_xml(path: Path, *, tests: int, skipped: int, failures: int = 0, skip_messages=()):
    cases = []
    for message in skip_messages:
        cases.append(f'<testcase name="t{len(cases)}"><skipped message="{message}"/></testcase>')
    path.write_text(
        textwrap.dedent(
            f"""\
            <?xml version="1.0" encoding="utf-8"?>
            <testsuites>
              <testsuite name="pytest" tests="{tests}" failures="{failures}" errors="0" skipped="{skipped}">
                {''.join(cases)}
              </testsuite>
            </testsuites>
            """
        ),
        encoding="utf-8",
    )


def test_script_exists():
    assert SCRIPT.exists(), "check_test_floor.py is missing; the floor gate cannot run"


def test_workflow_invokes_the_gate():
    text = WORKFLOW.read_text(encoding="utf-8", errors="replace")
    assert "check_test_floor.py" in text, (
        "device-tests.yml does not invoke check_test_floor.py, so the floor is not enforced"
    )


def test_floors_file_is_valid_and_positive():
    floors = json.loads(FLOORS.read_text(encoding="utf-8"))
    assert floors, "executed_floor.json is empty; every suite would have a floor of 0"
    for suite, floor in floors.items():
        assert isinstance(floor, int) and floor > 0, f"floor for {suite} must be a positive int"


def test_healthy_run_passes(tmp_path):
    xml = tmp_path / "api.xml"
    _write_xml(xml, tests=454, skipped=47)
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 380}), encoding="utf-8")
    assert mod.main([f"api={xml}", "--floors", str(floors)]) == 0


def test_suite_below_floor_fails(tmp_path):
    """The headline case: pytest exits 0 having executed almost nothing."""
    xml = tmp_path / "api.xml"
    _write_xml(xml, tests=12, skipped=0)
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 380}), encoding="utf-8")
    assert mod.main([f"api={xml}", "--floors", str(floors)]) == 1


def test_wholesale_environment_skip_fails(tmp_path):
    """454 collected, 450 skipped: green today, caught now."""
    xml = tmp_path / "api.xml"
    _write_xml(xml, tests=454, skipped=450)
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 380}), encoding="utf-8")
    assert mod.main([f"api={xml}", "--floors", str(floors)]) == 1


def test_missing_xml_fails_closed(tmp_path):
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 380}), encoding="utf-8")
    assert mod.main([f"api={tmp_path / 'nope.xml'}", "--floors", str(floors)]) == 1


def test_unparseable_xml_fails_closed(tmp_path):
    xml = tmp_path / "api.xml"
    xml.write_text("<not valid xml", encoding="utf-8")
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 1}), encoding="utf-8")
    assert mod.main([f"api={xml}", "--floors", str(floors)]) == 1


def test_infra_skip_warns_by_default_and_fails_when_enforced(tmp_path):
    xml = tmp_path / "api.xml"
    _write_xml(xml, tests=454, skipped=1, skip_messages=["Device not reachable at https://x: timeout"])
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 380}), encoding="utf-8")

    assert mod.main([f"api={xml}", "--floors", str(floors)]) == 0
    assert mod.main([f"api={xml}", "--floors", str(floors), "--enforce-infra-skips"]) == 1


def test_benign_skip_never_fails_even_when_enforcing(tmp_path):
    xml = tmp_path / "api.xml"
    _write_xml(xml, tests=454, skipped=1, skip_messages=["No events on device"])
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 380}), encoding="utf-8")
    assert mod.main([f"api={xml}", "--floors", str(floors), "--enforce-infra-skips"]) == 0


def test_deferred_skip_never_fails_even_when_enforcing(tmp_path):
    # A tracked debt must not block main; it is visible in the report instead.
    xml = tmp_path / "api.xml"
    _write_xml(
        xml,
        tests=454,
        skipped=1,
        skip_messages=["Backup/aux heater is not mapped from any Tuya register yet"],
    )
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 380}), encoding="utf-8")
    assert mod.main([f"api={xml}", "--floors", str(floors), "--enforce-infra-skips"]) == 0


def test_report_separates_deferred_from_benign(tmp_path):
    # The whole point of the third class is that debts stay countable rather
    # than blending into the permanent-by-design pile.
    xml = tmp_path / "api.xml"
    _write_xml(
        xml,
        tests=454,
        skipped=2,
        skip_messages=[
            "Backup/aux heater is not mapped from any Tuya register yet",
            "dangerous endpoint: /login",
        ],
    )
    floors = tmp_path / "floors.json"
    floors.write_text(json.dumps({"api": 380}), encoding="utf-8")
    report = tmp_path / "report.json"
    assert mod.main([f"api={xml}", "--floors", str(floors), "--report", str(report)]) == 0

    data = json.loads(report.read_text(encoding="utf-8"))["api"]
    assert len(data["deferred_skips"]) == 1
    assert data["infra_skips"] == []
    assert data["unknown_skips"] == []


def test_deferred_takes_precedence_over_a_broader_benign_pattern():
    # Ordering inside classify_skip is load-bearing: if benign were checked
    # first, a debt could be absorbed by a more general pattern and disappear.
    # Construct a message that genuinely matches BOTH classes, otherwise this
    # test passes without ever exercising the ordering it claims to guard.
    both = "No events on device: backup/aux heater is not mapped from any Tuya register yet"
    assert any(rx.search(both) for rx in mod._BENIGN_RE), "fixture no longer matches a benign pattern"
    assert any(rx.search(both) for rx in mod._DEFERRED_RE), "fixture no longer matches a deferred pattern"
    assert mod.classify_skip(both) == "deferred"
    assert mod.DEFERRED_SKIP_PATTERNS, "the deferred class has been emptied"


@pytest.mark.parametrize(
    "message,expected",
    [
        ("Device not reachable at https://host: timeout", "infra"),
        ("Device unreachable at https://host", "infra"),
        ("ARCTIC_API_KEY not set", "infra"),
        ("Demo mode not enabled on device", "infra"),
        ("Device is not running in demo mode", "infra"),
        ("WiFi not connected", "infra"),
        ("Firmware not built with CONFIG_TEST_ENDPOINTS", "infra"),
        ("HTTPS not reachable", "infra"),
        ("No events on device", "benign"),
        ("Another OTA operation in progress - cannot test error state", "benign"),
        ("Need at least 2 entries to test ordering", "benign"),
        ("Regenerating the API key invalidates ARCTIC_API_KEY for the rest of the run", "benign"),
        # Deliberately unknown since #242: the OTA rollback stub these described
        # was removed once .github/workflows/ota-rollback.yml began exercising
        # rollback nightly on hardware. Pinned here so re-adding the patterns
        # without a matching stub is caught. Unknown only warns, never fails.
        ("Requires serial connection and device reboot", "unknown"),
        ("Poison firmware binary not yet available", "unknown"),
        ("Backup/aux heater is not mapped from any Tuya register yet", "deferred"),
        ("some brand new reason nobody classified", "unknown"),
        ("", "unknown"),
    ],
)
def test_classification(message, expected):
    assert mod.classify_skip(message) == expected


def test_every_skip_message_in_the_tree_is_classified():
    """A new skip reason must be consciously classified, not silently ignored.

    Without this, someone adds `pytest.skip("Device offline")`, it never matches
    an infra pattern, and the gate waves it through forever.
    """
    skip_re = re.compile(r"pytest\.skip\(\s*f?[\"']([^\"']{4,200})")
    unclassified = []
    for path in sorted((ROOT / "tests").rglob("*.py")):
        if path.resolve() == Path(__file__).resolve():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in skip_re.finditer(text):
            message = match.group(1)
            # Strip f-string placeholders; they carry no classifiable text.
            literal = re.sub(r"\{[^}]*\}", "", message).strip()
            if not literal:
                continue
            if mod.classify_skip(literal) == "unknown":
                unclassified.append(f"{path.relative_to(ROOT).as_posix()}: {message}")

    assert not unclassified, (
        "Unclassified pytest.skip() messages found. Add each to INFRA_SKIP_PATTERNS "
        "(the harness could not test what it was asked to) or BENIGN_SKIP_PATTERNS "
        "(the case genuinely does not apply) in .github/scripts/check_test_floor.py:\n  "
        + "\n  ".join(unclassified)
    )
