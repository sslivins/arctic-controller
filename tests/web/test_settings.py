"""Tests for the settings workspace."""

from playwright.sync_api import Page, expect


def open_settings(page: Page, section: str):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text=section).click()
    page.locator(".settings-nav .nav-link.active", has_text=section).wait_for()


class TestSettingsWorkspace:
    def test_all_sections_present(self, dashboard_page: Page):
        dashboard_page.locator('button[aria-label="Settings"]').click()
        labels = dashboard_page.locator(".settings-nav .nav-link").all_inner_texts()
        assert labels == ["WiFi", "Firmware", "Time", "Display", "Preferences",
                          "Security", "Diagnostics", "System"]

    def test_wifi_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "WiFi")
        expect(dashboard_page.get_by_role("button", name="Scan")).to_be_visible()
        expect(dashboard_page.locator('form[data-form="wifi"]')).to_be_visible()

    def test_firmware_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Firmware")
        expect(dashboard_page.get_by_role("button", name="Check for updates")).to_be_visible()
        expect(dashboard_page.locator('input[type="file"]')).to_have_attribute("accept", ".bin,application/octet-stream")

    def test_time_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Time")
        expect(dashboard_page.locator('select[name="timezone"]')).to_be_visible()
        expect(dashboard_page.get_by_role("button", name="Sync now")).to_be_visible()

    def test_display_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Display")
        slider = dashboard_page.locator('input[name="brightness"]')
        expect(slider).to_be_visible()
        expect(slider).to_have_attribute("min", "5")
        expect(slider).to_have_attribute("max", "100")

    def test_security_and_tls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Security")
        tls_form = dashboard_page.locator('form[data-form="tls"]')
        expect(
            dashboard_page.locator(
                ".notice",
                has_text="Web login and API-key authentication are always required",
            )
        ).to_be_visible()
        expect(tls_form).to_be_visible()
        assert tls_form.locator("textarea").count() == 2

    def test_diagnostics(self, dashboard_page: Page):
        open_settings(dashboard_page, "Diagnostics")
        expect(dashboard_page.locator("#log-container")).to_be_visible()
        expect(dashboard_page.locator('a[href="/api/heatpump/diagnostic"]')).to_be_visible()
        expect(dashboard_page.locator('a[href="/api/screenshot"]')).to_be_visible()

    def test_system_and_factory_reset(self, dashboard_page: Page):
        open_settings(dashboard_page, "System")
        expect(dashboard_page.get_by_role("button", name="Restart controller")).to_be_visible()
        expect(dashboard_page.get_by_role("button", name="Erase and reset")).to_be_visible()
