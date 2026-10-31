"""Web dashboard tests for the temperature history view.

The cycle chart is the shared tschart widget (components/tschart/web). Tests
reach it through its DOM hooks (data-tschart-*, .tschart-*) and the page's
`histChart.chart` instance for geometry.
"""

import re

import pytest
import requests
import urllib3
from playwright.sync_api import Page, expect
from playwright.sync_api import TimeoutError as PlaywrightTimeoutError

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

CHART = ".hist-chart"


def _seed_history(base_url: str) -> dict:
    """Replace telemetry history with the deterministic 8-hour fixture."""
    r = requests.post(
        f"{base_url}/api/test/populate-temperature-history",
        timeout=30,
        verify=False,
    )
    r.raise_for_status()
    return r.json()


def _open_history(page: Page) -> None:
    page.locator(".rail .nav-link", has_text="Status").click()
    page.get_by_role("button", name="View cycle history").click()
    page.wait_for_selector(f"{CHART} .tschart-line", timeout=10000)


def _settled(page: Page) -> None:
    """Wait until no window is loading and the chart isn't moving."""
    page.wait_for_function(
        "histChart && !histChart.chart.pending && !histChart.chart.spring"
        " && !histChart.chart.gesture && histChart.chart.dx === 0"
        " && !state.tempHistory.loading"
    )


