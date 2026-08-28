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
import shutil
import subprocess
import sys
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
    "***ERROR*** A stack overflow in task arctic_demo_sync has been detected.",
    "abort() was called at PC 0x4008a1b2 on core 0",
    "assert failed: xQueueSemaphoreTake queue.c:1545 (pxQueue)",
    "CORRUPT HEAP: Bad head at 0x3fca1234. Expected 0xabba1234",
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


def test_scanner_has_no_crlf_line_endings():
    """The scanner MUST be LF-only. CRLF silently breaks bash on the Linux
    runner (bash errors on every ``\\r``), the failure gets swallowed, and the
    gate reports "no crash" for real crashes — an actual weeks-long outage.
    This byte check runs everywhere (no bash needed) and pins that exact bug.
    """
    raw = SCRIPT.read_bytes()
    assert b"\r" not in raw, "scan_crashes.sh contains CR bytes (CRLF) — bash on CI will break"


_BASH = shutil.which("bash")
# The point of this test is to catch a bash-syntax/CRLF break the way CI would.
# On Windows ``bash`` usually resolves to the WSL launcher, which can't take a
# Windows-style path — so restrict the executed check to the Linux CI runner.
_SKIP_BASH = _BASH is None or sys.platform.startswith("win")


@pytest.mark.skipif(_SKIP_BASH, reason="bash-execution check runs on Linux CI only")
def test_scanner_executes_under_bash(tmp_path):
    """Actually run the scanner under bash (as CI does), so a bash-syntax or
    CRLF break FAILS this test instead of silently disabling the gate.

    A crash log must exit 0; a benign log must exit 1. A broken script exits
    with neither (e.g. 2), tripping both assertions.
    """
    crash_log = tmp_path / "crash.log"
    crash_log.write_text("\n".join(BENIGN_LINES + CRASH_LINES) + "\n", encoding="utf-8")
    benign_log = tmp_path / "benign.log"
    benign_log.write_text("\n".join(BENIGN_LINES) + "\n", encoding="utf-8")

    crash = subprocess.run(
        [_BASH, str(SCRIPT), str(crash_log)], capture_output=True, text=True
    )
    assert crash.returncode == 0, (
        f"scanner should exit 0 when a crash is present; got {crash.returncode}\n"
        f"stdout={crash.stdout!r} stderr={crash.stderr!r}"
    )
    assert "Guru Meditation" in crash.stdout, f"crash line not reported: {crash.stdout!r}"

    benign = subprocess.run(
        [_BASH, str(SCRIPT), str(benign_log)], capture_output=True, text=True
    )
    assert benign.returncode == 1, (
        f"scanner should exit 1 when no crash is present; got {benign.returncode}\n"
        f"stdout={benign.stdout!r} stderr={benign.stderr!r}"
    )
