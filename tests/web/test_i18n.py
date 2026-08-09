"""Tests for device language and temperature-unit preferences."""

from playwright.sync_api import Page, expect


def open_preferences(page: Page):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text="Preferences").click()


class TestPreferences:
    def test_language_options(self, dashboard_page: Page):
        open_preferences(dashboard_page)
        select = dashboard_page.locator('select[name="language"]')
        expect(select).to_be_visible()
        assert select.locator("option").count() == 3

    def test_temperature_unit_options(self, dashboard_page: Page):
        open_preferences(dashboard_page)
        select = dashboard_page.locator('select[name="temp_unit"]')
        expect(select).to_be_visible()
        assert select.locator("option").count() == 2

    def test_demo_mode_control(self, dashboard_page: Page):
        open_preferences(dashboard_page)
        expect(dashboard_page.locator('input[name="demo_mode"]')).to_be_visible()