class TestTemperatureHistoryWeb:
    def test_history_view_renders_chart(self, dashboard_page: Page, base_url: str):
        _seed_history(base_url)
        _open_history(dashboard_page)
        page = dashboard_page

        expect(page.get_by_role("heading", name="History", exact=True)).to_be_visible()
        for layer in ("outlet", "inlet", "setpoint"):
            expect(page.locator(f'{CHART} [data-layer="{layer}"] .tschart-line')).to_have_count(1)
        # Chips double as the legend; the key names the bands.
        expect(page.locator(f'{CHART} [data-tschart-chip="temps"]')).to_have_text("Temps")
        expect(page.locator(f'{CHART} [data-tschart-chip="power"]')).to_have_text("Power")
        for key, text in (("heating", "Heating"), ("hot_water", "Hot water"),
                          ("defrost", "Defrost"), ("fault", "Fault")):
            expect(page.locator(f'{CHART} [data-tschart-key="{key}"]')).to_have_text(text)
        # Latest values are tagged at the right edge.
        expect(page.locator(f'{CHART} [data-tschart-tag="outlet"]')).to_contain_text("out")

    def test_axis_labels_are_whole_hours(self, dashboard_page: Page, base_url: str):
        _seed_history(base_url)
        _open_history(dashboard_page)

        # SVG <text> nodes support textContent (not innerText).
        labels = dashboard_page.locator(f"{CHART} text.tschart-time").all_text_contents()
        assert labels, "expected time labels on the x-axis"
        # 24 h ticks read "14:00"; 12 h ticks read "2 PM".
        assert all(re.fullmatch(r"\d{1,2}:00|\d{1,2} [AP]M", t) for t in labels), labels

    def test_temperature_axis_opens_with_round_degrees(
        self, dashboard_page: Page, base_url: str
    ):
        _seed_history(base_url)
        _open_history(dashboard_page)
        page = dashboard_page
        pill = page.locator(f'{CHART} [data-tschart-pill="temps"]')
        axis = page.locator(f"{CHART} text.tschart-axis")
        expect(axis).to_have_count(0)
        try:
            pill.click()
            expect(axis.first).to_be_visible()
            temps = [t for t in axis.all_text_contents() if t.endswith("°")]
            assert len(temps) >= 2, temps
            # Whole degrees (not 4.6°, 14.4°...) at a round, even step.
            assert all(re.fullmatch(r"-?\d+°", t) for t in temps), temps
            values = [int(t[:-1]) for t in temps]
            steps = {b - a for a, b in zip(values, values[1:])}
            assert len(steps) == 1 and steps.pop() in {1, 2, 5, 10, 20, 50, 100}, values

            # Open/closed is remembered.
            page.reload()
            page.wait_for_selector(".rail", timeout=15000)
            _open_history(page)
            expect(axis.first).to_be_visible()
            pill.click()
            expect(axis).to_have_count(0)
        finally:
            page.evaluate("localStorage.removeItem('arcticHistChart')")

    def test_history_navigation_shifts_window(
        self, dashboard_page: Page, base_url: str
    ):
        _seed_history(base_url)
        _open_history(dashboard_page)

        # At the most recent window, Later/Latest are disabled.
        expect(dashboard_page.locator('[data-action="history-latest"]')).to_be_disabled()

        range_before = dashboard_page.locator(".hist-range").inner_text()
        dashboard_page.locator('[data-action="history-prev"]').click()
        dashboard_page.wait_for_function(
            "prev => document.querySelector('.hist-range')?.innerText !== prev",
            arg=range_before,
            timeout=10000,
        )

        # Having stepped back, returning to the latest window is now possible.
        expect(dashboard_page.locator('[data-action="history-latest"]')).to_be_enabled()

    def test_hour_step_and_double_click_recentre(
        self, dashboard_page: Page, base_url: str
    ):
        _seed_history(base_url)
        _open_history(dashboard_page)
        page = dashboard_page

        def next_request_end(action):
            with page.expect_request(
                lambda r: "/api/heatpump/temperature-history" in r.url, timeout=10000
            ) as req:
                action()
            m = re.search(r"end=(\d+)", req.value.url)
            return int(m.group(1)) if m else None

        latest_end = page.evaluate("state.tempHistory.data.latest_end")
        back = next_request_end(
            lambda: page.locator('[data-action="history-back-hour"]').click()
        )
        assert back == latest_end - 3600
        page.wait_for_function(f"state.tempHistory.data?.end === {back}")
        _settled(page)

        # Double-clicking a moment centres the 8 h window on it (15 min snap).
        x, y = _plot_point(page, 0.3)
        t = page.evaluate(
            "x => histChart.chart.timeAtX(x - histChart.chart.body.getBoundingClientRect().left)",
            x,
        )
        centred = next_request_end(lambda: page.mouse.dblclick(x, y))
        assert centred is not None
        assert abs(centred - (t + 4 * 3600)) <= 450
        assert centred % 900 == 0

    def test_zoom_presets_narrow_the_window(
        self, dashboard_page: Page, base_url: str
    ):
        _seed_history(base_url)
        _open_history(dashboard_page)
        page = dashboard_page
        zoom = page.locator(".hist-zoom button")
        expect(zoom).to_have_text(["8 h", "4 h", "2 h", "1 h"])
        expect(page.locator('.hist-zoom [data-window="28800"]')).to_have_attribute(
            "aria-pressed", "true"
        )

        try:
            with page.expect_request(
                lambda r: "/api/heatpump/temperature-history" in r.url, timeout=10000
            ) as req:
                page.locator('.hist-zoom [data-window="3600"]').click()
            assert "window=3600" in req.value.url
            # Zooming from the live view stays live.
            assert "end=" not in req.value.url
            page.wait_for_function(
                "state.tempHistory.data && state.tempHistory.data.window_seconds === 3600"
            )
            _settled(page)
            data = page.evaluate("state.tempHistory.data")
            assert data["end"] - data["start"] == 3600
            assert page.evaluate("histChart.chart.getWindow()") == {
                "start": data["start"], "end": data["end"]
            }
            expect(page.locator('.hist-zoom [data-window="3600"]')).to_have_attribute(
                "aria-pressed", "true"
            )
            # Paging moves a whole hour, fine steps 15 min.
            expect(page.locator('[data-action="history-prev"]')).to_have_text("‹ 1 h")
            expect(page.locator('[data-action="history-back-hour"]')).to_have_text("‹ 15 min")
            # Gridlines every 15 minutes, on the quarter hour.
            clock = page.locator(f"{CHART} text.tschart-time").all_text_contents()
            assert len(clock) >= 4, clock
            assert all(
                re.fullmatch(r"\d{1,2}:(00|15|30|45)( [AP]M)?|\d{1,2} [AP]M", t)
                for t in clock
            ), clock

            with page.expect_request(
                lambda r: "/api/heatpump/temperature-history" in r.url, timeout=10000
            ) as req:
                page.locator('[data-action="history-back-hour"]').click()
            m = re.search(r"end=(\d+)", req.value.url)
            assert m and int(m.group(1)) == data["latest_end"] - 900

            # The choice is remembered.
            page.reload()
            page.wait_for_selector(".rail", timeout=15000)
            _open_history(page)
            expect(page.locator('.hist-zoom [data-window="3600"]')).to_have_attribute(
                "aria-pressed", "true"
            )
        finally:
            page.evaluate("localStorage.removeItem('arcticHistWindow')")

    def test_zoom_presets_fit_on_mobile(self, dashboard_page: Page, base_url: str):
        _seed_history(base_url)
        _open_history(dashboard_page)
        dashboard_page.set_viewport_size({"width": 390, "height": 844})
        expect(dashboard_page.locator(".hist-zoom button").first).to_be_visible()
        boxes = [b.bounding_box() for b in dashboard_page.locator(".hist-zoom button").all()]
        # One row, one line each ("8 h", not "8" over "h").
        assert len({round(b["y"]) for b in boxes}) == 1, boxes
        assert all(b["height"] < 48 for b in boxes), boxes
        width = dashboard_page.evaluate("() => document.documentElement.scrollWidth")
        assert width <= 390, width

    def test_card_names_the_operating_state(self, dashboard_page: Page, base_url: str):
        """The hover card says what the unit was doing, like the home chart's:
        a defrost (outside every run) reads Defrosting, not Idle."""
        _seed_history(base_url)

        def patch(route):
            resp = route.fetch()
            body = resp.json()
            end = body["end"]
            for s in body["samples"]:
                age = end - s["t"]
                s["run"] = 1800 <= age < 3600 or 600 <= age < 1200
                s["mode"] = 1 if s["run"] else 0
                if 600 <= age < 1200:
                    s["mode"] = 3
            body["defrosts"] = [{"start": end - 1800, "end": end - 1200}]
            route.fulfill(response=resp, json=body)

        dashboard_page.route("**/api/heatpump/temperature-history*", patch)
        _open_history(dashboard_page)
        _settled(dashboard_page)
        card = dashboard_page.locator(f"{CHART} [data-tschart-card]")
        end = dashboard_page.evaluate("state.tempHistory.data.end")
        _, y = _plot_point(dashboard_page, 0.5)

        for age, text in ((2700, "Heating"), (1500, "Defrosting"), (900, "Hot water"),
                          (300, "Idle")):
            x = dashboard_page.evaluate(
                "t => histChart.chart.body.getBoundingClientRect().left + histChart.chart.xAt(t)",
                end - age,
            )
            dashboard_page.mouse.move(x, y)
            expect(card.locator(".tschart-row.block .l")).to_have_text(text)
            # The series values follow.
            expect(card.locator(".tschart-row.value", has_text="Outlet")).to_be_visible()

        # Leaving the chart puts the end tags back.
        dashboard_page.mouse.move(0, 0)
        expect(card).to_have_count(0)
        expect(dashboard_page.locator(f'{CHART} [data-tschart-tag="outlet"]')).to_be_visible()

    def test_latest_window_refreshes_live(self, dashboard_page: Page, base_url: str):
        _seed_history(base_url)
        # Take over timers so a minute can pass instantly. fast_forward fires the
        # 5 s poll once per jump rather than replaying every tick in a burst.
        dashboard_page.clock.install()
        dashboard_page.reload()
        dashboard_page.wait_for_selector(".rail", timeout=15000)
        _open_history(dashboard_page)

        def is_history(request):
            return "/api/heatpump/temperature-history" in request.url

        def assert_no_reload(advance_ms: int):
            with pytest.raises(PlaywrightTimeoutError):
                with dashboard_page.expect_request(is_history, timeout=4000):
                    dashboard_page.clock.fast_forward(advance_ms)

        assert_no_reload(30_000)

        with dashboard_page.expect_request(is_history, timeout=10000) as reload:
            dashboard_page.clock.fast_forward(40_000)
        assert "end=" not in reload.value.url
        expect(dashboard_page.locator(CHART)).to_be_visible()

        # Paged back to an older window: it stays put.
        dashboard_page.locator('[data-action="history-prev"]').click()
        expect(dashboard_page.locator('[data-action="history-latest"]')).to_be_enabled()
        assert_no_reload(130_000)

    def test_touch_drag_pans_and_hold_scrubs(self, dashboard_page: Page, base_url: str):
        """On touch, a quick sideways drag pans back in time; press-and-hold
        first, then drag, moves the readout and leaves the window alone."""
        _seed_history(base_url)
        dashboard_page.clock.install()
        dashboard_page.reload()
        dashboard_page.wait_for_selector(".rail", timeout=15000)
        _open_history(dashboard_page)
        page = dashboard_page

        def touch(kind: str, frac: float) -> None:
            page.evaluate(
                """([kind, frac]) => {
                  const c = histChart.chart, p = c.getPlotArea(), r = c.body.getBoundingClientRect();
                  c.body.dispatchEvent(new PointerEvent(kind, { bubbles: true, isPrimary: true,
                    pointerId: 7, pointerType: 'touch',
                    clientX: r.left + p.x1 + frac * (p.x2 - p.x1),
                    clientY: r.top + (p.y1 + p.y2) / 2 }));
                }""",
                [kind, frac],
            )

        # Hold past the long-press time, then drag: the readout follows.
        touch("pointerdown", 0.3)
        page.clock.fast_forward(600)
        touch("pointermove", 0.45)
        touch("pointermove", 0.7)
        expect(page.locator(f"{CHART} [data-tschart-card]")).to_be_visible()
        touch("pointerup", 0.7)
        assert page.evaluate("state.tempHistory.end") is None
        assert page.evaluate("histChart.chart.pending") is None

        # Quick drag right: pans back in time.
        touch("pointerdown", 0.3)
        touch("pointermove", 0.45)
        touch("pointermove", 0.7)
        touch("pointerup", 0.7)
        assert page.evaluate("state.tempHistory.end") is not None
        expect(page.locator('[data-action="history-latest"]')).to_be_enabled()

    def test_drag_past_now_rubber_bands_and_springs_back(
        self, dashboard_page: Page, base_url: str
    ):
        """At the newest window a drag towards the future follows the pointer
        at a third of the speed over the hatched "Now" zone, then springs back
        without loading anything."""
        _seed_history(base_url)
        _open_history(dashboard_page)
        _settled(dashboard_page)
        loads = []
        dashboard_page.on(
            "request",
            lambda r: loads.append(r.url) if "temperature-history" in r.url else None,
        )

        x, y = _plot_point(dashboard_page, 0.7)
        dashboard_page.mouse.move(x, y)
        dashboard_page.mouse.down()
        dashboard_page.mouse.move(x - 240, y, steps=6)
        shift = dashboard_page.evaluate("histChart.chart.dx")
        assert -120 < shift < -60, shift
        expect(dashboard_page.locator(f'{CHART} [data-tschart-edge="after"]')).to_have_text("Now")
        dashboard_page.mouse.up()

        _settled(dashboard_page)
        assert not loads, loads
        assert dashboard_page.evaluate("state.tempHistory.end") is None
        expect(dashboard_page.locator('[data-action="history-latest"]')).to_be_disabled()

    def test_drag_past_start_of_history_rubber_bands(
        self, dashboard_page: Page, base_url: str
    ):
        _seed_history(base_url)

        # Make the fixture's own start the oldest record, so the window it
        # opens on is already the oldest one.
        def patch(route):
            resp = route.fetch()
            body = resp.json()
            body["earliest"] = body["start"]
            route.fulfill(response=resp, json=body)

        dashboard_page.route("**/api/heatpump/temperature-history*", patch)
        _open_history(dashboard_page)
        _settled(dashboard_page)
        expect(dashboard_page.locator('[data-action="history-prev"]')).to_be_disabled()
        end = dashboard_page.evaluate("state.tempHistory.data.end")

        x, y = _plot_point(dashboard_page, 0.3)
        dashboard_page.mouse.move(x, y)
        dashboard_page.mouse.down()
        dashboard_page.mouse.move(x + 240, y, steps=6)
        shift = dashboard_page.evaluate("histChart.chart.dx")
        assert 60 < shift < 120, shift
        expect(
            dashboard_page.locator(f'{CHART} [data-tschart-edge="before"]')
        ).to_have_text("Start of history")
        dashboard_page.mouse.up()

        _settled(dashboard_page)
        assert dashboard_page.evaluate("state.tempHistory.data.end") == end
        expect(dashboard_page.locator('[data-action="history-prev"]')).to_be_disabled()

    def test_empty_window_keeps_chart_and_navigation(
        self, dashboard_page: Page, base_url: str
    ):
        """A window with no samples inside a longer history still shows the
        chart and its navigation, so the user can move back out of it."""
        _seed_history(base_url)

        def empty(route):
            resp = route.fetch()
            body = resp.json()
            body["samples"] = []
            route.fulfill(response=resp, json=body)

        dashboard_page.route("**/api/heatpump/temperature-history*", empty)
        dashboard_page.locator(".rail .nav-link", has_text="Status").click()
        dashboard_page.get_by_role("button", name="View cycle history").click()
        page = dashboard_page.locator(".page")
        expect(page.locator(CHART)).to_be_visible()
        expect(page.locator(f'{CHART} [data-tschart-status="empty"]')).to_have_text(
            "No samples in this window"
        )
        expect(page).not_to_contain_text("No cycle history recorded yet")
        expect(page.locator('[data-action="history-prev"]')).to_be_enabled()

        dashboard_page.unroute("**/api/heatpump/temperature-history*")
        end = dashboard_page.evaluate("state.tempHistory.data.end")
        page.locator('[data-action="history-prev"]').click()
        dashboard_page.wait_for_function(f"state.tempHistory.data.end < {end}")
        expect(page.locator('[data-action="history-latest"]')).to_be_visible()
        expect(page.locator(f"{CHART} [data-tschart-status]")).to_have_count(0)

    def test_empty_history_shows_the_no_history_message(
        self, dashboard_page: Page, base_url: str
    ):
        _seed_history(base_url)

        def empty(route):
            resp = route.fetch()
            body = resp.json()
            body["samples"] = []
            body["earliest"] = body["start"]
            body["latest_end"] = body["end"]
            route.fulfill(response=resp, json=body)

        dashboard_page.route("**/api/heatpump/temperature-history*", empty)
        dashboard_page.locator(".rail .nav-link", has_text="Status").click()
        dashboard_page.get_by_role("button", name="View cycle history").click()
        expect(dashboard_page.locator(".page")).to_contain_text("No cycle history recorded yet")
        expect(dashboard_page.locator(CHART)).to_have_count(0)

    def test_spinner_marks_the_revealed_strip(self, dashboard_page: Page, base_url: str):
        """A slow load after a drag shows a spinner centred on the strip of
        new data, and removes it once the chart redraws."""
        _seed_history(base_url)
        _open_history(dashboard_page)
        _zoom_1h(dashboard_page)
        _gate_history_loads(dashboard_page)

        x, y = _plot_point(dashboard_page, 0.3)
        plot_w = dashboard_page.evaluate(
            "(() => { const p = histChart.chart.getPlotArea(); return p.x2 - p.x1; })()"
        )
        drag = plot_w / 4
        dashboard_page.mouse.move(x, y)
        dashboard_page.mouse.down()
        dashboard_page.mouse.move(x + drag, y, steps=6)
        dashboard_page.mouse.up()

        # The release snaps to 5 min, so the strip is about the dragged width.
        strip = dashboard_page.evaluate(
            """() => { const c = histChart.chart, p = c.getPlotArea(), w = c.getWindow(),
                q = c.pending, r = c.body.getBoundingClientRect();
              const px = (w.start - q.start) * (p.x2 - p.x1) / (w.end - w.start);
              return { left: r.left + p.x1, width: px }; }"""
        )
        assert abs(strip["width"] - drag) <= plot_w / 24 + 1, (strip, drag)
        spin = dashboard_page.locator(f"{CHART} .tschart-spin")
        expect(spin).to_be_visible()
        box = spin.bounding_box()
        centre = box["x"] + box["width"] / 2
        assert abs(centre - (strip["left"] + strip["width"] / 2)) < 4, (centre, strip)

        dashboard_page.evaluate("window.__openHistGate()")
        expect(spin).to_have_count(0)
        expect(dashboard_page.locator('[data-action="history-latest"]')).to_be_enabled()

    def test_steps_during_a_load_are_not_lost(self, dashboard_page: Page, base_url: str):
        """Clicking Earlier twice while the first load is still in flight lands
        two windows back, not one."""
        _seed_history(base_url)
        _open_history(dashboard_page)
        _zoom_1h(dashboard_page)
        d = dashboard_page.evaluate("state.tempHistory.data")
        _gate_history_loads(dashboard_page)

        prev = dashboard_page.locator('[data-action="history-prev"]')
        prev.click()
        prev.click()
        dashboard_page.evaluate("window.__openHistGate()")
        target = d["end"] - 2 * d["window_seconds"]
        dashboard_page.wait_for_function(
            f"state.tempHistory.data.end === {target} && !state.tempHistory.loading"
        )
        _settled(dashboard_page)
        assert dashboard_page.evaluate("histChart.chart.getWindow().end") == target


