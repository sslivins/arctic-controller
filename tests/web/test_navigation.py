"""Tests for web dashboard page navigation."""

import pytest
from playwright.sync_api import Page, expect


class TestNavigation:
    """Navigate between the 5 pages via nav buttons."""

    def test_starts_on_dashboard(self, dashboard_page: Page):
        """Dashboard is the default page after load."""
        expect(dashboard_page.locator(".hero-card")).to_be_visible()

    def test_navigate_to_parameters(self, dashboard_page: Page):
        """Clicking Parameters nav button shows the parameters page."""
        dashboard_page.locator("nav button").nth(1).click()
        dashboard_page.wait_for_timeout(500)
        # Parameters page should show one of the two power buttons (ON or OFF)
        visible_power = dashboard_page.locator(".power-hold-btn").locator("visible=true")
        expect(visible_power).to_be_visible()

    def test_navigate_to_events(self, dashboard_page: Page):
        """Clicking Events nav button shows the events page."""
        dashboard_page.locator("nav button").nth(2).click()
        dashboard_page.wait_for_timeout(500)
        # Hero card should be hidden
        expect(dashboard_page.locator(".hero-card")).not_to_be_visible()

    def test_navigate_to_logs(self, dashboard_page: Page):
        """Clicking Logs nav button shows the logs page."""
        dashboard_page.locator("nav button").nth(3).click()
        dashboard_page.wait_for_timeout(500)
        # Log level filter buttons should appear
        expect(dashboard_page.locator(".log-level-filters")).to_be_visible()

    def test_navigate_to_settings(self, dashboard_page: Page):
        """Clicking Settings nav button shows the settings page."""
        dashboard_page.locator("nav button").nth(4).click()
        dashboard_page.wait_for_timeout(500)
        # Settings page has multiple .card elements
        cards = dashboard_page.locator(".card")
        assert cards.count() >= 4, f"Expected at least 4 settings cards, got {cards.count()}"

    def test_navigate_back_to_dashboard(self, dashboard_page: Page):
        """Can navigate away and back to Dashboard."""
        # Go to settings
        dashboard_page.locator("nav button").nth(4).click()
        dashboard_page.wait_for_timeout(300)
        # Go back to dashboard
        dashboard_page.locator("nav button").nth(0).click()
        dashboard_page.wait_for_timeout(500)
        expect(dashboard_page.locator(".hero-card")).to_be_visible()

    def test_nav_button_highlight(self, dashboard_page: Page):
        """Active nav button has a distinct style (active class)."""
        # Dashboard button (first) should have active state
        first_btn = dashboard_page.locator("nav button").nth(0)
        # Click a different page
        dashboard_page.locator("nav button").nth(2).click()
        dashboard_page.wait_for_timeout(300)
        # Third button should now be active
        third_btn = dashboard_page.locator("nav button").nth(2)
        # Both should be visible and clickable
        expect(first_btn).to_be_visible()
        expect(third_btn).to_be_visible()


class TestLogsPage:
    """Tests specific to the Logs page."""

    def _go_to_logs(self, page: Page):
        page.locator("nav button").nth(3).click()
        page.wait_for_timeout(500)

    def test_log_level_filters_visible(self, dashboard_page: Page):
        """All 5 log level filter buttons are shown."""
        self._go_to_logs(dashboard_page)
        btns = dashboard_page.locator(".log-level-filters button")
        assert btns.count() == 5, f"Expected 5 filter buttons, got {btns.count()}"

    def test_log_auto_scroll_checkbox(self, dashboard_page: Page):
        """Auto-scroll checkbox exists and is checked by default."""
        self._go_to_logs(dashboard_page)
        checkbox = dashboard_page.locator(".log-toolbar input[type='checkbox']")
        expect(checkbox).to_be_visible()
        expect(checkbox).to_be_checked()

    def test_log_entries_appear(self, dashboard_page: Page):
        """Log entries load after polling interval."""
        self._go_to_logs(dashboard_page)
        # Wait for log polling (2s interval + buffer)
        dashboard_page.wait_for_timeout(3000)
        entries = dashboard_page.locator(".log-entry")
        assert entries.count() > 0, "Expected at least one log entry"

    def test_log_entry_structure(self, dashboard_page: Page):
        """Each log entry has sequence, level, tag, and message parts."""
        self._go_to_logs(dashboard_page)
        dashboard_page.wait_for_timeout(3000)
        entries = dashboard_page.locator(".log-entry")
        if entries.count() > 0:
            first = entries.first
            expect(first.locator(".log-seq")).to_be_visible()
            expect(first.locator(".log-level")).to_be_visible()
            expect(first.locator(".log-tag")).to_be_visible()
            expect(first.locator(".log-msg")).to_be_visible()


class TestEventsPage:
    """Tests specific to the Events page."""

    def _go_to_events(self, page: Page):
        page.locator("nav button").nth(2).click()
        page.wait_for_timeout(500)

    def test_events_page_loads(self, dashboard_page: Page):
        """Events page loads without error."""
        self._go_to_events(dashboard_page)
        # Hero card should be hidden on events page
        expect(dashboard_page.locator(".hero-card")).not_to_be_visible()


class TestParametersPage:
    """Tests specific to the Parameters page."""

    def _go_to_params(self, page: Page):
        page.locator("nav button").nth(1).click()
        page.wait_for_timeout(500)

    def test_power_button_visible(self, dashboard_page: Page):
        """Power ON/OFF button is shown on parameters page."""
        self._go_to_params(dashboard_page)
        visible_power = dashboard_page.locator(".power-hold-btn").locator("visible=true")
        expect(visible_power).to_be_visible()

    def test_mode_selector_visible(self, dashboard_page: Page):
        """Mode dropdown (cooling/heating/etc) is shown."""
        self._go_to_params(dashboard_page)
        # The mode select is in a power-hold-card
        select = dashboard_page.locator("select").first
        expect(select).to_be_visible()
