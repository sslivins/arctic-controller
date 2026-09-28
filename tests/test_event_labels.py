"""Every event type has a name on both the device screen and the web UI.

The device's Events screen once showed network_unreachable/network_recovered
as "System Start": event_type_to_str_id() had no case for them and fell
through to its default. The web UI likewise needs a name and icon for every
type. These checks fail as soon as a new event type is added to event_log.h
without a label in both places.
"""

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


def _event_enum():
    text = (MAIN / "event_log.h").read_text(encoding="utf-8")
    body = re.search(r"typedef enum \{(.*?)\} event_type_t;", text, re.S).group(1)
    names = re.findall(r"^\s*(EVENT_[A-Z_]+)", body, re.M)
    names = [n for n in names if n != "EVENT_TYPE_COUNT"]
    assert len(names) > 20, names
    return names


def _switch_cases(source, function):
    start = source.index(function)
    end = source.index("\n}\n", start)
    return set(re.findall(r"case (EVENT_[A-Z_]+):", source[start:end]))


@pytest.mark.parametrize("function", [
    "static string_id_t event_type_to_str_id(",
    "static const char* event_type_icon(",
])
def test_device_screen_handles_every_event_type(function):
    source = (MAIN / "event_log_screen.cpp").read_text(encoding="utf-8")
    missing = set(_event_enum()) - _switch_cases(source, function)
    assert not missing, f"{function.strip()} has no case for {sorted(missing)}"


def _web_object_keys(html, const_name):
    body = re.search(r"const %s = \{(.*?)\};" % const_name, html, re.S).group(1)
    return set(re.findall(r"(\w+):", body))


@pytest.mark.parametrize("const_name", ["eventNames", "eventIcons"])
def test_web_ui_labels_every_event_type(const_name):
    html = (MAIN / "web" / "index.html").read_text(encoding="utf-8")
    api_names = {n[len("EVENT_"):].lower() for n in _event_enum()}
    missing = api_names - _web_object_keys(html, const_name)
    assert not missing, f"web UI {const_name} has no entry for {sorted(missing)}"
