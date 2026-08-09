"""Tests for the responsive Home dashboard."""

from playwright.sync_api import Page, expect


class TestHome:
    def test_home_heading_and_hero(self, dashboard_page: Page):
        expect(dashboard_page.get_by_role("heading", name="Home", exact=True)).to_be_visible()
        expect(dashboard_page.locator(".hero")).to_be_visible()

    def test_tank_temperature_is_present(self, dashboard_page: Page):
        text = dashboard_page.locator(".temp-value").inner_text()
        assert any(char.isdigit() for char in text) or "--" in text

    def test_equipment_parity(self, dashboard_page: Page):
        components = dashboard_page.locator(".component")
        assert components.count() == 4
        for label in ("Compressor", "Fan", "Water pump", "Aux heater"):
            expect(dashboard_page.locator(".component", has_text=label)).to_be_visible()

    def test_performance_metrics(self, dashboard_page: Page):
        for label in ("Outdoor", "Water flow", "Power", "Efficiency"):
            expect(dashboard_page.locator(".metric", has_text=label)).to_be_visible()

    def test_problem_summary_and_error_history_link(self, dashboard_page: Page):
        expect(dashboard_page.get_by_role("heading", name="Problems")).to_be_visible()
        expect(dashboard_page.get_by_role("button", name="Error history")).to_be_visible()

    def test_status_survives_poll(self, dashboard_page: Page):
        dashboard_page.wait_for_timeout(5500)
        expect(dashboard_page.locator(".hero")).to_be_visible()


class TestResponsiveShell:
    def test_web_palette_matches_device_identity(self, dashboard_page: Page):
        brand = dashboard_page.locator(".brand-mark")
        assert brand.evaluate(
            "(element) => getComputedStyle(element).backgroundColor"
        ) == "rgb(0, 90, 158)"
        dashboard_page.evaluate(
            "() => document.documentElement.setAttribute('data-theme', 'dark')"
        )
        assert brand.evaluate(
            "(element) => getComputedStyle(element).backgroundColor"
        ) == "rgb(0, 212, 255)"
        assert dashboard_page.locator("body").evaluate(
            "(element) => getComputedStyle(element).backgroundColor"
        ) == "rgb(26, 26, 46)"

    def test_desktop_rail_has_primary_pages(self, dashboard_page: Page):
        labels = dashboard_page.locator(".rail .nav-link").all_inner_texts()
        assert all(any(name in label for label in labels) for name in ("Home", "Status", "Control", "Events"))

    def test_mobile_bottom_navigation(self, dashboard_page: Page):
        dashboard_page.set_viewport_size({"width": 390, "height": 844})
        expect(dashboard_page.locator(".mobile-nav")).to_be_visible()
        expect(dashboard_page.locator(".rail")).not_to_be_visible()
