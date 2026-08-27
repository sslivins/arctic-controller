"""Sleep-budget ratchet: block *new* fixed-delay sleeps in the test suite.

Fixed ``time.sleep()`` / ``asyncio.sleep()`` / Playwright ``wait_for_timeout()``
calls are the classic source of flaky, slow device tests — they either wait too
long (slow) or not long enough (flaky). The durable fix is to wait for an
observable condition (``DeviceClient.wait_until`` / ``wait_for_screen`` /
``wait_for_widget``) instead.

This hostside test enforces a **ratchet**: ``tests/sleep_budget.json`` records
the exact number of sleep calls currently allowed per file. The gate fails if
any file has *more* sleeps than its budget (a regression, or a brand-new file
introducing sleeps) or *fewer* than its budget (a migration that forgot to
tighten the budget). Either way the suite can only ever move toward fewer
sleeps, never more.

When you legitimately change sleep counts (migrating a file to ``wait_until``,
or adding a justified sleep), regenerate the budget:

    python tests/api/test_sleep_budget.py --write

and commit the updated ``tests/sleep_budget.json`` alongside your change.
"""

import json
import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = ROOT / "tests"
BUDGET_PATH = TESTS_DIR / "sleep_budget.json"

# Fixed-delay calls that should be replaced by condition-based waits.
SLEEP_RE = re.compile(r"(?:\btime\.sleep|\basyncio\.sleep|\.wait_for_timeout)\s*\(")

# This file necessarily contains the literal patterns above (in SLEEP_RE), so it
# must never be scanned or budgeted.
_SELF = Path(__file__).resolve()


def _iter_test_files():
    for path in sorted(TESTS_DIR.rglob("*.py")):
        if path.resolve() == _SELF:
            continue
        yield path


def scan() -> dict:
    """Return ``{posix-relpath-from-repo-root: sleep_count}`` for files with >0."""
    counts = {}
    for path in _iter_test_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        n = len(SLEEP_RE.findall(text))
        if n:
            counts[path.relative_to(ROOT).as_posix()] = n
    return counts


def _load_budget() -> dict:
    if not BUDGET_PATH.exists():
        return {}
    return json.loads(BUDGET_PATH.read_text(encoding="utf-8"))


def _write_budget(counts: dict) -> None:
    BUDGET_PATH.write_text(
        json.dumps(counts, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def test_sleep_budget_is_not_exceeded():
    current = scan()
    budget = _load_budget()

    regressions = []  # more sleeps than allowed, or a new file with sleeps
    stale = []        # fewer sleeps than budgeted — tighten the budget

    for f, n in sorted(current.items()):
        allowed = budget.get(f)
        if allowed is None:
            regressions.append(f"  {f}: {n} sleep(s) in a file with no budget entry")
        elif n > allowed:
            regressions.append(f"  {f}: {n} sleep(s) > budget {allowed}")
        elif n < allowed:
            stale.append(f"  {f}: {n} sleep(s) < budget {allowed}")

    for f, allowed in sorted(budget.items()):
        if f not in current:
            stale.append(f"  {f}: 0 sleep(s) < budget {allowed} (file gone or fully migrated)")

    hint = ("Prefer DeviceClient.wait_until / wait_for_screen / wait_for_widget "
            "over fixed delays. After an intentional change, regenerate the "
            "budget with:  python tests/api/test_sleep_budget.py --write")

    messages = []
    if regressions:
        messages.append(
            "New or increased fixed-delay sleeps are not allowed:\n"
            + "\n".join(regressions)
        )
    if stale:
        messages.append(
            "Sleep budget is stale (a migration must tighten it in the same PR):\n"
            + "\n".join(stale)
        )
    assert not messages, "\n\n".join(messages) + "\n\n" + hint


if __name__ == "__main__":
    import sys

    if "--write" in sys.argv:
        counts = scan()
        _write_budget(counts)
        total = sum(counts.values())
        print(f"Wrote {BUDGET_PATH.relative_to(ROOT).as_posix()}: "
              f"{total} sleep(s) across {len(counts)} file(s).")
    else:
        current = scan()
        print(json.dumps(current, indent=2, sort_keys=True))
