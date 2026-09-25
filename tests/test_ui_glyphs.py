"""Every non-ASCII character in the firmware's UI strings must exist in the fonts.

The LVGL fonts are subsets, so a missing character renders as an empty box on
the device (e.g. "0° ▯ 50°" in the event log before U+2192 was added).
See tools/check_glyphs.py.
"""

import importlib.util
import textwrap
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

ROOT = Path(__file__).parents[1]
_spec = importlib.util.spec_from_file_location("check_glyphs", ROOT / "tools" / "check_glyphs.py")
check_glyphs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_glyphs)


def test_firmware_ui_strings_only_use_characters_the_fonts_can_draw():
    problems, checked = check_glyphs.find_problems(ROOT)
    assert not problems, "Characters the device cannot draw:\n" + "\n".join(problems)
    assert checked > 100, "the scan found suspiciously few non-ASCII characters"


def test_the_arrow_and_dashes_used_by_the_ui_are_in_every_ui_font():
    fonts = check_glyphs.load_fonts(ROOT / "main" / "fonts")
    ui = [n for n in fonts if n.startswith("montserrat_") and n.endswith("_latin")]
    assert len(ui) == 4
    for name in ui:
        for cp in (0x2192, 0x2014, 0x2013, 0xB0, 0xE9):
            assert cp in fonts[name], f"U+{cp:04X} missing from {name}"


# --- The checker itself -----------------------------------------------------

FONT_TEMPLATE = """
static const uint16_t unicode_list_1[] = {{ 0x0, 0x2 }};
static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{ .range_start = 32, .range_length = 95, .glyph_id_start = 1,
      .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0 }},
    {{ .range_start = {sparse_start}, .range_length = 3, .glyph_id_start = 96,
      .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = 2 }}
}};
const lv_font_t {name} = {{
    .fallback = {fallback},
}};
"""


def _tree(tmp_path, source, sparse_start=0xB0):
    fonts = tmp_path / "main" / "fonts"
    fonts.mkdir(parents=True)
    # 0xB0 and 0xB2 (°, ²) in the latin font; the misc fallback adds U+2192/U+2194.
    for size in (16, 24):
        (fonts / f"montserrat_{size}_latin.c").write_text(FONT_TEMPLATE.format(
            name=f"montserrat_{size}_latin", sparse_start=sparse_start,
            fallback=f"&montserrat_{size}_misc"), encoding="utf-8")
        (fonts / f"montserrat_{size}_misc.c").write_text(FONT_TEMPLATE.format(
            name=f"montserrat_{size}_misc", sparse_start=0x2192, fallback="NULL"),
            encoding="utf-8")
    (tmp_path / "main" / "ui.cpp").write_text(textwrap.dedent(source), encoding="utf-8")
    return tmp_path


def test_flags_a_character_missing_from_the_fonts(tmp_path):
    root = _tree(tmp_path, 'lv_label_set_text(l, "Standby \\xe2\\x80\\x94 waiting");\n')
    problems, _ = check_glyphs.find_problems(root)
    assert problems == ["main/ui.cpp:1: U+2014 '\u2014' is not in the Montserrat UI fonts"]


def test_accepts_glyphs_found_through_the_fallback_font(tmp_path):
    root = _tree(tmp_path, 'snprintf(buf, n, "%d\u00b0 \u2192 %d\u00b0", a, b);\n')
    assert check_glyphs.find_problems(root) == ([], 2)


def test_ignores_logs_comments_and_annotated_lines(tmp_path):
    root = _tree(tmp_path, """\
        // A comment with an em dash \u2014 is fine
        /* so is a block \u2014 comment */
        ESP_LOGW(TAG, "boot \u2014 %s",
                 "still inside the log call \u2014");
        send(req, "\\xEF\\xBB\\xBF", 3);  // glyphs: not-ui
        """)
    assert check_glyphs.find_problems(root) == ([], 0)


def test_annotation_checks_against_the_named_font(tmp_path):
    root = _tree(tmp_path, """\
        #define ICON_OK  "\u2192"  // glyphs: montserrat_16_misc
        #define ICON_BAD "\u00b0"  // glyphs: montserrat_16_misc
        #define ICON_TYPO "x"      // glyphs: no_such_font
        """)
    problems, _ = check_glyphs.find_problems(root)
    assert problems == [
        "main/ui.cpp:2: U+00B0 '\u00b0' is not in montserrat_16_misc",
        "main/ui.cpp:3: unknown font 'no_such_font' in glyphs annotation",
    ]
