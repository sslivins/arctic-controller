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
        assert dashboard_page.locator(".mode-btn").count() == 4
        assert dashboard_page.locator('form[data-form="setpoint"]').count() == 3
        assert dashboard_page.locator('form[data-form="setpoint"] input[type="range"]').count() == 3
        expect(dashboard_page.locator('form[data-form="setpoint"] output').first).to_contain_text("°")

    def test_advanced_controls_use_bounded_editors(self, dashboard_page: Page):
        primary(dashboard_page, "Control").click()
        dashboard_page.locator(".ap-row").first.wait_for(state="attached")
        heading = dashboard_page.locator(".ap-group > summary").first
        expect(heading).to_be_visible()
        assert heading.evaluate(
            "(element) => getComputedStyle(element).backgroundColor !== getComputedStyle(element.closest('.card')).backgroundColor"
        )
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

    def test_open_filter_dropdown_survives_background_refresh(self, dashboard_page: Page):
        """An open category/time dropdown has focus; the 5 s poll must not rebuild it (which closed it)."""
        primary(dashboard_page, "Events").click()
        expect(dashboard_page.locator("#event-list")).to_be_visible()
        for select_id in ("event-category", "event-time"):
            select = dashboard_page.locator(f"#{select_id}")
            select.focus()
            select.evaluate("el => { el.dataset.sameElement = '1'; }")
            # Only the background poll fetches events on this page; let it land and be applied.
            with dashboard_page.expect_request_finished(lambda r: "/api/events?" in r.url, timeout=15000):
                pass
            dashboard_page.evaluate("() => new Promise(requestAnimationFrame)")
            expect(select).to_be_focused()
            expect(select).to_have_attribute("data-same-element", "1")
            select.blur()


FAKE_ERRORS = {
    "demo_mode": True, "connected": True, "has_errors": True, "error_count": 2,
    "highest_severity": "critical",
    "active": [
        {"code": "P02", "name": "HIGH_PRESSURE", "description": "Refrigerant pressure too high",
         "resolution": "Check the water flow.", "severity": "critical", "active": True, "occurred": None,
         "help_url": "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832838"},
        {"code": "X99", "name": "BOGUS", "description": "Link that isn't Arctic's", "severity": "warning",
         "active": True, "occurred": None, "help_url": "https://example.com/phish"},
    ],
    "history": [],
}


