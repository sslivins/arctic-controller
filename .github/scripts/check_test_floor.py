#!/usr/bin/env python3
"""Guard against a test suite passing while it barely ran.

Two independent failure modes are covered, both of which produce a GREEN run
today:

1. **Collapse in executed tests.** A conftest error, a bad marker expression, a
   collection failure, or a wholesale environment skip can reduce a suite from
   hundreds of tests to a handful while pytest still exits 0. `device-tests.yml`
   previously parsed the JUnit XML only to paint a badge, so nothing noticed.
   Each suite therefore carries a floor in `tests/executed_floor.json`.

2. **Infrastructure preconditions skipping silently.** The suites call
   `pytest.skip()` for two very different reasons, and conflating them is what
   makes this dangerous:

     - *benign*: the case genuinely does not apply to this device right now
       ("No events on device", "Another OTA operation in progress").
     - *infrastructure*: the harness could not test what it was asked to test
       ("Device not reachable", "ARCTIC_API_KEY not set", "Demo mode not
       enabled"). In CI that is a broken run wearing a green tick. Locally it is
       normal and must stay quiet.

   This is the same class of bug #214 fixed for schemathesis (every case
   silently skipping while the job stayed green); the guard was never
   generalised. Here it is generalised.

Exit status is 0 on success and 1 on violation. A missing or unparseable XML for
a suite that was expected to run is a FAILURE, not a pass: absence of evidence
is not evidence of a passing suite.

Usage:
    check_test_floor.py [--floors PATH] [--enforce-infra-skips]
                        [--report PATH] ui=test-results.xml api=...
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# --------------------------------------------------------------------------
# Skip classification.
#
# Matched case-insensitively against the skip message as recorded in the JUnit
# XML. Kept as a single source of truth so the workflow gate and the hostside
# test that audits skip messages at source level cannot drift apart.
#
# Anything unmatched is UNKNOWN, deliberately: a new skip reason must be
# consciously classified rather than defaulting into silence. The hostside test
# `tests/api/test_executed_floor.py` fails when an unclassified `pytest.skip()`
# message appears in the tree, so the decision is forced at review time rather
# than discovered during an incident.
# --------------------------------------------------------------------------

INFRA_SKIP_PATTERNS = [
    r"device (?:is )?(?:not |un)reachable",
    r"device not in demo mode",
    r"device is not running in demo mode",
    r"demo mode not enabled",
    r"arctic_api_key not set",
    r"arctic_username\s*/\s*arctic_password not set",
    r"auth required\b",
    r"api auth enabled\b",
    r"api auth required but key not accepted",
    r"wifi not connected",
    r"firmware not built with config_test_endpoints",
    r"device firmware does not expose test instrumentation",
    r"https not (?:active|reachable)",
    r"firmware binary not found",
    r"pending_verify not in firmware",
]

BENIGN_SKIP_PATTERNS = [
    r"^dangerous endpoint",
    r"^mutating[: ]",
    r"undocumented 401",
    r"filtered run",
    r"no events on device",
    r"no log entries on device",
    r"need at least \d+ entries",
    r"no writable ap parameters available",
    r"device cannot reach github",
    r"another ota operation in progress",
    r"download failed too quickly",
    r"poison firmware binary not yet available",
    r"live weather reading",
    # Self-invalidating by design: the call under test destroys the credential
    # the rest of the run depends on, so it can only be exercised in isolation.
    r"invalidates arctic_api_key",
]

# Deliberate, tracked gaps: the test is skipped because the feature or the
# harness capability genuinely does not exist yet, not because anything broke.
# Separated from BENIGN because these are debts with an owner -- each one is
# referenced from an issue -- whereas a benign skip is permanent by design.
#
# Evidence for the entries below (device run 34306377818, the first run to
# publish the skip itemisation): 48 API skips, of which 0 were infra.
DEFERRED_SKIP_PATTERNS = [
    # OTA rollback Tiers 2/3 -- #217 T05/T06. The runner does in fact have USB
    # serial access to the controller, so "not available on CI" understates it:
    # the harness has not been built. Tracked rather than silently tolerated.
    r"requires serial connection",
    # Aux/backup heater has no Tuya register mapping yet, so the reading is
    # hardcoded false; the test is real and will matter once it is mapped.
    r"not mapped from any tuya register yet",
]

_INFRA_RE = [re.compile(p, re.I) for p in INFRA_SKIP_PATTERNS]
_BENIGN_RE = [re.compile(p, re.I) for p in BENIGN_SKIP_PATTERNS]
_DEFERRED_RE = [re.compile(p, re.I) for p in DEFERRED_SKIP_PATTERNS]


def classify_skip(message: str) -> str:
    """Return 'infra', 'deferred', 'benign' or 'unknown' for a skip message."""
    text = (message or "").strip()
    if not text:
        return "unknown"
    for rx in _INFRA_RE:
        if rx.search(text):
            return "infra"
    # Checked before benign: a deferred reason is the more specific statement
    # and must not be absorbed by a broader benign pattern.
    for rx in _DEFERRED_RE:
        if rx.search(text):
            return "deferred"
    for rx in _BENIGN_RE:
        if rx.search(text):
            return "benign"
    return "unknown"


class SuiteResult:
    def __init__(self, name: str, path: Path):
        self.name = name
        self.path = path
        self.executed = 0
        self.skipped = 0
        self.failures = 0
        self.skip_messages: list[str] = []
        self.error: str | None = None

    @property
    def infra_skips(self) -> list[str]:
        return [m for m in self.skip_messages if classify_skip(m) == "infra"]

    @property
    def unknown_skips(self) -> list[str]:
        return [m for m in self.skip_messages if classify_skip(m) == "unknown"]

    @property
    def deferred_skips(self) -> list[str]:
        return [m for m in self.skip_messages if classify_skip(m) == "deferred"]


def parse_suite(name: str, path: Path) -> SuiteResult:
    result = SuiteResult(name, path)
    if not path.exists():
        result.error = f"no JUnit XML at {path}"
        return result
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        result.error = f"unparseable JUnit XML at {path}: {exc}"
        return result

    suites = root.findall("testsuite") if root.tag == "testsuites" else [root]
    total = skipped = failures = 0
    for suite in suites:
        total += int(suite.get("tests", 0))
        skipped += int(suite.get("skipped", 0))
        failures += int(suite.get("failures", 0)) + int(suite.get("errors", 0))

    for case in root.iter("testcase"):
        for skip in case.findall("skipped"):
            result.skip_messages.append(skip.get("message", "") or (skip.text or ""))

    result.executed = total - skipped
    result.skipped = skipped
    result.failures = failures
    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("suites", nargs="+", metavar="NAME=PATH")
    ap.add_argument("--floors", default="tests/executed_floor.json")
    ap.add_argument(
        "--enforce-infra-skips",
        action="store_true",
        help="Fail on infrastructure-class skips instead of only warning.",
    )
    ap.add_argument("--report", help="Write a JSON summary here.")
    args = ap.parse_args(argv)

    floors_path = Path(args.floors)
    floors = json.loads(floors_path.read_text(encoding="utf-8")) if floors_path.exists() else {}

    problems: list[str] = []
    warnings: list[str] = []
    report: dict = {}

    for spec in args.suites:
        if "=" not in spec:
            print(f"::error::malformed suite argument {spec!r}, expected NAME=PATH")
            return 1
        name, _, raw_path = spec.partition("=")
        res = parse_suite(name, Path(raw_path))

        if res.error:
            # Fail closed. A suite whose results we cannot read has not been
            # shown to pass, and this gate exists precisely to stop "no
            # evidence" from reading as "no problem".
            problems.append(f"{name}: {res.error}")
            report[name] = {"error": res.error}
            continue

        floor = int(floors.get(name, 0))
        report[name] = {
            "executed": res.executed,
            "skipped": res.skipped,
            "failures": res.failures,
            "floor": floor,
            "infra_skips": res.infra_skips,
            "deferred_skips": res.deferred_skips,
            "unknown_skips": res.unknown_skips,
        }

        print(
            f"{name}: executed={res.executed} skipped={res.skipped} "
            f"failures={res.failures} floor={floor}"
        )

        if res.executed < floor:
            problems.append(
                f"{name}: only {res.executed} tests executed, floor is {floor}. "
                f"The suite ran but barely tested anything - check for collection "
                f"errors or a wholesale environment skip."
            )

        if res.infra_skips:
            uniq = sorted(set(res.infra_skips))
            detail = "; ".join(uniq[:8])
            msg = (
                f"{name}: {len(res.infra_skips)} infrastructure-precondition "
                f"skip(s) fired: {detail}"
            )
            (problems if args.enforce_infra_skips else warnings).append(msg)

        if res.unknown_skips:
            uniq = sorted(set(res.unknown_skips))
            warnings.append(
                f"{name}: {len(res.unknown_skips)} unclassified skip(s): "
                f"{'; '.join(uniq[:8])}"
            )

    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    for warning in warnings:
        print(f"::warning title=Test floor::{warning}")

    if problems:
        for problem in problems:
            print(f"::error title=Test floor::{problem}")
        return 1

    print("OK: every suite cleared its executed-test floor.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
