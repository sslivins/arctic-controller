"""Latency-budget ratchet: catch UI/firmware slowdowns that generous timeouts hide.

The condition-based waits (``DeviceClient.wait_until`` / ``wait_for_screen`` /
``wait_for_widget``) that replaced fixed ``time.sleep`` calls succeed as soon
as the device is ready — great for correctness and speed. But their generous
``timeout`` is a *correctness* deadline, not a performance one: if an LVGL
upgrade (or any firmware change) made every screen transition twice as slow,
every wait would still succeed well under its timeout and the suite would stay
green while silently regressing.

This gate closes that hole. Every successful wait records its elapsed time
(``DeviceClient.latency_samples``); the device session flushes them to
``tests/.latency-samples.json``. Here we aggregate the **p95 per operation**
(screens/widgets bucketed by a normalized key) and compare against the committed
baseline ``tests/latency_budget.json`` with a tolerance. A broad slowdown trips
the gate as a real CI failure, naming the operation and the delta.

It is a **ratchet**, like the sleep budget: when latencies legitimately change
(a real speedup, or a justified, reviewed slowdown), regenerate the baseline
from a real device run's samples:

    python tests/api/test_latency_budget.py --write        # from tests/.latency-samples.json
    python tests/api/test_latency_budget.py --write path/to/samples.json

and commit the updated ``tests/latency_budget.json`` alongside the change so the
ceiling only ever moves deliberately.

The synthetic contract tests below run hostside (no device); the real-samples
check runs whenever a samples file is present — i.e. in the api-suite process of
device CI, right after the device suite produced it.
"""

import json
import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = ROOT / "tests"
BUDGET_PATH = TESTS_DIR / "latency_budget.json"
SAMPLES_PATH = TESTS_DIR / ".latency-samples.json"

# A regression must clear BOTH a relative and an absolute margin over baseline
# to fail — so tiny, noisy operations aren't tripped by millisecond jitter, and
# large operations get proportional headroom for CI load. allowed = max(
# baseline * TOLERANCE_RATIO, baseline + TOLERANCE_FLOOR_S).
TOLERANCE_RATIO = 1.5
TOLERANCE_FLOOR_S = 0.20

# Don't enforce an operation until we have enough samples for p95 to be stable.
MIN_SAMPLES = 5

# Strip quoted literals ('settings', tag='foo') so per-screen/per-widget calls
# collapse into a stable, low-cardinality operation bucket. A global slowdown
# then shows up as one bucket's p95 rising, robust to which screens ran.
_QUOTED = re.compile(r"'[^']*'|\"[^\"]*\"")


def normalize_op(description: str) -> str:
    """Bucket a wait description into a stable operation key.

    ``"settled screen 'settings'"`` -> ``"settled screen"``
    ``"widget tag='reboot'"``       -> ``"widget tag"``
    ``"device to finish reboot"``   -> ``"device to finish reboot"`` (unchanged)
    """
    stripped = _QUOTED.sub("", description)
    return re.sub(r"\s+", " ", stripped).strip().rstrip(" =")


def percentile(values, pct: float) -> float:
    """Nearest-rank percentile of ``values`` (0 <= pct <= 100)."""
    if not values:
        raise ValueError("percentile of empty sequence")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    # Nearest-rank: rank = ceil(pct/100 * N), 1-based, clamped to [1, N].
    import math
    rank = max(1, min(len(ordered), math.ceil(pct / 100.0 * len(ordered))))
    return ordered[rank - 1]


def aggregate(samples) -> dict:
    """Aggregate ``[description, elapsed]`` pairs into ``{op: {"p95", "n"}}``."""
    buckets: dict = {}
    for description, elapsed in samples:
        buckets.setdefault(normalize_op(description), []).append(float(elapsed))
    return {
        op: {"p95": round(percentile(vals, 95), 4), "n": len(vals)}
        for op, vals in buckets.items()
    }


def load_budget(path: Path = BUDGET_PATH) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def check(agg: dict, budget: dict) -> list:
    """Return a list of human-readable regression messages (empty == pass).

    Only operations that are (a) present in the committed budget and (b) backed
    by at least ``MIN_SAMPLES`` observations are enforced. New operations (no
    baseline yet) and low-sample operations are reported by the CLI but never
    fail the gate — a slowdown of a *known* operation is the signal we want.
    """
    regressions = []
    for op, baseline in sorted(budget.items()):
        obs = agg.get(op)
        if obs is None or obs["n"] < MIN_SAMPLES:
            continue
        allowed = max(baseline * TOLERANCE_RATIO, baseline + TOLERANCE_FLOOR_S)
        if obs["p95"] > allowed:
            regressions.append(
                f"  {op}: p95 {obs['p95']*1000:.0f}ms "
                f"(n={obs['n']}) > allowed {allowed*1000:.0f}ms "
                f"(baseline {baseline*1000:.0f}ms)"
            )
    return regressions


