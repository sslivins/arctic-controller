"""Tests for the settings workspace."""

from playwright.sync_api import Page, expect


def open_settings(page: Page, section: str):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text=section).click()
    page.locator(".settings-nav .nav-link.active", has_text=section).wait_for()
    # settingsPage() renders the nav immediately but shows only a spinner in
    # the panel body until state.settingsLoaded flips (see main/web/index.html).
    # Waiting on the nav link alone therefore races the async load, and the
    # 5s default expect() timeout would intermittently fire against a panel
    # that was still spinning rather than one that was genuinely missing.
    page.locator(".settings-layout .spinner").wait_for(state="detached",
                                                       timeout=30000)


class TestSettingsWorkspace:
    def test_all_sections_present(self, dashboard_page: Page):
        dashboard_page.locator('button[aria-label="Settings"]').click()
        labels = dashboard_page.locator(".settings-nav .nav-link").all_inner_texts()
        assert labels == ["WiFi", "Firmware", "Time & Location", "Display", "Preferences",
                          "Security", "Home Assistant", "Diagnostics", "System"]

    def test_wifi_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "WiFi")
        expect(dashboard_page.get_by_role("button", name="Scan")).to_be_visible()
        expect(dashboard_page.locator('form[data-form="wifi"]')).to_be_visible()

    def test_firmware_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Firmware")
        expect(dashboard_page.get_by_role("button", name="Check for updates")).to_be_visible()
        expect(dashboard_page.locator('input[type="file"]')).to_have_attribute("accept", ".bin,application/octet-stream")

    def test_check_for_updates_shows_progress(self, dashboard_page: Page):
        """The update check takes a couple of seconds; the button must show
        that it's working instead of doing nothing until the result lands."""
        open_settings(dashboard_page, "Firmware")
        held = []
        dashboard_page.route("**/api/ota/releases", lambda route: held.append(route))
        try:
            dashboard_page.get_by_role("button", name="Check for updates").click()
            checking = dashboard_page.get_by_role("button", name="Checking for updates…")
            expect(checking).to_be_visible()
            expect(checking).to_be_disabled()
            expect(checking.locator(".btn-spinner")).to_be_visible()
            expect(dashboard_page.get_by_role("button", name="Check for updates")).to_have_count(0)
            assert len(held) == 1
            held.pop().continue_()
            expect(dashboard_page.get_by_role("button", name="Check for updates")).to_be_enabled(timeout=30000)
            expect(checking).to_have_count(0)
        finally:
            dashboard_page.unroute("**/api/ota/releases")
            for route in held:
                route.continue_()

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
        expect(dashboard_page.locator('a[href="/api/screenshot?format=jpeg"]')).to_be_visible()

    def test_system_and_factory_reset(self, dashboard_page: Page):
        open_settings(dashboard_page, "System")
        expect(dashboard_page.get_by_role("button", name="Restart controller")).to_be_visible()
        expect(dashboard_page.get_by_role("button", name="Erase and reset")).to_be_visible()
