"""Guard against a competing, unverifiable test backlog reappearing in-tree.

`tests/device/TODO.md` was 300 lines with 74 unchecked and 69 checked items, last
substantively touched 2026-08-16 (before #164). By the time it was removed its
checkboxes were actively misleading in both directions: items marked unchecked
were in fact covered (inject-fault had 43 matches in the suite, localization 63,
Fahrenheit 79), and nothing verified that its `[x]` claims still held.

A markdown checklist cannot be executed, so nothing tells you when it goes
stale. Anyone reading it to decide what to work on gets a wrong answer, which is
worse than having no list. Coverage gaps belong in issues, where they can be
closed by a PR, or in a test, where CI keeps them honest.

This test is the "or gate it with a test" half of #217 T20.
"""

from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]

# Files that would recreate the same problem: a hand-maintained checklist of
# test work, living next to the tests, with no mechanism to keep it true.
FORBIDDEN = [
    Path("tests") / "device" / "TODO.md",
    Path("tests") / "TODO.md",
    Path("tests") / "api" / "TODO.md",
    Path("tests") / "web" / "TODO.md",
]


def test_no_competing_test_backlog_file():
    present = [p.as_posix() for p in FORBIDDEN if (ROOT / p).exists()]
    assert not present, (
        "A test-backlog markdown file has reappeared: "
        + ", ".join(present)
        + ".\nTrack coverage gaps as GitHub issues (see #217) or as skipped/xfail tests, "
        "not as a checklist that nothing verifies. tests/device/TODO.md was removed "
        "precisely because its checkboxes had drifted out of sync with the suite in "
        "both directions."
    )


def test_checklist_free_of_stale_claims_in_test_readmes():
    """READMEs may document architecture, but must not carry a coverage checklist.

    A `- [ ]`/`- [x]` list in a README is the same failure mode wearing a
    different filename.
    """
    offenders = []
    for readme in sorted((ROOT / "tests").rglob("README.md")):
        text = readme.read_text(encoding="utf-8", errors="replace")
        boxes = text.count("- [ ]") + text.count("- [x]") + text.count("- [X]")
        if boxes >= 5:
            offenders.append(f"{readme.relative_to(ROOT).as_posix()} ({boxes} checkboxes)")

    assert not offenders, (
        "Test README(s) contain a coverage checklist that nothing verifies: "
        + ", ".join(offenders)
        + ".\nMove these to issues; see #217 T20."
    )
