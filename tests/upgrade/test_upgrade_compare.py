"""Host-side tests for the upgrade migration check's comparison rules."""

import json

import pytest

from upgrade_check import EXCEPTIONS_PATH, CheckError, _seed_diff, compare, load_exceptions

pytestmark = pytest.mark.hostside


def _snapshot():
    return {
        "firmware": {"current_version": "3.0.1", "build_sha": "a"},
        "settings": {
            "/api/preferences": {"temp_unit": "fahrenheit", "language_code": "fr", "brightness": 37},
            "/api/performance/config": {"flow_lpm": 23.5, "sensors": {"supply": {"host": "192.0.2.10"}}},
            "/api/location": None,  # not in the old release
        },
        "events": [
            {"type": "mode_changed", "timestamp": 100, "boot_id": 7, "payload": 2, "details": {"to": "cooling"}},
            {"type": "power_off", "timestamp": 101, "boot_id": 7, "payload": 0},
        ],
        "history": {"end": 300, "samples": [{"t": 240, "in": 30.5, "mode": 1}, {"t": 270, "in": 30.6, "mode": 1}]},
    }


def test_identical_data_passes():
    assert compare(_snapshot(), _snapshot(), []) == []


def test_new_keys_events_and_samples_are_allowed():
    new = _snapshot()
    new["settings"]["/api/preferences"]["new_pref"] = True
    new["settings"]["/api/location"] = {"valid": False}
    new["events"].insert(1, {"type": "system_start", "timestamp": 200, "boot_id": 8, "payload": 0})
    new["events"][0]["details"]["help_url"] = "x"
    new["history"]["samples"].append({"t": 300, "in": 31.0, "mode": 1})
    new["history"]["samples"][0]["run"] = True
    assert compare(_snapshot(), new, []) == []


@pytest.mark.parametrize("mutate, where", [
    (lambda s: s["settings"]["/api/preferences"].update(temp_unit="celsius"), "/api/preferences.temp_unit"),
    (lambda s: s["settings"]["/api/performance/config"]["sensors"]["supply"].pop("host"),
     "/api/performance/config.sensors.supply.host"),
    (lambda s: s["settings"].update({"/api/preferences": None}), "/api/preferences"),
    (lambda s: s["events"].pop(0), "/api/events"),
    (lambda s: s["events"][1].update(timestamp=0), "/api/events"),
    (lambda s: s["events"].reverse(), "/api/events"),
    (lambda s: s["history"]["samples"].pop(), "/api/heatpump/temperature-history"),
    (lambda s: s["history"]["samples"][0].update({"in": 0}), "/api/heatpump/temperature-history"),
    (lambda s: s.update(history=None), "/api/heatpump/temperature-history"),
])
def test_lost_or_changed_data_fails(mutate, where):
    new = _snapshot()
    mutate(new)
    problems = compare(_snapshot(), new, [])
    assert problems and problems[0].startswith(where), problems


def test_exception_silences_only_its_path():
    new = _snapshot()
    new["settings"]["/api/preferences"]["temp_unit"] = "celsius"
    new["events"][0]["details"]["to"] = "COOLING"
    ignored = ["/api/preferences.temp_unit", "/api/events.details.to"]
    assert compare(_snapshot(), new, ignored) == []
    new["settings"]["/api/preferences"]["brightness"] = 50
    assert len(compare(_snapshot(), new, ignored)) == 1


def test_exceptions_file_is_valid():
    load_exceptions()
    assert isinstance(json.loads(EXCEPTIONS_PATH.read_text())["ignore"], list)


def test_exception_without_reason_is_rejected(tmp_path):
    p = tmp_path / "x.json"
    p.write_text(json.dumps({"ignore": [{"path": "/api/preferences.language"}]}))
    with pytest.raises(CheckError):
        load_exceptions(p)


def test_seed_readback_skips_fields_the_old_release_predates():
    want = {"a": 1, "s": {"x": 2, "new_field": "n"}, "missing_top": 3}
    got = {"a": 1, "s": {"x": 2}}
    assert _seed_diff(want, got) == ({}, ["s.new_field", "missing_top"])


def test_seed_readback_fails_on_a_reported_but_different_value():
    bad, _ = _seed_diff({"s": {"x": 2}}, {"s": {"x": 3}})
    assert bad == {"s.x": (2, 3)}
