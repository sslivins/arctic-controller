"""The PR title/description gate that runs first in ci.yml."""

import importlib.util
import re
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).parents[1]
_spec = importlib.util.spec_from_file_location(
    "check_pr_metadata", ROOT / ".github" / "scripts" / "check_pr_metadata.py")
meta = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = meta
_spec.loader.exec_module(meta)

GOOD_BODY = "French and Spanish showed VENTIL. under the fan speed; spell it out."


@pytest.mark.parametrize("title", [
    "fix(i18n): spell out the fan label and translate the RPM unit",
    "feat: link each fault to Arctic's troubleshooting article",
    "ci: reject PRs without a useful title before the device suite",
    "refactor(api)!: split the preferences handler into validators",
    "chore: remove the stale ESP-IDF master toolchain notes",
])
def test_accepts_useful_conventional_titles(title):
    assert meta.check(title, GOOD_BODY) == []


@pytest.mark.parametrize("title", [
    "Link each fault to Arctic's troubleshooting article (QR code, web, HA)",
    "Refresh language live, fix missing accents",
    "Fix: capitalised type",
    "fix:missing space",
    "fix (i18n): space before scope",
])
def test_rejects_titles_without_a_conventional_prefix(title):
    errors = meta.check_title(title)
    assert errors and "conventional commit" in errors[0]


def test_rejects_types_the_changelog_ignores():
    errors = meta.check_title("perf: make the home screen redraw faster")
    assert errors and '"perf:" is not a recognised type' in errors[0]


@pytest.mark.parametrize("title", [
    "fix: update", "feat: changes", "fix: stuff.", "docs: tweaks", "fix: typo", "feat: webui",
])
def test_rejects_vague_subjects(title):
    errors = meta.check_title(title)
    assert errors and "too vague" in errors[0]


@pytest.mark.parametrize("title", ["feat: WIP friendly controller name", "fix: do not merge yet please"])
def test_rejects_work_in_progress(title):
    assert any("work-in-progress" in e for e in meta.check_title(title))


@pytest.mark.parametrize("body", [
    "",
    None,
    "   \n\n  ",
    "## What\n\n## Why\n\n## Testing\n- [ ]\n",
    "<!-- Describe what changed and why. -->\n## What\n",
    "Fixes the thing.",
])
def test_rejects_empty_or_template_only_descriptions(body):
    errors = meta.check_body(body)
    assert errors and "description" in errors[0]


def test_accepts_a_filled_in_template():
    body = (Path(ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md").read_text(encoding="utf-8")
            + "\nThe fan label was abbreviated in French and Spanish; it now fits in full.\n")
    assert meta.check_body(body) == []


def test_the_bare_template_is_rejected():
    template = (ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md").read_text(encoding="utf-8")
    assert meta.check_body(template), "the unedited PR template must not pass"


def test_types_match_the_release_changelog():
    """Every accepted type except chore must be a changelog category in create-release.yml."""
    release = (ROOT / ".github" / "workflows" / "create-release.yml").read_text(encoding="utf-8")
    categories = set(re.findall(r'--grep="\^([a-z]+)"', release))
    assert set(meta.TYPES) - {"chore"} == categories
