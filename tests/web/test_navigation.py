"""Tests for centralized hash navigation and page-specific loading."""

import re
from playwright.sync_api import Page, expect


def primary(page: Page, name: str):
    return page.locator(".rail .nav-link", has_text=name)


class TestNavigation:
    def test_default_route(self, dashboard_page: Page):
        expect(dashboard_page.get_by_role("heading", name="Home", exact=True)).to_be_visible()

    def test_primary_routes(self, dashboard_page: Page):
        for name in ("Status", "Control", "Events"):
            primary(dashboard_page, name).click()
            expect(dashboard_page.get_by_role("heading", name=name, exact=True)).to_be_visible()
            assert dashboard_page.url.endswith(f"#/{name.lower()}")

    def test_active_route_highlight(self, dashboard_page: Page):
        primary(dashboard_page, "Status").click()
        expect(primary(dashboard_page, "Status")).to_have_class(re.compile(r"\bactive\b"))

    def test_settings_deep_link(self, dashboard_page: Page):
        dashboard_page.locator('button[aria-label="Settings"]').click()
        expect(dashboard_page.get_by_role("heading", name="Settings", exact=True)).to_be_visible()
        dashboard_page.locator(".settings-nav .nav-link", has_text="Diagnostics").click()
        expect(dashboard_page.get_by_role("heading", name="Diagnostics")).to_be_visible()
        assert dashboard_page.url.endswith("#/settings/diagnostics")

    def test_control_surface(self, dashboard_page: Page):
        primary(dashboard_page, "Control").click()
        expect(dashboard_page.locator(".power-btn")).to_be_visible()
        assert dashboard_page.locator(".mode-btn").count() == 5
        assert dashboard_page.locator('form[data-form="setpoint"]').count() == 3

    def test_events_surface(self, dashboard_page: Page):
        primary(dashboard_page, "Events").click()
        expect(dashboard_page.locator("#event-search")).to_be_visible()
        expect(dashboard_page.locator("#event-category")).to_be_visible()
        expect(dashboard_page.locator("#event-time")).to_be_visible()