class TestErrorHelpLinks:
    def test_errors_link_to_arctic_troubleshooting_articles(self, dashboard_page: Page):
        import json
        dashboard_page.route("**/api/heatpump/errors", lambda route: route.fulfill(
            status=200, content_type="application/json", body=json.dumps(FAKE_ERRORS)))
        dashboard_page.evaluate("location.hash = '#/errors'")
        dashboard_page.reload()
        expect(dashboard_page.get_by_role("heading", name="Errors", exact=True)).to_be_visible()
        links = dashboard_page.locator("a[data-help-url]")
        expect(links).to_have_count(1)
        expect(links.first).to_have_text("Troubleshooting guide ↗")
        expect(links.first).to_have_attribute(
            "href", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832838")
        expect(links.first).to_have_attribute("target", "_blank")
        expect(links.first).to_have_attribute("rel", "noopener noreferrer")


def _fake_events():
    """A fixed event log covering every detail format, dated relative to now."""
    import datetime as dt
    import time

    now = int(time.time())
    yesterday_noon = int(dt.datetime.combine(
        dt.date.today() - dt.timedelta(days=1), dt.time(12, 0)).timestamp())
    last_year = dt.date.today().year - 1
    old = int(dt.datetime(last_year, 3, 4, 12, 0).timestamp())
    cur, prev = 111, 222

    def ev(type_, category, ts, boot, **extra):
        return {"type": type_, "category": category, "timestamp": ts, "boot_id": boot,
                "uptime_ms": 120000, "payload": 0, **extra}

    events = [
        ev("error_appeared", "problems", now, cur, fault_code="P02",
           fault_label="High pressure protection",
           help_url="https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832838"),
        ev("setpoint_changed", "changes", now - 1, cur, setpoint="hot_water", **{"from": 50, "to": 45}),
        ev("mode_changed", "changes", now - 2, cur, from_mode="hot_water", to_mode="heating"),
        ev("compressor_on", "equipment", now - 3, cur),
        ev("watchdog_reset", "problems", yesterday_noon, prev, watchdog="task"),
        ev("network_recovered", "problems", yesterday_noon - 60, prev, recovery="wifi_bounce"),
        ev("system_start", "system", old, prev),
        ev("pump_on", "equipment", 0, prev),
    ]
    body = {"total": len(events), "offset": 0, "count": len(events), "current_boot_id": cur,
            "brownout_count": 0, "last_reset_reason": "sw", "events": events}
    return body, f"Mar 4, {last_year}"


class TestEventsPage:
    """The Events page reads like the device's Events screen."""

    def _open(self, page: Page):
        import json
        body, old_day = _fake_events()
        page.route("**/api/events?*", lambda route: route.fulfill(
            status=200, content_type="application/json", body=json.dumps(body)))
        primary(page, "Events").click()
        expect(page.locator("#event-list .event")).to_have_count(len(body["events"]))
        return old_day

    def _details(self, page: Page):
        return page.locator("#event-list .event-detail").all_inner_texts()

    def test_events_are_grouped_by_day(self, dashboard_page: Page):
        old_day = self._open(dashboard_page)
        expect(dashboard_page.locator("#event-list .event-day")).to_have_text(
            ["Today", "Yesterday", old_day, "Before the clock was set"])

    def test_details_are_readable(self, dashboard_page: Page):
        self._open(dashboard_page)
        details = self._details(dashboard_page)
        assert "(P02) High pressure protection" in details
        assert "Hot water → Heating" in details
        assert "Task watchdog" in details
        assert "Restored by reconnecting WiFi" in details
        assert any(d in details for d in ("Hot water: 50°C → 45°C", "Hot water: 122°F → 113°F")), details

    def test_no_boot_numbers_and_no_empty_details(self, dashboard_page: Page):
        self._open(dashboard_page)
        text = dashboard_page.locator("#event-list").inner_text()
        assert not re.search(r"\bBoot\b|\b111\b|\b222\b", text), text
        compressor = dashboard_page.locator('.event[data-event-type="compressor_on"]')
        expect(compressor.locator(".event-title")).to_have_text("Compressor started")
        expect(compressor.locator(".event-detail")).to_have_count(0)
        expect(dashboard_page.locator('.event[data-event-type="pump_on"] .event-time')).to_have_text("0h 2m after start")

    def test_problem_events_are_coloured_and_link_to_help(self, dashboard_page: Page):
        self._open(dashboard_page)
        problem = dashboard_page.locator('.event[data-event-type="error_appeared"]')
        expect(problem).to_have_class(re.compile(r"\btone-bad\b"))
        link = dashboard_page.locator("#event-list a[data-help-url]")
        expect(link).to_have_count(1)
        expect(link).to_have_attribute(
            "href", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832838")

    def test_filters(self, dashboard_page: Page):
        self._open(dashboard_page)
        events = dashboard_page.locator("#event-list .event")
        time_filter = dashboard_page.locator("#event-time")
        assert time_filter.locator("option").all_inner_texts() == [
            "All time", "Today", "Last 24 hours", "Last 7 days", "Current boot"]
        time_filter.select_option("boot")
        expect(events).to_have_count(4)
        time_filter.select_option("today")
        expect(events).to_have_count(4)
        dashboard_page.locator("#event-time").select_option("all")
        dashboard_page.locator("#event-category").select_option("problems")
        expect(events).to_have_count(3)
        dashboard_page.locator("#event-category").select_option("all")
        dashboard_page.locator("#event-search").fill("P02")
        expect(events).to_have_count(1)
