from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_display_idle_has_additive_stages():
    source = (ROOT / "main" / "display_idle.cpp").read_text(encoding="utf-8")
    assert "s_dimmed_since_ms" in source
    assert "s_off_minutes" in source
    assert "display_idle_force_off" in source
    assert "lv_tick_elaps(s_dimmed_since_ms)" in source


def test_display_timeout_controls_are_device_only():
    device_ui = (
        ROOT / "main" / "settings" / "settings_display_screen.cpp"
    ).read_text(encoding="utf-8")
    web_ui = (ROOT / "main" / "web" / "index.html").read_text(encoding="utf-8")

    assert "display_dim_timeout" in device_ui
    assert "display_off_timeout" in device_ui
    assert "display_dim_timeout" not in web_ui
    assert "display_off_timeout" not in web_ui
