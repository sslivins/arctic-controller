"""Stack high-water-mark ratchet: catch shrinking task stack headroom before it
overflows into a crash.

The ``arctic_demo_sync`` stack overflow (fixed in #174) shipped green for weeks:
~3 KB of on-stack fault buffers grew until a full fault-set run overflowed the
4 KB task stack, and nothing measured how close each task was to the edge. This
gate closes that hole.

At device-session end the suite queries ``/api/test/stack-watermarks`` — every
task's ``uxTaskGetStackHighWaterMark`` (the smallest amount of stack, in bytes,
that task has ever had free since creation) after every screen/API path has run.
The device session flushes it to ``tests/.stack-watermarks.json`` and this
hostside gate enforces two things:

1. **Absolute floor (always on):** every task present must have more than
   ``ABSOLUTE_FLOOR_BYTES`` free. This is the safety net that would have caught
   the demo-sync overflow directly (free stack fell to 0). It needs no baseline,
   so it protects even brand-new or transient tasks.

2. **Per-task ratchet (opt-in via baseline):** ``tests/stack_watermark_budget.json``
   records a committed minimum free-stack floor per long-lived task. If a task's
   headroom drops below its floor — i.e. a change started eating its stack — the
   gate fails, naming the task and the delta, *before* it can overflow.

Like the sleep/latency budgets it is a ratchet: when stack usage legitimately
changes, regenerate the baseline from a real device run's snapshot:

    python tests/api/test_stack_watermark_budget.py --write        # from tests/.stack-watermarks.json
    python tests/api/test_stack_watermark_budget.py --write path/to/snapshot.json

and commit the updated ``tests/stack_watermark_budget.json`` alongside the change
so the floor only ever moves deliberately.

The synthetic contract tests below run hostside (no device); the real-snapshot
check runs whenever a snapshot file is present — i.e. in the api-suite process of
device CI, right after the device suite produced it.
"""

import json
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = ROOT / "tests"
BUDGET_PATH = TESTS_DIR / "stack_watermark_budget.json"
SNAPSHOT_PATH = TESTS_DIR / ".stack-watermarks.json"
ESSENTIAL_PATH = TESTS_DIR / "essential_tasks.json"

# A real device snapshot enumerates every FreeRTOS task. Far fewer than this
# means the snapshot was truncated or the endpoint returned a stub, in which
# case every headroom check below is vacuously satisfied.
MIN_PLAUSIBLE_TASK_COUNT = 12

# Every live task must keep strictly more than this many bytes of stack free.
# This is an UNCONDITIONAL danger floor (applies to every task, even system/IDLE
# tasks we don't control and tasks with no committed baseline), so it is set
# conservatively low: no healthy task realistically runs this close to the edge,
# yet it still catches a genuine near-overflow — the demo-sync overflow drove
# free stack to 0. Earlier, per-task warning comes from the ratchet below once a
# baseline is committed; this floor is just the "never this close to overflow"
# safety net, kept low to avoid false-positiving an uncontrolled system task.
ABSOLUTE_FLOOR_BYTES = 256

# When (re)writing the baseline, a task's committed floor is this fraction of its
# observed free stack — so a task must lose >40% of its measured headroom before
# the ratchet fails. Tolerant of run-to-run path differences, strict enough to
# flag a real regression (the demo-sync task's headroom collapsed to 0).
RATCHET_FRACTION = 0.6


def load_json(path: Path) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def check(snapshot: dict, budget: dict) -> list:
    """Return a list of human-readable regression messages (empty == pass).

    Fails a task when it violates the absolute floor OR (if it has a committed
    baseline) drops below its ratchet floor. Tasks present but unbudgeted are
    still held to the absolute floor; budgeted tasks absent from the snapshot
    are ignored here (reported separately by the CLI) since transient/renamed
    tasks must not red an otherwise-healthy run.
    """
    regressions = []
    for task in sorted(snapshot):
        free = snapshot[task]
        if free < ABSOLUTE_FLOOR_BYTES:
            regressions.append(
                f"  {task}: {free} B free < absolute floor {ABSOLUTE_FLOOR_BYTES} B "
                f"(task is close to stack overflow)"
            )
            continue  # absolute-floor failure subsumes any ratchet check
        floor = budget.get(task)
        if floor is not None and free < floor:
            regressions.append(
                f"  {task}: {free} B free < baseline floor {floor} B "
                f"(stack headroom shrank)"
            )
    return regressions


