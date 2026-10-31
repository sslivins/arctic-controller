"""Tests for the responsive Home dashboard (mirrors the device home screen)."""

import re

from playwright.sync_api import Page, expect

HERO_STATES = ("Disconnected", "Fault", "Standby", "Defrosting", "Heating", "Cooling", "Idle",
               "Freeze protection")


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

    def test_loading_before_first_status(self, dashboard_page: Page):
        """A refresh shows a neutral loading state, not a red "Disconnected",
        until the first heat pump status arrives."""
        held, released = [], []

        def hold(route):
            if released:
                route.continue_()
            else:
                held.append(route)

        dashboard_page.route("**/api/heatpump/status", hold)
        dashboard_page.reload(wait_until="domcontentloaded")
        hero = dashboard_page.locator(".hero")
        expect(hero).to_have_attribute("data-hero-state", "loading", timeout=10000)
        expect(dashboard_page.locator(".hp-status.loading")).to_contain_text("Connecting")
        expect(dashboard_page.locator(".hp-status .dot.bad")).to_have_count(0)
        expect(dashboard_page.locator(".home-banner")).to_have_count(0)
        released.append(True)
        for route in held:
            route.continue_()
        expect(hero).not_to_have_attribute("data-hero-state", "loading", timeout=15000)
        expect(dashboard_page.locator(".hp-status.loading")).to_have_count(0)

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

    def test_trend_arrows(self, dashboard_page: Page):
        """Tank/supply/return show the device's trend; "unknown" shows nothing."""
        def patch(route):
            resp = route.fetch()
            body = resp.json()
            body["connected"] = True
            body["trends"] = {"tank": "rising", "outlet": "falling", "inlet": "unknown"}
            route.fulfill(response=resp, json=body)

        dashboard_page.route("**/api/heatpump/status", patch)
        hero = dashboard_page.locator(".hero-tank .trend")
        expect(hero).to_have_attribute("data-trend", "rising", timeout=15000)
        expect(hero).to_have_text("↑")
        supply = dashboard_page.locator(".tile[data-tile='supply'] .trend")
        expect(supply).to_have_attribute("data-trend", "falling")
        expect(supply).to_have_text("↓")
        expect(dashboard_page.locator(".tile[data-tile='return'] .trend")).to_have_count(0)
        assert hero.evaluate("e => getComputedStyle(e).color") != \
            supply.evaluate("e => getComputedStyle(e).color")

    def test_strip(self, dashboard_page: Page):
        strip = dashboard_page.locator(".home-strip")
        expect(strip).to_be_visible()
        assert re.search(r"Running|Last run", strip.inner_text())

    BODY = "[data-home-chart] .tschart-body"

    def test_chart_renders(self, dashboard_page: Page):
        expect(dashboard_page.locator(self.BODY)).to_be_visible(timeout=15000)
        labels = dashboard_page.locator("[data-home-chart] .tschart-axis-out text")
        expect(labels).to_have_count(5)
        assert all(label.endswith("°") for label in labels.all_inner_texts())
        expect(dashboard_page.locator("[data-home-chart] .tschart-time").last).to_have_text("now")

    def test_chart_hover_tooltip(self, dashboard_page: Page):
        area = dashboard_page.locator(self.BODY)
        expect(area).to_be_visible(timeout=15000)
        box = area.bounding_box()
        dashboard_page.mouse.move(box["x"] + box["width"] * 0.95, box["y"] + box["height"] / 2)
        tip = dashboard_page.locator("[data-home-chart] .tschart-card")
        expect(tip).to_be_visible()
        expect(tip).to_contain_text("Tank")

    def test_chart_tooltip_names_a_defrost(self, dashboard_page: Page):
        """A defrost splits the run it interrupts, so it sits outside every run
        span; the tooltip must still call it a defrost, not "Idle"."""
        span = {}

        def patch(route):
            resp = route.fetch()
            body = resp.json()
            end = body["end"]
            span.update(start=body["start"], end=end)
            body["runs"] = [{"start": end - 3600, "end": end - 1800, "mode": "heating"},
                            {"start": end - 1200, "end": end - 600, "mode": "heating"}]
            body["defrosts"] = [{"start": end - 1800, "end": end - 1200}]
            route.fulfill(response=resp, json=body)

        dashboard_page.route("**/api/heatpump/home-history", patch)
        dashboard_page.reload(wait_until="domcontentloaded")
        area = dashboard_page.locator(self.BODY)
        expect(area).to_be_visible(timeout=15000)
        expect(dashboard_page.locator("[data-home-chart] .tschart-span[data-key='defrost']")).to_have_count(1)
        tip = dashboard_page.locator("[data-home-chart] .tschart-card")
        plot = dashboard_page.locator("[data-home-chart] .tschart-time")
        # Plot edges: the "4 h" label starts at the left edge, "now" ends at the right.
        x0 = plot.first.bounding_box()["x"]
        last = plot.last.bounding_box()
        x1 = last["x"] + last["width"]
        box = area.bounding_box()

        def hover_at(t):
            frac = (t - span["start"]) / (span["end"] - span["start"])
            dashboard_page.mouse.move(x0 + (x1 - x0) * frac, box["y"] + box["height"] / 2)

        hover_at(span["end"] - 1500)
        expect(tip).to_contain_text("Defrosting")
        expect(tip).not_to_contain_text("Idle")
        hover_at(span["end"] - 2700)
        expect(tip).to_contain_text("Heating")

    def _expect_history_cycles(self, page: Page):
        expect(page.get_by_role("heading", name="History", exact=True)).to_be_visible()
        expect(page.locator(".hist-seg-btn", has_text="Cycles")).to_have_attribute("aria-selected", "true")
        expect(page.locator(".rail .nav-link.active", has_text="History")).to_be_visible()

    def test_cycle_history_link(self, dashboard_page: Page):
        dashboard_page.locator(".home-chart").get_by_role("button", name="Cycle history").click()
        self._expect_history_cycles(dashboard_page)

    def test_status_cycle_history_link(self, dashboard_page: Page):
        dashboard_page.locator(".rail .nav-link", has_text="Status").click()
        dashboard_page.get_by_role("button", name="View cycle history").click()
        self._expect_history_cycles(dashboard_page)

    def test_status_survives_poll(self, dashboard_page: Page):
        dashboard_page.wait_for_timeout(5500)
        expect(dashboard_page.locator(".hero")).to_be_visible()
        expect(dashboard_page.locator("[data-home-chart]")).to_be_visible()

    def test_mobile_stacks_hero_chart_tiles(self, dashboard_page: Page):
        dashboard_page.set_viewport_size({"width": 390, "height": 844})
        expect(dashboard_page.locator(self.BODY)).to_be_visible(timeout=15000)
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
        assert all(any(name in label for label in labels) for name in ("Home", "Status", "Control", "History"))

    def test_mobile_bottom_navigation(self, dashboard_page: Page):
        dashboard_page.set_viewport_size({"width": 390, "height": 844})
        expect(dashboard_page.locator(".mobile-nav")).to_be_visible()
        expect(dashboard_page.locator(".rail")).not_to_be_visible()