def _plot_point(page: Page, frac: float) -> tuple[float, float]:
    """Client x/y at `frac` across the plot, half way down."""
    return tuple(
        page.evaluate(
            """frac => { const c = histChart.chart, p = c.getPlotArea(),
                r = c.body.getBoundingClientRect();
              return [r.left + p.x1 + frac * (p.x2 - p.x1), r.top + (p.y1 + p.y2) / 2]; }""",
            frac,
        )
    )


def _zoom_1h(page: Page) -> None:
    """Zoom to 1 h (not remembered) so the 8 h fixture has room to page back."""
    page.locator('.hist-zoom [data-window="3600"]').click()
    page.wait_for_function("state.tempHistory.data?.window_seconds === 3600")
    _settled(page)
    page.evaluate("localStorage.removeItem('arcticHistWindow')")


def _gate_history_loads(page: Page) -> None:
    """Hold temperature-history fetches until window.__openHistGate()."""
    page.evaluate(
        """() => {
          let open; const gate = new Promise(r => { open = r; });
          window.__openHistGate = open;
          const real = window.fetch;
          window.fetch = (url, ...rest) => String(url).includes('temperature-history')
            ? gate.then(() => real(url, ...rest)) : real(url, ...rest);
        }"""
    )


def _post(base_url: str, path: str, body: dict | None = None) -> None:
    r = requests.post(f"{base_url}{path}", json=body, timeout=30, verify=False)
    r.raise_for_status()


