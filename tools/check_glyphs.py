#!/usr/bin/env python3
"""Fail if firmware UI strings use characters the LVGL fonts cannot draw.

The UI fonts are subsets (see main/fonts/), so a character outside them renders
as an empty box on the device. This reads the exact code points each font
contains, including its fallback chain, straight from the generated font .c
files, then checks every non-ASCII character in the firmware's string literals
(plain UTF-8 or \\x escapes, e.g. the i18n tables).

A string must be drawable by every Montserrat UI size, because the same text
can be shown at any size. Literals passed to logging calls are skipped (they
never reach the display). For anything else that is not UI text, or that is
drawn with a different font, annotate the line:

    // glyphs: not-ui              skip this line
    // glyphs: weather_icons_32    check against that font instead

Usage: python tools/check_glyphs.py [--root DIR]
Exit status: 0 = clean, 1 = missing glyphs found.
Run in CI by tests/test_ui_glyphs.py (Host Tests).
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

UI_FONT_GLOB = "montserrat_*_latin"
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp"}
SKIP_DIRS = {"fonts", "tests"}
# Calls whose string arguments go to the log/console, never to the display.
LOG_CALL = re.compile(
    r"^(ESP_(EARLY_|DRAM_)?LOG[EWIDV]|ESP_LOG_BUFFER\w*|printf|fprintf|puts|"
    r"ESP_RETURN_ON_\w+|ESP_GOTO_ON_\w+|ESP_ERROR_CHECK\w*|LV_LOG\w*|assert)$"
)
ANNOTATION = re.compile(r"//\s*glyphs:\s*([\w-]+)")


def parse_font(path: pathlib.Path) -> tuple[set[int], str | None]:
    """Return (code points, fallback font name) for an lv_font_conv .c file."""
    text = path.read_text(encoding="utf-8", errors="replace")
    lists = {
        m.group(1): [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", m.group(2))]
        for m in re.finditer(r"unicode_list_(\d+)\[\]\s*=\s*\{([^}]*)\}", text)
    }
    cps: set[int] = set()
    for m in re.finditer(
        r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+).*?"
        r"\.unicode_list\s*=\s*(\w+)",
        text,
        re.S,
    ):
        start, length, ulist = int(m.group(1)), int(m.group(2)), m.group(3)
        if ulist == "NULL":
            cps.update(range(start, start + length))
        else:
            cps.update(start + ofs for ofs in lists[ulist.rsplit("_", 1)[1]])
    fallback = re.search(r"\.fallback\s*=\s*&(\w+)", text)
    return cps, fallback.group(1) if fallback else None


def load_fonts(font_dir: pathlib.Path) -> dict[str, set[int]]:
    """Map font name -> code points it can draw, following fallback fonts."""
    raw = {p.stem: parse_font(p) for p in font_dir.glob("*.c")}
    resolved: dict[str, set[int]] = {}
    for name in raw:
        cps, seen, fb = set(raw[name][0]), {name}, raw[name][1]
        while fb and fb in raw and fb not in seen:
            seen.add(fb)
            cps |= raw[fb][0]
            fb = raw[fb][1]
        resolved[name] = cps
    return resolved


def decode_literal(body: str) -> str:
    """Decode a C string literal body (UTF-8 source plus \\x/\\ooo escapes)."""
    out = bytearray()
    i = 0
    while i < len(body):
        ch = body[i]
        if ch == "\\" and i + 1 < len(body):
            nxt = body[i + 1]
            if nxt == "x":
                m = re.match(r"[0-9a-fA-F]{1,2}", body[i + 2 :])
                out.append(int(m.group(0), 16) if m else 0)
                i += 2 + (len(m.group(0)) if m else 0)
                continue
            m = re.match(r"[0-7]{1,3}", body[i + 1 :])
            if m:
                out.append(int(m.group(0), 8) & 0xFF)
                i += 1 + len(m.group(0))
                continue
            out.append(0x20)  # \n, \t, \" ... are ASCII; the value is irrelevant
            i += 2
            continue
        out += ch.encode("utf-8")
        i += 1
    return out.decode("utf-8", errors="replace")


def literals(source: str):
    """Yield (line_no, literal_body, enclosing_call_name) for each string literal.

    Comments and char literals are skipped. The enclosing call is the
    identifier before the innermost open parenthesis of the current statement.
    """
    i, line, n = 0, 1, len(source)
    stack: list[str] = []
    while i < n:
        c = source[i]
        if c == "\n":
            line += 1
            i += 1
        elif source.startswith("//", i):
            j = source.find("\n", i)
            i = n if j < 0 else j
        elif source.startswith("/*", i):
            j = source.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += source.count("\n", i, j)
            i = j
        elif c == "'":
            m = re.match(r"'(\\.|[^'\\\n])*'", source[i:])
            i += len(m.group(0)) if m else 1
        elif c == '"':
            j = i + 1
            while j < n and source[j] not in '"\n':
                j += 2 if source[j] == "\\" else 1
            yield line, source[i + 1 : j], (stack[-1] if stack else "")
            i = j + 1
        elif c == "(":
            m = re.search(r"([A-Za-z_]\w*)\s*$", source[max(0, i - 64) : i])
            stack.append(m.group(1) if m else "")
            i += 1
        elif c == ")":
            if stack:
                stack.pop()
            i += 1
        elif c in ";{}":
            stack.clear()  # statement boundary: no longer inside any call
            i += 1
        else:
            i += 1


def find_problems(root: pathlib.Path) -> tuple[list[str], int]:
    """Return (problem lines, number of non-ASCII characters checked)."""
    main_dir = root / "main"
    fonts = load_fonts(main_dir / "fonts")
    ui_fonts = {n: cps for n, cps in fonts.items() if pathlib.PurePath(n).match(UI_FONT_GLOB)}
    if not ui_fonts:
        return [f"no {UI_FONT_GLOB} fonts found in {main_dir / 'fonts'}"], 0
    ui_common = set.intersection(*ui_fonts.values())

    problems: list[str] = []
    checked = 0
    for path in sorted(main_dir.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if path.suffix not in SOURCE_SUFFIXES or SKIP_DIRS & set(path.relative_to(main_dir).parts):
            continue
        source = path.read_text(encoding="utf-8", errors="replace")
        lines = source.splitlines()
        for line_no, body, call in literals(source):
            if LOG_CALL.match(call):
                continue
            note = ANNOTATION.search(lines[line_no - 1]) if line_no <= len(lines) else None
            if note and note.group(1) == "not-ui":
                continue
            if note:
                if note.group(1) not in fonts:
                    problems.append(f"{rel}:{line_no}: unknown font '{note.group(1)}' "
                                    "in glyphs annotation")
                    continue
                allowed, where = fonts[note.group(1)], note.group(1)
            else:
                allowed, where = ui_common, "the Montserrat UI fonts"
            for ch in dict.fromkeys(decode_literal(body)):
                cp = ord(ch)
                if cp < 0x80:
                    continue
                checked += 1
                if cp not in allowed:
                    problems.append(f"{rel}:{line_no}: U+{cp:04X} '{ch}' is not in {where}")
    return problems, checked


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parents[1])
    args = ap.parse_args()
    problems, checked = find_problems(args.root)
    if problems:
        print("Characters the device cannot draw (they render as empty boxes):")
        for p in problems:
            print("  " + p)
        print("\nFix: add the code point to main/fonts/regen_misc_fonts.ps1 and rerun it, "
              "use a character the fonts already have, or annotate a non-UI line with "
              "'// glyphs: not-ui'.")
        return 1
    print(f"check_glyphs: OK ({checked} non-ASCII characters checked)")
    return 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
