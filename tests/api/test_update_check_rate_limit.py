"""Guard update-check behavior against exhausting GitHub's public API quota."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main" / "main.cpp"
WEB = ROOT / "main" / "web" / "index.html"


def test_periodic_update_check_is_at_least_hourly():
    source = MAIN.read_text(encoding="utf-8")
    match = re.search(
        r"#define\s+UPDATE_CHECK_INTERVAL_MS\s+\(([^)]+)\)",
        source,
    )
    assert match, "UPDATE_CHECK_INTERVAL_MS definition not found"

    factors = [int(value) for value in re.findall(r"\d+", match.group(1))]
    interval_ms = 1
    for factor in factors:
        interval_ms *= factor

    assert interval_ms >= 60 * 60 * 1000


def test_mutation_errors_are_not_toasted_twice():
    source = WEB.read_text(encoding="utf-8")
    assert "error.toastShown = true" in source
    assert source.count("if (!error.toastShown) toast(error.message, \"bad\");") == 2
