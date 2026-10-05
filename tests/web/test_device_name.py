"""Web UI friendly-name rendering."""

import json

from playwright.sync_api import Page, expect


PREFS_WITH_NAME = {
    "demo_mode": True,
    "temp_unit": "celsius",
    "device_name": "Heat Pump 1 – Radiant Floor",
    "brightness": 80,
    "language": "English",
    "language_code": "en",
    "format_24h": True,
    "timezone": "UTC0",
}


class TestDeviceNameWeb:
    def test_header_and_title_show_controller_name(self, dashboard_page: Page):
        dashboard_page.route("**/api/preferences", lambda route: route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps(PREFS_WITH_NAME),
        ))
        dashboard_page.reload(wait_until="domcontentloaded")
        dashboard_page.wait_for_selector(".rail", timeout=10000)

        expect(dashboard_page.locator(".brand")).to_contain_text(
            "Heat Pump 1 – Radiant Floor · Arctic Controller"
        )
        expect(dashboard_page).to_have_title(
            "Heat Pump 1 – Radiant Floor · Arctic Controller"
        )
        # Wide screens show the name in the top bar, so no separate line.
        expect(dashboard_page.locator(".name-strip")).to_be_hidden()

    def test_narrow_screen_shows_name_below_top_bar(self, dashboard_page: Page):
        # The top bar hides its title on narrow screens; the name moves to a
        # line underneath it instead of disappearing.
        dashboard_page.set_viewport_size({"width": 390, "height": 844})
        dashboard_page.route("**/api/preferences", lambda route: route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps(PREFS_WITH_NAME),
        ))
        dashboard_page.reload(wait_until="domcontentloaded")
        dashboard_page.wait_for_selector(".mobile-nav", timeout=10000)

        strip = dashboard_page.locator(".name-strip")
        expect(strip).to_be_visible()
        expect(strip).to_have_text("Heat Pump 1 – Radiant Floor")
        top_bar = dashboard_page.locator(".topbar").bounding_box()
        box = strip.bounding_box()
        assert box["y"] >= top_bar["y"] + top_bar["height"] - 1

    def test_no_name_strip_without_a_name(self, dashboard_page: Page):
        dashboard_page.set_viewport_size({"width": 390, "height": 844})
        dashboard_page.route("**/api/preferences", lambda route: route.fulfill(
            status=200,
            content_type="application/json",
            body=json.dumps({**PREFS_WITH_NAME, "device_name": ""}),
        ))
        dashboard_page.reload(wait_until="domcontentloaded")
        dashboard_page.wait_for_selector(".mobile-nav", timeout=10000)
        expect(dashboard_page.locator(".name-strip")).to_have_count(0)

    def test_preferences_form_has_controller_name_field(self, dashboard_page: Page):
        dashboard_page.locator('button[aria-label="Settings"]').click()
        dashboard_page.locator(".settings-nav .nav-link", has_text="Preferences").click()
        dashboard_page.locator(".settings-layout .spinner").wait_for(
            state="detached", timeout=30000
        )
        expect(dashboard_page.locator('input[name="device_name"]')).to_be_visible()

