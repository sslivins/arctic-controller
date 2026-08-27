"""Pin the crash-detection contract used by the device-tests CI gate.

``.github/scripts/scan_crashes.sh`` is the single source of truth for which
serial-log lines count as a genuine firmware crash. Device/API tests
deliberately SEED event-log entries named ``application_crash``,
``watchdog_reset`` and ``brownout_reset`` to exercise the event-log UI — those
are journalled events, not crashes, and must NEVER trip the gate (matching them
once failed an otherwise-clean run). Conversely a real ``Guru Meditation`` panic
or a boot_stats ``Crash reboot (...)`` MUST trip it.

Rather than duplicate the regex (which would drift), this test *extracts* the
canonical ``CRASH_RE`` from the shell script and applies it in pure Python, so
it is portable (no bash needed) and can't diverge from what CI actually runs.
"""

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github" / "scripts" / "scan_crashes.sh"

# Serial-log lines tests legitimately produce (seeded event-log entries and
# ordinary clean reboots) — none of these may be treated as a crash.
BENIGN_LINES = [
    "I (12345) event_log: journaled event application_crash",
    "I (12346) event_log: journaled event watchdog_reset",
    "I (12347) event_log: journaled event brownout_reset",
    'I (12348) test_endpoints: seeding event {"type":"application_crash"}',
    "I (12349) boot_stats: reboot reason SW_CPU_RESET (clean)",
    "I (12350) ota: intentional reboot requested via /api/ota/reboot",
]

# Serial-log lines that are genuine, hardware-derived crash evidence.
CRASH_LINES = [
    "Guru Meditation Error: Core 0 panic'ed (Load access fault).",
    "I (900) boot_stats: Crash reboot (Panic) - consecutive crash streak now 1",
    "I (901) boot_stats: Crash reboot (Task watchdog)",
]


def _canonical_crash_regex() -> str:
    """Extract ``CRASH_RE`` from the shared scanner (single source of truth)."""
    text = SCRIPT.read_text(encoding="utf-8")
    m = re.search(r"^CRASH_RE='([^']*)'", text, re.MULTILINE)
    assert m, "could not find CRASH_RE='...' in scan_crashes.sh"
    return m.group(1)


def test_script_exists_and_applies_the_canonical_regex():
    assert SCRIPT.is_file(), f"missing crash scanner: {SCRIPT}"
    # The regex we test is the one the script actually greps with.
    assert 'grep -nE "$CRASH_RE"' in SCRIPT.read_text(encoding="utf-8")


def test_seeded_event_log_entries_are_not_crashes():
    """Seeded application_crash/watchdog_reset/brownout_reset must not match."""
    rx = re.compile(_canonical_crash_regex())
    matched = [ln for ln in BENIGN_LINES if rx.search(ln)]
    assert matched == [], f"seeded/benign lines were flagged as crashes: {matched}"


def test_real_crash_signatures_are_detected():
    """Guru Meditation and boot_stats 'Crash reboot (' must match."""
    rx = re.compile(_canonical_crash_regex())
    missed = [ln for ln in CRASH_LINES if not rx.search(ln)]
    assert missed == [], f"real crash signatures were not detected: {missed}"


def test_mixed_log_reports_only_crash_lines():
    """With seeded events AND a real crash present, only crashes are reported."""
    rx = re.compile(_canonical_crash_regex())
    hits = [ln for ln in (BENIGN_LINES + CRASH_LINES) if rx.search(ln)]
    assert sorted(hits) == sorted(CRASH_LINES)
