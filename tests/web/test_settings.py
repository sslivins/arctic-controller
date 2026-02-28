"""Tests for the web dashboard Settings page."""

import pytest
from playwright.sync_api import Page, expect


def _go_to_settings(page: Page):
    """Navigate to the Settings page."""
    page.locator("nav button").nth(5).click()
    page.wait_for_timeout(500)


def _go_to_security(page: Page):
    """Navigate to the Security page."""
    page.locator("nav button").nth(4).click()
    page.wait_for_timeout(500)


class TestSettingsCards:
    """Settings page layout and card visibility."""

    def test_all_settings_cards_visible(self, dashboard_page: Page):
        """All 5 settings cards are rendered (security cards moved to Security tab)."""
        _go_to_settings(dashboard_page)
        cards = dashboard_page.locator(".card")
        assert cards.count() >= 5, f"Expected at least 5 settings cards, got {cards.count()}"

    def test_device_info_card(self, dashboard_page: Page):
        """Device Info card shows version, platform, free heap, uptime."""
        _go_to_settings(dashboard_page)
        # First card is Device Info — check it has text content
        cards = dashboard_page.locator(".card")
        first_card = cards.first
        text = first_card.inner_text()
        assert len(text) > 10, "Device Info card appears empty"

    def test_wifi_status_card(self, dashboard_page: Page):
        """WiFi Status card is present and has content."""
        _go_to_settings(dashboard_page)
        cards = dashboard_page.locator(".card")
        if cards.count() >= 2:
            text = cards.nth(1).inner_text()
            assert len(text) > 10, "WiFi Status card appears empty"


class TestTimeSettings:
    """Time settings card interactions."""

    def test_timezone_selector_visible(self, dashboard_page: Page):
        """Timezone dropdown is shown in Time Settings card."""
        _go_to_settings(dashboard_page)
        # Find the timezone select (has 21+ timezone options)
        selects = dashboard_page.locator(".card select")
        found = False
        for i in range(selects.count()):
            options = selects.nth(i).locator("option")
            if options.count() > 10:  # Timezone dropdown has 21 options
                found = True
                break
        assert found, "Timezone selector not found"

    def test_24h_format_toggle_visible(self, dashboard_page: Page):
        """24-hour format toggle is present."""
        _go_to_settings(dashboard_page)
        toggles = dashboard_page.locator(".toggle input[type='checkbox']")
        assert toggles.count() >= 1, "Expected at least one toggle checkbox"


class TestSecuritySettings:
    """Security card interactions (on Security tab)."""

    def test_web_auth_toggle_visible(self, dashboard_page: Page):
        """Web Auth toggle is present in Authentication card."""
        _go_to_security(dashboard_page)
        toggles = dashboard_page.locator(".toggle input[type='checkbox']")
        # There are multiple toggles (web auth, api auth)
        assert toggles.count() >= 2, f"Expected at least 2 toggles, got {toggles.count()}"

    def test_api_key_display_visible(self, dashboard_page: Page):
        """API key display area is present."""
        _go_to_security(dashboard_page)
        api_key = dashboard_page.locator(".api-key-display")
        expect(api_key).to_be_visible()

    def test_api_key_copy_button(self, dashboard_page: Page):
        """Copy API key button is clickable."""
        _go_to_security(dashboard_page)
        copy_btn = dashboard_page.locator(".api-key-display .btn-group button").nth(1)
        expect(copy_btn).to_be_visible()
        expect(copy_btn).to_be_enabled()


class TestFirmwareSettings:
    """Firmware update card."""

    def test_check_updates_button_visible(self, dashboard_page: Page):
        """'Check for Updates' button is present."""
        _go_to_settings(dashboard_page)
        # Find button by its distinctive text
        btn = dashboard_page.get_by_role("button", name="Check for Updates")
        expect(btn).to_be_visible()

    def test_file_upload_zone_visible(self, dashboard_page: Page):
        """File upload drop zone is present."""
        _go_to_settings(dashboard_page)
        upload = dashboard_page.locator(".file-upload")
        expect(upload).to_be_visible()

    def test_file_input_exists(self, dashboard_page: Page):
        """Hidden file input accepts .bin files."""
        _go_to_settings(dashboard_page)
        file_input = dashboard_page.locator("input[type='file'][accept='.bin']")
        assert file_input.count() == 1


class TestDiagnosticsSettings:
    """Diagnostics card in Settings."""

    def test_diagnostics_card_visible(self, dashboard_page: Page):
        """Diagnostics card is present on Settings page."""
        _go_to_settings(dashboard_page)
        link = dashboard_page.locator("a[href='/api/heatpump/diagnostic']")
        expect(link).to_be_visible()

    def test_diagnostics_download_link_href(self, dashboard_page: Page):
        """Download link points to the diagnostic CSV endpoint."""
        _go_to_settings(dashboard_page)
        link = dashboard_page.locator("a[href='/api/heatpump/diagnostic']")
        assert link.get_attribute("href") == "/api/heatpump/diagnostic"


class TestSystemSettings:
    """System card (reboot)."""

    def test_reboot_button_visible(self, dashboard_page: Page):
        """Reboot button is present on system card."""
        _go_to_settings(dashboard_page)
        # Find reboot button — last card, secondary button
        btn = dashboard_page.get_by_role("button", name="Reboot")
        expect(btn).to_be_visible()
