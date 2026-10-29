"""Tests for centralized hash navigation and page-specific loading."""

import re
from playwright.sync_api import Page, expect


def primary(page: Page, name: str):
    return page.locator(".rail .nav-link", has_text=name)


def history_seg(page: Page, name: str):
    return page.locator(".hist-seg-btn", has_text=name)


def open_events(page: Page) -> None:
    primary(page, "History").click()
    history_seg(page, "Events").click()
    expect(history_seg(page, "Events")).to_have_attribute("aria-selected", "true")


class TestNavigation:
    def test_default_route(self, dashboard_page: Page):
        expect(dashboard_page.get_by_role("heading", name="Home", exact=True)).to_be_visible()

    def test_primary_routes(self, dashboard_page: Page):
        for name in ("Status", "Control", "History"):
            primary(dashboard_page, name).click()
            expect(dashboard_page.get_by_role("heading", name=name, exact=True)).to_be_visible()
            assert dashboard_page.url.endswith(f"#/{name.lower()}")
        expect(history_seg(dashboard_page, "Cycles")).to_have_attribute("aria-selected", "true")

    def test_history_switches_segments_and_remembers_last(self, dashboard_page: Page):
        open_events(dashboard_page)
        expect(dashboard_page.locator("#event-search")).to_be_visible()
        assert dashboard_page.url.endswith("#/events")
        expect(primary(dashboard_page, "History")).to_have_class(re.compile(r"\bactive\b"))
        primary(dashboard_page, "Home").click()
        primary(dashboard_page, "History").click()
        expect(history_seg(dashboard_page, "Events")).to_have_attribute("aria-selected", "true")
        history_seg(dashboard_page, "Cycles").click()
        expect(history_seg(dashboard_page, "Cycles")).to_have_attribute("aria-selected", "true")
        assert dashboard_page.url.endswith("#/history")

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

    def test_advanced_save_gated_and_keeps_group_open(self, dashboard_page: Page):
        primary(dashboard_page, "Control").click()
        group = dashboard_page.locator("details.ap-group", has_text="Defrost").first
        group.locator(":scope > summary").click()
        row = group.locator('form[data-key="AP29"]')
        save = row.locator('button[type="submit"]')
        slider = row.locator('input[type="range"]')
        expect(save).to_be_disabled()

        original = slider.input_value()
        changed = str(int(original) - 1 if int(original) > 0 else 1)
        set_value = "(el, v) => { el.value = v; el.dispatchEvent(new Event('input', { bubbles: true })); }"
        slider.evaluate(set_value, changed)
        expect(save).to_be_enabled()
        slider.evaluate(set_value, original)
        expect(save).to_be_disabled()

        try:
            slider.evaluate(set_value, changed)
            with dashboard_page.expect_response(lambda r: "/api/heatpump/advanced/AP29" in r.url and r.request.method == "PUT"):
                save.click()
            expect(group).to_have_attribute("open", "")
            expect(slider).to_have_value(changed)
            expect(save).to_be_disabled()
        finally:
            slider.evaluate(set_value, original)
            with dashboard_page.expect_response(lambda r: "/api/heatpump/advanced/AP29" in r.url and r.request.method == "PUT"):
                save.click()

        events = dashboard_page.request.get(dashboard_page.url.split("#")[0].rstrip("/") + "/api/events?limit=5").json()
        logged = [e for e in events["events"] if e["type"] == "param_changed" and e.get("param") == "AP29"]
        assert logged and logged[0]["from"] == int(changed) and logged[0]["to"] == int(original), logged

    def test_advanced_revert_and_use_default(self, dashboard_page: Page):
        primary(dashboard_page, "Control").click()
        group = dashboard_page.locator("details.ap-group", has_text="Defrost").first
        group.locator(":scope > summary").click()
        row = group.locator('form[data-key="AP29"]')
        save = row.locator('button[type="submit"]')
        revert = row.locator('[data-action="form-revert"]')
        use_default = row.locator('[data-action="ap-default"]')
        slider = row.locator('input[type="range"]')
        output = row.locator("[data-range-output]")
        default = use_default.get_attribute("data-default")
        expect(row.locator(".ap-default")).to_contain_text(f"Default: {default}")
        expect(revert).to_be_disabled()

        original = slider.input_value()
        other = str(int(default) + 1)
        slider.evaluate("(el, v) => { el.value = v; el.dispatchEvent(new Event('input', { bubbles: true })); }", other)
        expect(revert).to_be_enabled()
        expect(use_default).to_be_enabled()

        use_default.click()
        expect(slider).to_have_value(default)
        expect(output).to_contain_text(default)
        expect(use_default).to_be_disabled()
        if default == original:
            expect(save).to_be_disabled()
        else:
            expect(save).to_be_enabled()

        if default == original:
            slider.evaluate("(el, v) => { el.value = v; el.dispatchEvent(new Event('input', { bubbles: true })); }", other)
        revert.click()
        expect(slider).to_have_value(original)
        expect(save).to_be_disabled()
        expect(revert).to_be_disabled()

    def test_advanced_change_elsewhere_refreshes_open_page(self, dashboard_page: Page):
        """A change made on the device (here: via the API) shows without a reload."""
        primary(dashboard_page, "Control").click()
        group = dashboard_page.locator("details.ap-group", has_text="Defrost").first
        group.locator(":scope > summary").click()
        slider = group.locator('form[data-key="AP29"] input[type="range"]')
        original = slider.input_value()
        changed = str(int(original) - 1 if int(original) > 0 else 1)
        url = dashboard_page.url.split("#")[0].rstrip("/") + "/api/heatpump/advanced/AP29"
        headers = {"Content-Type": "text/plain"}
        try:
            assert dashboard_page.request.put(url, data=changed, headers=headers).ok
            expect(slider).to_have_value(changed, timeout=15000)  # next 5 s status poll
            expect(group).to_have_attribute("open", "")
        finally:
            dashboard_page.request.put(url, data=original, headers=headers)

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
        open_events(dashboard_page)
        expect(dashboard_page.locator("#event-search")).to_be_visible()
        expect(dashboard_page.locator("#event-category")).to_be_visible()
        expect(dashboard_page.locator("#event-time")).to_be_visible()

    def test_event_search_keeps_focus_and_clears(self, dashboard_page: Page):
        open_events(dashboard_page)
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
        open_events(dashboard_page)
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
        # The old Errors page link still works: it lands on History › Faults.
        dashboard_page.evaluate("location.hash = '#/errors'")
        dashboard_page.reload()
        expect(history_seg(dashboard_page, "Faults")).to_have_attribute("aria-selected", "true")
        assert dashboard_page.url.endswith("#/faults")
        # An active fault puts a dot on the segment.
        expect(dashboard_page.locator(".hist-seg-btn .hist-seg-dot")).to_have_count(1)
        links = dashboard_page.locator("a[data-help-url]")
        expect(links).to_have_count(1)
        expect(links.first).to_have_text("Troubleshooting guide ↗")
        expect(links.first).to_have_attribute(
            "href", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832838")
        expect(links.first).to_have_attribute("target", "_blank")
        expect(links.first).to_have_attribute("rel", "noopener noreferrer")


def _fake_fault_history():
    import time
    now = int(time.time())
    started = now - 2 * 3600
    return {
        "demo_mode": True, "connected": True, "has_errors": False, "error_count": 0,
        "active": [],
        "history": [
            {"code": "P06", "name": "Water flow fault", "severity": "error", "active": False,
             "occurred": started, "cleared": started + 130,
             "help_url": "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832840"},
            {"code": "E19", "name": "Old fault", "severity": "error", "active": False,
             "occurred": now - 30 * 86400, "cleared": now - 30 * 86400 + 60,
             "help_url": "https://arcticheatpumps.freshdesk.com/support/solutions"},
        ],
    }, started


class TestFaultsPage:
    def test_history_rows_and_view_on_chart(self, dashboard_page: Page):
        import json
        data, started = _fake_fault_history()
        dashboard_page.route("**/api/heatpump/errors", lambda route: route.fulfill(
            status=200, content_type="application/json", body=json.dumps(data)))
        dashboard_page.evaluate("location.hash = '#/faults'")
        dashboard_page.reload()
        expect(history_seg(dashboard_page, "Faults")).to_have_attribute("aria-selected", "true")
        expect(dashboard_page.locator(".hist-seg-dot")).to_have_count(0)
        expect(dashboard_page.locator(".fault-ok")).to_contain_text("Last fault was P06")

        recent = dashboard_page.locator('.fault-row[data-fault="P06"]')
        expect(recent).to_contain_text("Water flow fault")
        expect(recent.locator(".f-dur")).to_have_text("2 min 10 s")
        expect(recent.locator("a[data-help-url]")).to_have_count(1)
        # Faults older than the chart's 14 days get no chart link.
        expect(dashboard_page.locator('.fault-row[data-fault="E19"] [data-action="fault-view"]')).to_have_count(0)

        view = recent.locator('[data-action="fault-view"]')
        end = int(view.get_attribute("data-end"))
        window = dashboard_page.evaluate("state.histWindow")
        assert end % 900 == 0
        # The 130 s fault sits inside the window, near its middle.
        assert end - window < started and started + 130 < end
        assert abs((end - window / 2) - (started + 65)) <= 450
        view.click()
        expect(history_seg(dashboard_page, "Cycles")).to_have_attribute("aria-selected", "true")
        assert dashboard_page.url.endswith("#/history")

    def test_narrow_layout_keeps_both_links(self, dashboard_page: Page):
        import json
        data, _ = _fake_fault_history()
        dashboard_page.route("**/api/heatpump/errors", lambda route: route.fulfill(
            status=200, content_type="application/json", body=json.dumps(data)))
        dashboard_page.set_viewport_size({"width": 420, "height": 900})
        dashboard_page.evaluate("location.hash = '#/faults'")
        dashboard_page.reload()
        recent = dashboard_page.locator('.fault-row[data-fault="P06"]')
        expect(recent.locator("a[data-help-url]")).to_be_visible()
        expect(recent.locator('[data-action="fault-view"]')).to_be_visible()
        expect(recent.locator(".f-lasted")).to_be_visible()
        expect(recent.locator(".f-dur")).to_be_hidden()


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
        ev("firmware_updated", "system", yesterday_noon - 120, prev, from_version="3.4.4", to_version="3.4.5"),
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
        open_events(page)
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
        assert "3.4.4 → 3.4.5" in details
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
