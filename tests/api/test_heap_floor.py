"""Pin the internal-heap-floor contract used by the device-tests CI gate.

``.github/scripts/scan_heap_floor.sh`` is the single source of truth for the
heap-headroom gate. It exists because internal (non-PSRAM) RAM exhaustion does
not present as a device fault -- it presents as a flaky network. See issue #234:
the device stopped completing TCP handshakes, and five tests across four
unrelated suites failed with ``ConnectTimeout`` / ``ConnectionError`` /
``ChunkedEncodingError`` / websocket ``TimeoutError``, plus a bogus "throttling
is broken" assertion (401 != 429) caused purely by a dropped connection. Every
symptom was indistinguishable from runner flake, and was dismissed as such more
than once.

THE SUBTLE PART, which these tests exist to protect:

netdiag reports two heap figures, and only one of them is a usable signal.

  * ``(min M)`` -- the since-boot low-water (``heap_caps_get_minimum_free_size``)
    -- is monotonic and never resets, so one brief early transient pins it near
    zero for the whole boot. It measured **16 bytes on 7 of 7 sampled runs**,
    passing and failing alike. Gating on it would fail every run and block the
    repository while distinguishing nothing.
  * ``heap int=N`` -- the instantaneous free internal heap -- separates cleanly:
    5 healthy runs stayed in 24847..32727 bytes, while the 2 wedged runs dropped
    to 23 bytes and also logged ``PCB walk TIMED OUT``.

``test_since_boot_low_water_alone_does_not_trip_the_gate`` pins that distinction
directly, because getting it wrong is the difference between a useful gate and
one that has to be switched off.
"""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / ".github" / "scripts" / "scan_heap_floor.sh"

# A comfortable run: instantaneous free stays ~40 KB (lowest 39880).
# Note the "(min ...)" values are deliberately low-ish -- that is realistic and
# must not by itself trip the gate.
HEALTHY_LINES = [
    "I (33395) netdiag: heap int=40615 (min 12632) psram=23673156 | tcp active=1 tw=0 bound=0 listen=4",
    "I (63840) netdiag: heap int=40279 (min 12120) psram=23607152 | tcp active=1 tw=3 bound=0 listen=4",
    "I (94001) netdiag: heap int=39880 (min 11788) psram=23600000 | tcp active=2 tw=5 bound=0 listen=4",
]

# Realistic "every run looks like this" samples: the since-boot low-water has
# already bottomed out at 16, yet the device is perfectly healthy because
# instantaneous free heap is still ~35 KB. Observed on all 7 sampled runs.
SATURATED_LOWWATER_LINES = [
    "I (338034) netdiag: heap int=39047 (min 16) psram=23620608 | tcp active=2 tw=0 bound=0 listen=4",
    "I (368496) netdiag: heap int=35023 (min 16) psram=23620608 | tcp active=2 tw=4 bound=0 listen=4",
]

# A genuine wedge: instantaneous free internal heap collapses below the floor.
# TIME_WAIT is saturated at 23 (CONFIG_LWIP_MAX_ACTIVE_TCP is 24).
EXHAUSTED_LINES = [
    "I (1309000) netdiag: heap int=12000 (min 16) psram=23670000 | tcp active=1 tw=19 bound=0 listen=4",
    "I (1339386) netdiag: heap int=512 (min 16) psram=23670940 | tcp active=1 tw=23 bound=0 listen=4",
]

# Verbatim from the issue #234 run: 23 bytes free and the lwIP tcpip thread
# unresponsive -- the same wedge observed from the network-stack side.
PCB_TIMEOUT_LINE = (
    "W (1581248) netdiag: heap int=23 (min 16) psram=23637360 "
    "| tcp PCB walk TIMED OUT (tcpip thread unresponsive)"
)

FLOOR = "8192"


def _write(tmp_path, name, lines):
    p = tmp_path / name
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return p


def test_script_exists():
    assert SCRIPT.is_file(), f"missing heap-floor scanner: {SCRIPT}"


def test_scanner_has_no_crlf_line_endings():
    """The scanner MUST be LF-only.

    CRLF silently breaks bash on the Linux runner (it errors on every ``\\r``),
    and a gate that errors out instead of evaluating protects nothing. This is
    the same failure that once disabled the crash gate, so it is pinned here as
    a plain byte check that runs on every platform.
    """
    raw = SCRIPT.read_bytes()
    assert b"\r" not in raw, "scan_heap_floor.sh contains CR bytes (CRLF) - bash on CI will break"


def test_workflow_invokes_the_shared_scanner():
    """CI must call this script rather than reimplementing the parsing inline,
    so the gate and its tests cannot drift apart."""
    wf = (ROOT / ".github" / "workflows" / "device-tests.yml").read_text(encoding="utf-8")
    assert "scan_heap_floor.sh" in wf, (
        "device-tests.yml does not invoke scan_heap_floor.sh - the heap gate is not wired up"
    )


_BASH = shutil.which("bash")
# As with the crash scanner: on Windows ``bash`` is usually the WSL launcher,
# which cannot accept Windows-style paths, so execute only on Linux CI.
_SKIP_BASH = _BASH is None or sys.platform.startswith("win")
requires_bash = pytest.mark.skipif(_SKIP_BASH, reason="bash-execution check runs on Linux CI only")


def _run(log, floor=FLOOR):
    return subprocess.run([_BASH, str(SCRIPT), str(log), floor], capture_output=True, text=True)


