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

    def test_preferences_form_has_controller_name_field(self, dashboard_page: Page):
        dashboard_page.locator('button[aria-label="Settings"]').click()
        dashboard_page.locator(".settings-nav .nav-link", has_text="Preferences").click()
        dashboard_page.locator(".settings-layout .spinner").wait_for(
            state="detached", timeout=30000
        )
        expect(dashboard_page.locator('input[name="device_name"]')).to_be_visible()

