"""i18n completeness: every string ID must be translated in every language.

`main/i18n/i18n.cpp` holds three parallel tables -- `strings_en`, `strings_fr`
and `strings_es` -- each indexed by the `STR_*` enum in `main/i18n/strings.h`.
They are designated-initialiser arrays, so a missing entry is not a compile
error: the slot is simply NULL. At runtime `i18n_get()` falls back to English,
so a forgotten translation is invisible on the device unless someone happens to
switch language and recognise the untranslated word.

Existing coverage checked exactly four keys (the `STR_HISTORY_*` group) and
otherwise rendered a handful of screens in French and Spanish. That samples the
tables; it cannot prove them complete. This sweeps all of them.

This is a source lint, so it runs on a hosted runner with no device -- which
matters because the alternative (rendering every screen in three languages) is
neither feasible nor reliable on the single physical controller.

Refs #217 (T16).
"""

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).resolve().parents[2]
STRINGS_H = ROOT / "main" / "i18n" / "strings.h"
I18N_CPP = ROOT / "main" / "i18n" / "i18n.cpp"

LANGUAGES = ["en", "fr", "es"]

# A key deliberately left identical across languages is fine (e.g. "OK", "WiFi",
# units). We only require presence and non-emptiness, never difference.
_KEY_RE = re.compile(r"\[\s*(STR_[A-Z0-9_]+)\s*\]\s*=\s*(.)", re.S)


def _read(path: Path) -> str:
    assert path.exists(), f"{path} is missing"
    return path.read_text(encoding="utf-8", errors="replace")


def declared_string_ids() -> list[str]:
    """Every STR_* enumerator from strings.h, in declaration order, minus STR_COUNT."""
    text = _read(STRINGS_H)
    ids: list[str] = []
    for match in re.finditer(r"^\s*(STR_[A-Z0-9_]+)\s*(?:=[^,]*)?,", text, re.M):
        name = match.group(1)
        if name != "STR_COUNT":
            ids.append(name)
    return ids


def reserved_string_ids() -> set[str]:
    """IDs strings.h marks as deliberately unused (kept only for enum stability).

    Derived from the source comment rather than hardcoded here, so retiring or
    adding a reserved slot needs no change to this test -- and, more importantly,
    so that emptying a *real* translation cannot be waved through by editing a
    list in the test file.
    """
    text = _read(STRINGS_H)
    return {
        m.group(1)
        for m in re.finditer(
            r"^\s*(STR_[A-Z0-9_]+)\s*,\s*//[^\n]*\b(unused|reserved)\b",
            text,
            re.M | re.I,
        )
    }


def _table_body(text: str, lang: str) -> str:
    """Return the initialiser body of strings_<lang>, brace-matched."""
    start = re.search(rf"strings_{lang}\s*\[\s*STR_COUNT\s*\]\s*=\s*\{{", text)
    assert start, f"could not find the strings_{lang} table in i18n.cpp"
    i = start.end()
    depth = 1
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    assert depth == 0, f"unbalanced braces in strings_{lang}"
    return text[start.end() : i - 1]


def table_entries(lang: str) -> dict[str, str]:
    """Map STR_ID -> first character of its initialiser for one language table."""
    body = _table_body(_read(I18N_CPP), lang)
    return {m.group(1): m.group(2) for m in _KEY_RE.finditer(body)}


def test_string_ids_are_discovered():
    ids = declared_string_ids()
    assert len(ids) > 200, (
        f"only {len(ids)} STR_ ids parsed from strings.h; the parser has probably "
        "broken and this whole module would silently stop checking anything"
    )


@pytest.mark.parametrize("lang", LANGUAGES)
def test_every_string_id_is_present_in_every_language(lang):
    declared = declared_string_ids()
    entries = table_entries(lang)
    missing = [key for key in declared if key not in entries]
    assert not missing, (
        f"{len(missing)} string ID(s) have no entry in strings_{lang} and will "
        f"silently fall back to English on the device:\n  " + "\n  ".join(missing)
    )


@pytest.mark.parametrize("lang", LANGUAGES)
def test_no_translation_is_empty_or_null(lang):
    entries = table_entries(lang)
    bad = [key for key, first in entries.items() if first not in {'"'} ]
    assert not bad, (
        f"{len(bad)} entry/entries in strings_{lang} are NULL or not a string "
        f"literal, which renders as blank or falls back:\n  " + "\n  ".join(sorted(bad))
    )

    empty = [
        key
        for key in entries
        if key not in reserved_string_ids()
        and re.search(rf"\[\s*{key}\s*\]\s*=\s*\"\"", _table_body(_read(I18N_CPP), lang))
    ]
    assert not empty, (
        f"{len(empty)} entry/entries in strings_{lang} are the empty string:\n  "
        + "\n  ".join(sorted(empty))
    )


def test_reserved_ids_are_empty_in_every_language():
    """A reserved slot must be uniformly blank, or it is not really reserved."""
    reserved = reserved_string_ids()
    assert reserved, (
        "no reserved IDs parsed from strings.h; if the 'unused, kept for enum "
        "stability' comments were reworded, the empty-string exemption above has "
        "silently widened and this module should be updated deliberately"
    )
    bodies = {lang: _table_body(_read(I18N_CPP), lang) for lang in LANGUAGES}
    inconsistent = [
        f"{key} is non-empty in strings_{lang}"
        for key in sorted(reserved)
        for lang, body in bodies.items()
        if not re.search(rf"\[\s*{key}\s*\]\s*=\s*\"\"", body)
    ]
    assert not inconsistent, (
        "IDs documented as unused carry a translation in some languages, so they "
        "are either in use (and the comment is wrong) or the text is dead:\n  "
        + "\n  ".join(inconsistent)
    )


@pytest.mark.parametrize("lang", LANGUAGES)
def test_no_unknown_keys_in_table(lang):
    """A typo'd or removed ID left in a table is dead weight and hides a real gap."""
    declared = set(declared_string_ids())
    unknown = sorted(set(table_entries(lang)) - declared)
    assert not unknown, (
        f"strings_{lang} initialises ID(s) not declared in strings.h:\n  "
        + "\n  ".join(unknown)
    )


@pytest.mark.parametrize("lang", LANGUAGES)
def test_no_duplicate_keys_in_table(lang):
    """C allows a later designated initialiser to silently overwrite an earlier one."""
    body = _table_body(_read(I18N_CPP), lang)
    keys = [m.group(1) for m in _KEY_RE.finditer(body)]
    duplicates = sorted({key for key in keys if keys.count(key) > 1})
    assert not duplicates, (
        f"strings_{lang} initialises the same ID more than once; the last one wins "
        f"and the earlier translation is silently discarded:\n  " + "\n  ".join(duplicates)
    )


def test_all_languages_cover_the_same_ids():
    """Catches drift even if strings.h parsing were ever to miss an ID."""
    tables = {lang: set(table_entries(lang)) for lang in LANGUAGES}
    union = set().union(*tables.values())
    report = []
    for lang, keys in tables.items():
        gap = sorted(union - keys)
        if gap:
            report.append(f"strings_{lang} is missing {len(gap)}: " + ", ".join(gap[:20]))
    assert not report, "language tables disagree on which IDs they translate:\n  " + "\n  ".join(report)