def _write_budget(agg: dict, path: Path = BUDGET_PATH) -> dict:
    """Persist ``{op: p95_seconds}`` for operations with enough samples."""
    budget = {op: v["p95"] for op, v in agg.items() if v["n"] >= MIN_SAMPLES}
    path.write_text(
        json.dumps(budget, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return budget


# --------------------------------------------------------------------------- #
# Synthetic contract tests — hostside, no device required.
# --------------------------------------------------------------------------- #

def test_normalize_op_buckets_by_operation():
    assert normalize_op("settled screen 'settings'") == "settled screen"
    assert normalize_op("settled screen 'wifi'") == "settled screen"
    assert normalize_op("widget tag='reboot_confirm'") == "widget tag"
    assert normalize_op('widget text="Save"') == "widget text"
    assert normalize_op("device to finish reboot") == "device to finish reboot"


def test_percentile_nearest_rank():
    assert percentile([0.1], 95) == 0.1
    # 20 values 0.01..0.20; p95 -> rank ceil(0.95*20)=19 -> 19th smallest = 0.19
    vals = [i / 100 for i in range(1, 21)]
    assert percentile(vals, 95) == pytest.approx(0.19)
    assert percentile([0.05, 0.05, 0.05], 95) == pytest.approx(0.05)


def test_aggregate_groups_and_computes_p95():
    samples = [["settled screen 'a'", 0.10], ["settled screen 'b'", 0.20],
               ["widget tag='x'", 0.05]]
    agg = aggregate(samples)
    assert agg["settled screen"]["n"] == 2
    assert agg["widget tag"]["n"] == 1
    assert agg["settled screen"]["p95"] == pytest.approx(0.20)


def test_check_flags_a_regression():
    budget = {"settled screen": 0.15}
    # p95 0.40 > max(0.15*1.5=0.225, 0.15+0.20=0.35) = 0.35 -> regression
    agg = {"settled screen": {"p95": 0.40, "n": 30}}
    msgs = check(agg, budget)
    assert len(msgs) == 1 and "settled screen" in msgs[0]


def test_check_allows_within_tolerance():
    budget = {"settled screen": 0.15}
    # p95 0.30 <= allowed 0.35 -> tolerated (CI jitter, not a regression)
    agg = {"settled screen": {"p95": 0.30, "n": 30}}
    assert check(agg, budget) == []


def test_check_ignores_low_sample_and_unbudgeted_ops():
    budget = {"settled screen": 0.15}
    # Way over budget but only 3 samples -> not enforced (p95 unstable).
    assert check({"settled screen": {"p95": 0.9, "n": 3}}, budget) == []
    # A brand-new operation with no baseline -> not a failure.
    assert check({"new op": {"p95": 5.0, "n": 50}}, budget) == []


def test_empty_budget_never_fails():
    # Bootstrapping state: no baseline committed yet -> gate is a no-op.
    assert check({"settled screen": {"p95": 9.0, "n": 99}}, {}) == []


# --------------------------------------------------------------------------- #
# Real-samples enforcement — runs when a device run has produced samples
# (i.e. the api-suite process in device CI, after the device suite flushed).
# --------------------------------------------------------------------------- #

@pytest.mark.skipif(not SAMPLES_PATH.exists(),
                    reason="no device latency samples on disk (hostside/local run)")
def test_recorded_latencies_within_budget():
    samples = json.loads(SAMPLES_PATH.read_text(encoding="utf-8"))
    agg = aggregate(samples)
    budget = load_budget()
    regressions = check(agg, budget)
    hint = ("A wait operation got slower than its committed budget. If this is a "
            "real, reviewed change, regenerate the baseline from this run's "
            "samples:  python tests/api/test_latency_budget.py --write  and "
            "commit tests/latency_budget.json.")
    assert not regressions, (
        "UI latency regressions detected:\n" + "\n".join(regressions) + "\n\n" + hint
    )


if __name__ == "__main__":
    import sys

    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    samples_path = Path(args[0]) if args else SAMPLES_PATH

    if not samples_path.exists():
        print(f"No samples file at {samples_path} — nothing to do.")
        sys.exit(0)

    samples = json.loads(samples_path.read_text(encoding="utf-8"))
    agg = aggregate(samples)

    if "--write" in sys.argv:
        budget = _write_budget(agg)
        total = sum(v["n"] for v in agg.values())
        print(f"Wrote {BUDGET_PATH.relative_to(ROOT).as_posix()}: "
              f"{len(budget)} operation(s) from {total} sample(s).")
        skipped = [op for op, v in agg.items() if v["n"] < MIN_SAMPLES]
        if skipped:
            print(f"  (skipped {len(skipped)} op(s) with <{MIN_SAMPLES} samples: "
                  f"{', '.join(sorted(skipped))})")
    else:
        budget = load_budget()
        print(json.dumps(agg, indent=2, sort_keys=True))
        regressions = check(agg, budget)
        if regressions:
            print("\nREGRESSIONS:\n" + "\n".join(regressions))
            sys.exit(1)
        print("\nWithin budget.")