def essential_tasks(path: Path = ESSENTIAL_PATH) -> list:
    """Tasks whose absence from a run is itself a failure (see the JSON comment)."""
    return sorted(load_json(path).get("tasks", []))


def missing_essential(snapshot: dict, essential=None) -> list:
    """Return essential tasks absent from ``snapshot``.

    This is the counterpart to :func:`check`. ``check`` can only judge tasks it
    can see, so a task that crashed at startup or was never created presents as
    infinite headroom and sails through. Watching headroom without also watching
    liveness measures the wrong thing precisely when something has gone wrong.
    """
    if essential is None:
        essential = essential_tasks()
    return [task for task in essential if task not in snapshot]


def _write_budget(snapshot: dict, path: Path = BUDGET_PATH) -> dict:
    """Persist ``{task: floor_bytes}`` = RATCHET_FRACTION of observed free."""
    budget = {
        task: max(ABSOLUTE_FLOOR_BYTES, int(free * RATCHET_FRACTION))
        for task, free in snapshot.items()
    }
    path.write_text(
        json.dumps(budget, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return budget


# --------------------------------------------------------------------------- #
# Synthetic contract tests — hostside, no device required.
# --------------------------------------------------------------------------- #

def test_absolute_floor_flags_near_overflow():
    # A task with 0 B free (the demo-sync overflow condition) must fail even
    # with no baseline committed.
    msgs = check({"arctic_demo_sync": 0}, {})
    assert len(msgs) == 1 and "absolute floor" in msgs[0]


def test_absolute_floor_allows_healthy_task():
    assert check({"arctic_demo_sync": 1500}, {}) == []


def test_ratchet_flags_shrinking_headroom():
    # Above the absolute floor but below its committed baseline -> regression.
    msgs = check({"lvgl_task": 900}, {"lvgl_task": 1200})
    assert len(msgs) == 1 and "baseline floor" in msgs[0]


def test_ratchet_allows_headroom_at_or_above_floor():
    assert check({"lvgl_task": 1200}, {"lvgl_task": 1200}) == []
    assert check({"lvgl_task": 4000}, {"lvgl_task": 1200}) == []


def test_unbudgeted_task_still_held_to_absolute_floor():
    # A transient task with no baseline still must clear the absolute floor.
    assert check({"history_query": 100}, {}) != []
    assert check({"history_query": 5000}, {}) == []


def test_missing_budgeted_task_is_not_a_failure():
    # check() deliberately ignores absent tasks; liveness is enforced separately
    # by missing_essential() so that transient/renamed tasks cannot red a run
    # while a genuinely dead task still does.
    assert check({"main": 2000}, {"main": 1000, "ota_task": 1500}) == []


def test_empty_snapshot_never_fails_the_headroom_check():
    # ...but it must fail the liveness check, which is the point of T15.
    assert check({}, {"main": 1000}) == []
    assert missing_essential({}) == essential_tasks()


def test_missing_essential_task_is_reported():
    essential = ["main", "taskLVGL", "httpd"]
    snapshot = {"main": 2000, "httpd": 5000}
    assert missing_essential(snapshot, essential) == ["taskLVGL"]


def test_all_essential_tasks_present_passes_liveness():
    essential = ["main", "taskLVGL"]
    assert missing_essential({"main": 2000, "taskLVGL": 6000, "extra": 900}, essential) == []


def test_dead_task_would_otherwise_look_perfectly_healthy():
    """The exact failure mode this closes: absence reads as infinite headroom."""
    essential = ["main", "arctic_demo_syn"]
    snapshot = {"main": 2000}  # arctic_demo_syn crashed on startup
    assert check(snapshot, {"main": 1000, "arctic_demo_syn": 1557}) == []
    assert missing_essential(snapshot, essential) == ["arctic_demo_syn"]


def test_essential_task_list_is_populated_and_budgeted():
    """Guards against a typo'd or renamed task silently dropping out of scope."""
    essential = essential_tasks()
    assert len(essential) >= 10, (
        f"only {len(essential)} essential task(s) listed; the liveness check has "
        "lost most of its coverage"
    )
    budget = load_json(BUDGET_PATH)
    unknown = [task for task in essential if task not in budget]
    assert not unknown, (
        "essential task(s) do not appear in the committed stack budget, so they "
        "have most likely been renamed or misspelled and would report as "
        f"permanently dead:\n  " + "\n  ".join(unknown)
    )


def test_write_budget_applies_ratchet_fraction(tmp_path):
    snap = {"arctic_demo_sync": 2000, "tiny": 300}
    out = tmp_path / "budget.json"
    budget = _write_budget(snap, out)
    assert budget["arctic_demo_sync"] == 1200          # 2000 * 0.6
    assert budget["tiny"] == ABSOLUTE_FLOOR_BYTES       # 300*0.6=180 -> clamped to 256
    # And the freshly written baseline passes against its own snapshot.
    assert check(snap, budget) == []


# --------------------------------------------------------------------------- #
# Real-snapshot enforcement — runs when a device run has produced a snapshot
# (i.e. the api-suite process in device CI, after the device suite flushed).
# --------------------------------------------------------------------------- #

@pytest.mark.skipif(not SNAPSHOT_PATH.exists(),
                    reason="no device stack-watermark snapshot on disk (hostside/local run)")
def test_recorded_stack_headroom_within_budget():
    snapshot = load_json(SNAPSHOT_PATH)
    budget = load_json(BUDGET_PATH)
    regressions = check(snapshot, budget)
    hint = ("A task's stack headroom dropped below its floor. If this is a real, "
            "reviewed change, regenerate the baseline from this run's snapshot:  "
            "python tests/api/test_stack_watermark_budget.py --write  and commit "
            "tests/stack_watermark_budget.json.")
    assert not regressions, (
        "Stack headroom regressions detected:\n" + "\n".join(regressions) + "\n\n" + hint
    )


@pytest.mark.skipif(not SNAPSHOT_PATH.exists(),
                    reason="no device stack-watermark snapshot on disk (hostside/local run)")
def test_recorded_snapshot_is_plausibly_complete():
    """A truncated snapshot makes every other assertion in this module vacuous."""
    snapshot = load_json(SNAPSHOT_PATH)
    assert len(snapshot) >= MIN_PLAUSIBLE_TASK_COUNT, (
        f"snapshot lists only {len(snapshot)} task(s) "
        f"(expected >= {MIN_PLAUSIBLE_TASK_COUNT}): "
        f"{sorted(snapshot)}\nThe stack-watermark endpoint probably returned a "
        "partial or stub response, in which case the headroom and liveness "
        "checks below prove nothing."
    )


@pytest.mark.skipif(not SNAPSHOT_PATH.exists(),
                    reason="no device stack-watermark snapshot on disk (hostside/local run)")
def test_essential_tasks_were_alive():
    snapshot = load_json(SNAPSHOT_PATH)
    missing = missing_essential(snapshot)
    assert not missing, (
        "Task(s) that must be running were absent from the device's task list:\n  "
        + "\n  ".join(missing)
        + "\n\nA task that crashed at startup or was never created reports no "
        "stack usage at all, so the headroom budget passes it. If a task was "
        "renamed or is legitimately conditional, update "
        f"{ESSENTIAL_PATH.relative_to(ROOT).as_posix()} deliberately.\n"
        f"Tasks seen this run: {sorted(snapshot)}"
    )


if __name__ == "__main__":
    import sys

    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    snapshot_path = Path(args[0]) if args else SNAPSHOT_PATH

    if not snapshot_path.exists():
        print(f"No snapshot file at {snapshot_path} — nothing to do.")
        sys.exit(0)

    snapshot = load_json(snapshot_path)

    if "--write" in sys.argv:
        budget = _write_budget(snapshot)
        print(f"Wrote {BUDGET_PATH.relative_to(ROOT).as_posix()}: "
              f"{len(budget)} task floor(s) from {len(snapshot)} task(s).")
    else:
        budget = load_json(BUDGET_PATH)
        print(json.dumps(snapshot, indent=2, sort_keys=True))
        regressions = check(snapshot, budget)
        if regressions:
            print("\nREGRESSIONS:\n" + "\n".join(regressions))
            sys.exit(1)
        print("\nWithin budget.")
