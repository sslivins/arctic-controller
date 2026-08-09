"""Guard the shared device dialog styling.

These are static source checks (no device required): once every LVGL dialog goes
through ui_dialog_create(), a stray lv_msgbox would silently reintroduce the
unstyled default look, so assert the codebase stays free of direct msgbox use.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "main"


def test_device_dialogs_use_shared_styled_component():
    source_files = list(MAIN.rglob("*.cpp")) + list(MAIN.rglob("*.h"))
    msgbox_users = [
        str(path.relative_to(ROOT))
        for path in source_files
        if "lv_msgbox" in path.read_text(encoding="utf-8", errors="replace")
    ]
    assert msgbox_users == []

    dialog = (MAIN / "ui_dialog.cpp").read_text(encoding="utf-8")
    assert "UI_FONT_DIALOG_TITLE" in dialog
    assert "UI_FONT_SMALL" in dialog
    assert "UI_COLOR_PANEL" in dialog
