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
        assert dashboard_page.locator('form[data-form="setpoint"] input[type="range"]').count() == 3
        expect(dashboard_page.locator('form[data-form="setpoint"] output').first).to_contain_text("°")

    def test_advanced_controls_use_bounded_editors(self, dashboard_page: Page):
        primary(dashboard_page, "Control").click()
        dashboard_page.locator(".ap-row").first.wait_for(state="attached")
        assert dashboard_page.locator('.ap-row input[type="number"]').count() == 0
        assert dashboard_page.locator('.ap-row input[type="range"]').count() > 0
        assert dashboard_page.locator(".ap-row select.choice-select").count() > 0

    def test_discrete_control_labels_wrap_on_desktop(self, dashboard_page: Page):
        primary(dashboard_page, "Control").click()
        frequency = dashboard_page.locator("details", has_text="Frequency").first
        expect(frequency).to_be_visible()
        dashboard_page.wait_for_timeout(1000)
        frequency.locator(":scope > summary").click()
        selector = frequency.locator("select.choice-select").first
        description = selector.locator("xpath=following-sibling::*[contains(@class, 'choice-description')]")
        expect(selector).to_be_visible()
        expect(description).to_contain_text("Lowers running frequency")
        selector.select_option(index=2)
        expect(description).to_contain_text("2 steps per 2 Hz")
        assert description.evaluate(
            "(element) => getComputedStyle(element).overflowWrap === 'anywhere'"
        )

    def test_events_surface(self, dashboard_page: Page):
        primary(dashboard_page, "Events").click()
        expect(dashboard_page.locator("#event-search")).to_be_visible()
        expect(dashboard_page.locator("#event-category")).to_be_visible()
        expect(dashboard_page.locator("#event-time")).to_be_visible()

    def test_event_search_keeps_focus_and_clears(self, dashboard_page: Page):
        primary(dashboard_page, "Events").click()
        search = dashboard_page.locator("#event-search")
        search.fill("heat")
        expect(search).to_be_focused()
        expect(search).to_have_value("heat")
        clear = dashboard_page.get_by_role("button", name="Clear event search")
        expect(clear).to_be_visible()
        dashboard_page.wait_for_timeout(5500)
        expect(search).to_be_focused()
        expect(search).to_have_value("heat")
        clear.click()
        expect(search).to_be_focused()
        expect(search).to_have_value("")
        expect(clear).to_be_hidden()
