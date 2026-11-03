"""Selection logic for the PR-impacted stage of the device suite
(`.github/scripts/impacted_tests.py`)."""

import importlib.util
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).parents[1]
_spec = importlib.util.spec_from_file_location(
    "impacted_tests", ROOT / ".github" / "scripts" / "impacted_tests.py")
it = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = it
_spec.loader.exec_module(it)

SCREEN = '''
static const timezone_entry_t timezones[] = {
    {"US Pacific (PST/PDT)",      "PST8PDT,M3.2.0,M11.1.0"},
    {"Canada BC (PCT)",           "<PCT>7"},
};
'''


def test_a_test_naming_entries_beside_the_change_is_picked():
    # The PR added "Canada BC (PCT)"; the test only knows the entry next to it.
    weights = it.weigh(None, SCREEN)
    tests = {
        "tests/device/test_timezone.py": 'TZ = {"name": "US Pacific (PST/PDT)"}',
        "tests/device/test_unrelated.py": "def test_x(): pass",
    }
    assert it.rank([], [weights], tests) == ["tests/device/test_timezone.py"]


def test_one_large_diff_does_not_crowd_out_a_small_one():
    dashboard = {f"el-{i}": 2.0 for i in range(20)}
    screen = {"timezone_roller": 1.0}
    tests = {f"tests/web/test_w{i}.py": f'page.locator("#el-{i}")' for i in range(20)}
    tests["tests/device/test_timezone.py"] = 'set_roller("timezone_roller", 3)'
    picked = it.rank([], [dashboard, screen], tests, limit=4)
    assert "tests/device/test_timezone.py" in picked
    assert len(picked) == 4


def test_a_string_in_too_many_test_files_is_ignored():
    tests = {f"tests/api/test_{i}.py": '"demo_mode": True' for i in range(it.MAX_FANOUT + 1)}
    assert it.rank([], [{"demo_mode": 2.0}], tests) == []


def test_changed_tests_lead_and_are_never_dropped():
    tests = {f"tests/api/test_{i}.py": '"/api/ota/status"' for i in range(3)}
    changed = ["tests/api/test_2.py", "tests/api/test_1.py"]
    assert it.rank(changed, [{"/api/ota/status": 1.0}], tests, limit=1) == changed


def test_a_tag_must_match_whole():
    assert it.mentions('find_widget(tag="timezone_roller")', "timezone_roller")
    assert not it.mentions('find_widget(tag="timezone_roller_label")', "timezone_roller")