class TestHistoryFaultMarkers:
    def test_fault_is_marked_on_the_chart(
        self, dashboard_page: Page, base_url: str
    ):
        _seed_history(base_url)
        # Fault spans come from the event log; earlier suites leave a crowd of
        # injected faults in this window whose labels would crowd out P02's.
        from conftest import API_KEY
        requests.delete(
            f"{base_url}/api/events",
            headers={"X-API-Key": API_KEY} if API_KEY else {},
            timeout=30,
            verify=False,
        ).raise_for_status()
        _post(base_url, "/api/test/inject-fault", {"code": "P02", "active": True})
        try:
            _post(base_url, "/api/test/inject-fault", {"code": "P02", "active": False})
        finally:
            _post(base_url, "/api/test/clear-faults")

        _open_history(dashboard_page)
        chart = dashboard_page.locator(CHART)

        # A hatched span from the fault onset, flagged by a coded note.
        assert chart.locator('.tschart-spans[data-set="2"] .tschart-span.hatch').count() >= 1
        labels = chart.locator(".tschart-note-label").all_text_contents()
        assert any("P02" in t for t in labels), labels
        # The code sits in the strip above the plot, clear of the lines.
        label_bottom = chart.locator(".tschart-note-label").first.evaluate(
            "e => { const b = e.getBBox(); return b.y + b.height; }"
        )
        plot_top = dashboard_page.evaluate("histChart.chart.getPlotArea().y1")
        assert label_bottom <= plot_top, (label_bottom, plot_top)
        # No note line runs through a label in the strip.
        crossings = chart.locator("svg.tschart-svg").evaluate(
            """svg => {
              const labels = [...svg.querySelectorAll('.tschart-note-label')].map(e => e.getBBox());
              const out = [];
              for (const l of svg.querySelectorAll('.tschart-note line')) {
                const x = l.x1.baseVal.value, top = l.y1.baseVal.value;
                for (const b of labels)
                  if (x >= b.x && x <= b.x + b.width && top < b.y + b.height) out.push([x, b.x, b.width]);
              }
              return out;
            }"""
        )
        assert crossings == [], crossings
        expect(chart.locator('[data-tschart-key="fault"]')).to_be_visible()
