"""Keep the OpenAPI event enums in step with the firmware's own tables.

The firmware already guards itself: a static_assert in event_log.cpp ensures
s_event_names matches EVENT_TYPE_COUNT, so adding an enum value without a name
fails the build. Nothing extended that guarantee to docs/openapi.yaml, so the
spec was free to drift.

It did. network_unreachable and network_recovered existed in firmware but in
neither the spec nor the API tests. Because those events are only emitted when
the device actually loses its gateway, the gap stayed invisible until a
scheduled run happened to coincide with a real network blip, and then failed
two suites at once with what looked like a flake.

That is the failure mode worth preventing: the events most likely to be missing
from the spec are the rare ones, and rare events are exactly what a schema
consumer is least prepared to handle. Checking at build time is cheap; waiting
for the device to produce the event is not.

These tests are hostside — they read source, never the device.
"""

import re
from pathlib import Path

import yaml
import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
EVENT_LOG = ROOT / "main" / "event_log.cpp"
OPENAPI_SPEC = ROOT / "docs" / "openapi.yaml"


def _firmware_table(name):
    """Extract the string literals from a `static const char* <name>[]` table."""
    source = EVENT_LOG.read_text(encoding="utf-8")
    match = re.search(
        r"static\s+const\s+char\*\s+" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{(?P<body>.*?)\};",
        source,
        re.DOTALL,
    )
    assert match, (
        f"Could not find {name}[] in {EVENT_LOG.name}. If the table was renamed "
        "or restructured, update this test — do not delete it."
    )

    names = re.findall(r'"([^"]+)"', match.group("body"))
    assert names, f"{name}[] parsed as empty, so this test would vacuously pass"
    return names


def _spec_enum(schema, prop):
    spec = yaml.safe_load(OPENAPI_SPEC.read_text(encoding="utf-8"))
    enum = spec["components"]["schemas"][schema]["properties"][prop]["enum"]
    assert enum, f"{schema}.{prop} enum is empty, so this test would vacuously pass"
    return enum


def test_event_types_match_firmware():
    firmware = set(_firmware_table("s_event_names"))
    documented = set(_spec_enum("EventEntry", "type"))

    missing = firmware - documented
    extra = documented - firmware

    assert not missing, (
        "Event types the firmware can emit but the OpenAPI spec does not allow:\n"
        + "\n".join(f"  {t}" for t in sorted(missing))
        + "\n\nAdd them to EventEntry.type in docs/openapi.yaml. Until you do, any "
        "schema-validating client (including our own schemathesis suite) will "
        "reject a perfectly valid response the moment the device emits one."
    )

    assert not extra, (
        "Event types documented in the OpenAPI spec that the firmware cannot emit:\n"
        + "\n".join(f"  {t}" for t in sorted(extra))
        + "\n\nEither the firmware dropped an event type and the spec was not "
        "updated, or the name was mistyped. Stale values are not harmless: they "
        "tell integrators to handle cases that will never arrive."
    )


def test_event_categories_match_firmware():
    firmware = set(_firmware_table("s_category_names"))
    documented = set(_spec_enum("EventEntry", "category"))

    assert firmware == documented, (
        f"Event categories disagree.\n"
        f"  firmware only: {sorted(firmware - documented)}\n"
        f"  spec only:     {sorted(documented - firmware)}"
    )
