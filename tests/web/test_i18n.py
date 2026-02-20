"""Tests for web dashboard internationalization (i18n)."""

import pytest
from playwright.sync_api import Page, expect


class TestLanguageSwitching:
    """Verify language switching works across the dashboard."""

    def _get_header_lang_select(self, page: Page):
        return page.locator(".header-right .lang-selector select")

    def test_language_selector_visible(self, dashboard_page: Page):
        """Language selector is shown in the header."""
        expect(self._get_header_lang_select(dashboard_page)).to_be_visible()

    def test_default_language_english(self, dashboard_page: Page):
        """Default language is English."""
        select = self._get_header_lang_select(dashboard_page)
        value = select.input_value()
        assert value == "en", f"Expected default language 'en', got '{value}'"

    def test_switch_to_french(self, dashboard_page: Page):
        """Switching to French updates nav button text."""
        select = self._get_header_lang_select(dashboard_page)
        select.select_option("fr")
        dashboard_page.wait_for_timeout(300)

        # First nav button should now say "Tableau de bord" (French for Dashboard)
        first_nav = dashboard_page.locator("nav button").nth(0)
        text = first_nav.inner_text()
        assert text != "Dashboard", f"Expected French text, still got: {text}"

        # Restore English
        select.select_option("en")
        dashboard_page.wait_for_timeout(300)

    def test_switch_to_spanish(self, dashboard_page: Page):
        """Switching to Spanish updates nav button text."""
        select = self._get_header_lang_select(dashboard_page)
        select.select_option("es")
        dashboard_page.wait_for_timeout(300)

        first_nav = dashboard_page.locator("nav button").nth(0)
        text = first_nav.inner_text()
        assert text != "Dashboard", f"Expected Spanish text, still got: {text}"

        # Restore English
        select.select_option("en")
        dashboard_page.wait_for_timeout(300)

    def test_language_persists_across_reload(self, dashboard_page: Page):
        """Selected language survives a page reload (localStorage)."""
        select = self._get_header_lang_select(dashboard_page)
        select.select_option("fr")
        dashboard_page.wait_for_timeout(300)

        # Reload the page
        dashboard_page.reload(wait_until="networkidle")
        dashboard_page.wait_for_selector("nav", timeout=30000)

        # Should still be French
        value = self._get_header_lang_select(dashboard_page).input_value()
        assert value == "fr", f"Expected language 'fr' after reload, got '{value}'"

        # Restore English
        self._get_header_lang_select(dashboard_page).select_option("en")
        dashboard_page.wait_for_timeout(300)