@requires_bash
def test_healthy_run_passes(tmp_path):
    r = _run(_write(tmp_path, "healthy.log", HEALTHY_LINES))
    assert r.returncode == 0, f"healthy log should pass; got {r.returncode}\n{r.stdout}{r.stderr}"
    assert "min-instantaneous-internal-heap=39880" in r.stdout, r.stdout


@requires_bash
def test_since_boot_low_water_alone_does_not_trip_the_gate(tmp_path):
    """The regression that would render this gate useless.

    ``(min 16)`` appeared on 7 of 7 sampled runs, healthy and wedged alike,
    because the since-boot low-water never resets. If the gate keys off that
    number it fails every device run and has to be disabled. Only the
    instantaneous figure may decide the outcome.
    """
    r = _run(_write(tmp_path, "saturated.log", SATURATED_LOWWATER_LINES))
    assert r.returncode == 0, (
        "a run whose since-boot low-water bottomed out at 16 but whose "
        f"instantaneous heap stayed ~35 KB must PASS; got {r.returncode}\n{r.stdout}{r.stderr}"
    )
    assert "since-boot-low-water=16" in r.stdout, r.stdout


@requires_bash
def test_exhausted_run_fails(tmp_path):
    """The whole point: instantaneous heap below the floor must fail loudly."""
    r = _run(_write(tmp_path, "exhausted.log", HEALTHY_LINES + EXHAUSTED_LINES))
    assert r.returncode == 1, f"exhausted log should fail; got {r.returncode}\n{r.stdout}{r.stderr}"
    assert "min-instantaneous-internal-heap=512" in r.stdout, r.stdout
    # The operator must be told this is not flake, and where to look.
    assert "Internal heap exhausted" in r.stderr, r.stderr
    assert "#234" in r.stderr, r.stderr
    # Peak TIME_WAIT points at the accumulation mechanism.
    assert "peak-TIME_WAIT=23" in r.stdout, r.stdout


@requires_bash
def test_pcb_walk_timeout_fails_even_with_healthy_heap(tmp_path):
    """A wedged tcpip thread is a failure on its own terms.

    The heap figure is raised above the floor here so that the timeout signal is
    the only thing that can trip the gate.
    """
    line = PCB_TIMEOUT_LINE.replace("heap int=23 (min 16)", "heap int=40000 (min 16)")
    r = _run(_write(tmp_path, "timeout.log", HEALTHY_LINES + [line]))
    assert r.returncode == 1, f"PCB-walk timeout should fail; got {r.returncode}\n{r.stdout}{r.stderr}"
    assert "tcpip thread unresponsive" in r.stderr, r.stderr


@requires_bash
def test_run_without_netdiag_samples_fails_closed(tmp_path):
    """No data is not the same as good data.

    A run that never emitted netdiag has not demonstrated headroom; passing here
    would recreate the silent "no evidence == healthy" bug.
    """
    log = _write(tmp_path, "nodata.log", ["I (100) boot: starting", "I (200) wifi: connected"])
    r = _run(log)
    assert r.returncode == 2, f"missing netdiag data should exit 2; got {r.returncode}\n{r.stdout}{r.stderr}"
    assert "No netdiag samples" in r.stderr, r.stderr


@requires_bash
def test_missing_log_fails_closed(tmp_path):
    r = _run(tmp_path / "absent.log")
    assert r.returncode == 2, f"missing log should exit 2; got {r.returncode}\n{r.stdout}{r.stderr}"


@requires_bash
def test_floor_is_configurable(tmp_path):
    """The same log passes or fails purely on the configured floor, so the
    threshold can be tightened as headroom improves."""
    log = _write(tmp_path, "healthy.log", HEALTHY_LINES)  # lowest instantaneous 39880
    assert _run(log, "8192").returncode == 0
    assert _run(log, "50000").returncode == 1


# The extended netdiag line, added with the 100ms low-heap profiler (issue #234),
# reports the largest contiguous free block alongside the low-water mark. The
# scanner's regex must tolerate BOTH forms. When it did not, every sample stopped
# matching and the gate failed closed with "no netdiag samples" -- which reads as
# a device fault but was really log-format drift. That cost a full CI cycle to
# diagnose, so it is pinned here.
EXTENDED_LINES = [
    "I (33395) netdiag: heap int=40615 (min 12632, largest 17408) psram=23673156 | tcp active=1 tw=0 bound=0 listen=4",
    "I (63840) netdiag: heap int=39880 (min 16, largest 11776) psram=23607152 | tcp active=1 tw=3 bound=0 listen=4",
]


@requires_bash
def test_extended_netdiag_format_is_still_parsed(tmp_path):
    r = _run(_write(tmp_path, "extended.log", EXTENDED_LINES))
    assert r.returncode == 0, (
        "the extended '(min M, largest L)' form must still be recognised; "
        f"got {r.returncode}\n{r.stdout}{r.stderr}"
    )
    assert "netdiag samples=2" in r.stdout, r.stdout
    assert "min-instantaneous-internal-heap=39880" in r.stdout, r.stdout
    assert "since-boot-low-water=16" in r.stdout, r.stdout


@requires_bash
def test_extended_format_still_trips_the_gate_when_exhausted(tmp_path):
    """Tolerating the new field must not accidentally stop the gate firing."""
    lines = EXTENDED_LINES + [
        "W (99000) netdiag: heap int=23 (min 16, largest 0) psram=23637360 | tcp active=1 tw=9 bound=0 listen=4",
    ]
    r = _run(_write(tmp_path, "extended_bad.log", lines))
    assert r.returncode == 1, f"exhausted extended log should fail; got {r.returncode}\n{r.stdout}{r.stderr}"
    assert "min-instantaneous-internal-heap=23" in r.stdout, r.stdout