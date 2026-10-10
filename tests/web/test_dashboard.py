"""Tests for the responsive Home dashboard (mirrors the device home screen)."""

import re

from playwright.sync_api import Page, expect

HERO_STATES = ("Disconnected", "Fault", "Standby", "Defrosting", "Heating", "Cooling", "Idle")


class TestHome:
    def test_home_heading_and_hero(self, dashboard_page: Page):
        expect(dashboard_page.get_by_role("heading", name="Home", exact=True)).to_be_visible()
        expect(dashboard_page.locator(".hero")).to_be_visible()

    def test_hero_state_label(self, dashboard_page: Page):
        text = dashboard_page.locator(".hero-state").inner_text()
        assert text.split(" · ")[0] in HERO_STATES, text

    def test_tank_temperature_is_present(self, dashboard_page: Page):
        text = dashboard_page.locator(".temp-value").inner_text()
        assert any(char.isdigit() for char in text) or "--" in text

    def test_component_pills(self, dashboard_page: Page):
        if dashboard_page.locator(".hero[data-hero-state='disconnected']").count():
            assert dashboard_page.locator(".pill").count() == 0
            return
        for kind in ("compressor", "fan", "pump"):
            expect(dashboard_page.locator(f".pill[data-pill='{kind}']")).to_be_visible()
        expect(dashboard_page.locator(".pill[data-pill='fan'] .fan-bars i")).to_have_count(3)

    def test_tiles(self, dashboard_page: Page):
        for key in ("supply", "return", "power"):
            expect(dashboard_page.locator(f".tile[data-tile='{key}']")).to_be_visible()
        running = dashboard_page.locator(".hero.s-heating, .hero.s-cooling, .hero.s-defrost").count() > 0
        for key in ("dt", "cop", "hz"):
            assert (dashboard_page.locator(f".tile[data-tile='{key}']").count() == 1) == running

    def test_strip(self, dashboard_page: Page):
        strip = dashboard_page.locator(".home-strip")
        expect(strip).to_be_visible()
        assert re.search(r"Running|Last run", strip.inner_text())

    def test_chart_renders(self, dashboard_page: Page):
        chart = dashboard_page.locator("[data-home-chart] .hc-area")
        expect(chart).to_be_visible(timeout=15000)
        labels = dashboard_page.locator(".hc-y").all_inner_texts()
        assert len(labels) == 5 and all(label.endswith("°") for label in labels), labels
        expect(dashboard_page.locator(".hc-x").last).to_have_text("now")

    def test_chart_hover_tooltip(self, dashboard_page: Page):
        area = dashboard_page.locator("[data-home-chart] .hc-area")
        expect(area).to_be_visible(timeout=15000)
        box = area.bounding_box()
        dashboard_page.mouse.move(box["x"] + box["width"] * 0.95, box["y"] + box["height"] / 2)
        tip = dashboard_page.locator("[data-home-chart] .hist-tip")
        expect(tip).to_be_visible()
        expect(tip).to_contain_text("Tank")

    def test_cycle_history_link(self, dashboard_page: Page):
        dashboard_page.locator(".home-chart").get_by_role("button", name="Cycle history").click()
        expect(dashboard_page.get_by_role("heading", name="Cycle history")).to_be_visible()

    def test_cycle_history_back_returns_home(self, dashboard_page: Page):
        dashboard_page.locator(".home-chart").get_by_role("button", name="Cycle history").click()
        expect(dashboard_page.get_by_role("heading", name="Cycle history")).to_be_visible()
        dashboard_page.locator(".history-back").click()
        expect(dashboard_page.get_by_role("heading", name="Home", exact=True)).to_be_visible()

    def test_cycle_history_back_returns_status(self, dashboard_page: Page):
        dashboard_page.locator(".rail .nav-link", has_text="Status").click()
        dashboard_page.get_by_role("button", name="View cycle history").click()
        expect(dashboard_page.get_by_role("heading", name="Cycle history")).to_be_visible()
        dashboard_page.locator(".history-back").click()
        expect(dashboard_page.get_by_role("heading", name="Status", exact=True)).to_be_visible()

    def test_status_survives_poll(self, dashboard_page: Page):
        dashboard_page.wait_for_timeout(5500)
        expect(dashboard_page.locator(".hero")).to_be_visible()
        expect(dashboard_page.locator("[data-home-chart]")).to_be_visible()

    def test_mobile_stacks_hero_chart_tiles(self, dashboard_page: Page):
        dashboard_page.set_viewport_size({"width": 390, "height": 844})
        expect(dashboard_page.locator("[data-home-chart] .hc-area")).to_be_visible(timeout=15000)
        tops = [dashboard_page.locator(sel).bounding_box()["y"] for sel in (".home-hero", ".home-chart", ".tiles", ".home-strip")]
        assert tops == sorted(tops), tops
        width = dashboard_page.evaluate("() => document.documentElement.scrollWidth")
        assert width <= 390, width


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
