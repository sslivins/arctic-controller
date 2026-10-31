"""Web dashboard tests for the Energy history page and the Cycles power panel.

The chart tests serve /api/energy/* from a deterministic fixture so they do not
depend on how much energy the bench controller has recorded; one smoke test
and the power-panel tests run against the real device.
"""

import datetime as dt
import json
import re
from urllib.parse import parse_qs, urlparse

import pytest
import requests
import urllib3
from playwright.sync_api import Page, expect

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

TODAY = dt.date.today()


def _fake_day(day: dt.date) -> dict:
    n = day.toordinal()
    heating = 8000 + (n % 7) * 1000
    hot_water = 1500 + (n % 3) * 200
    standby = 300
    return {
        "date": day.isoformat(),
        "final": day != TODAY,
        "utc_offset_min": -420,
        "electrical_wh": {
            "heating": heating, "cooling": 0, "hot_water": hot_water,
            "standby": standby, "unattributed": 0,
            "total": heating + hot_water + standby,
        },
        "thermal_wh": {"heating": heating * 3, "cooling": 0, "hot_water": hot_water * 2},
        "covered_electrical_wh": {"heating": heating, "cooling": 0, "hot_water": hot_water},
        "run_seconds": {"heating": 18000, "cooling": 0, "hot_water": 2400},
        "starts": 9,
        "defrost": {"count": 2, "seconds": 480, "thermal_wh": -300},
        "coverage_seconds": {"power": 86000, "thermal": 50000},
        "source_seconds": {"unit": 0, "external": 50000, "fallback": 0, "settling": 600},
        "outdoor_c": {"min": -4.0, "avg": 1.0, "max": 5.0,
                      # Even days mimic records from before the running average.
                      "run_avg": -2.0 if n % 2 else None},
        "settings": {"flow_lpm": 40, "glycol_pct": 30, "mixed": False},
    }


def _fulfil_daily(route):
    query = parse_qs(urlparse(route.request.url).query)
    to = dt.date.fromisoformat(query["to"][0]) if "to" in query else TODAY
    frm = dt.date.fromisoformat(query["from"][0])
    days, day = [], frm
    while day <= to:
        days.append(_fake_day(day))
        day += dt.timedelta(days=1)
    route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({"from": frm.isoformat(), "to": to.isoformat(), "days": days}),
    )


def _fulfil_totals(route):
    route.fulfill(
        status=200,
        content_type="application/json",
        body=json.dumps({
            "reset_id": 1, "since": (TODAY - dt.timedelta(days=200)).isoformat(),
            "through": TODAY.isoformat(), "includes_today": True,
            "electrical_wh": 1, "thermal_wh": {}, "covered_electrical_wh": {},
        }),
    )


def _query(url: str) -> dict:
    return {k: v[0] for k, v in parse_qs(urlparse(url).query).items()}


@pytest.fixture
def page_errors(dashboard_page: Page):
    errors = []
    dashboard_page.on("pageerror", lambda exc: errors.append(str(exc)))
    return errors


@pytest.fixture
def mocked_energy(dashboard_page: Page):
    dashboard_page.route("**/api/energy/daily*", _fulfil_daily)
    dashboard_page.route("**/api/energy/totals", _fulfil_totals)
    return dashboard_page


def _open_cycles(page: Page) -> None:
    page.locator(".rail .nav-link", has_text="Status").click()
    page.get_by_role("button", name="View cycle history").click()
    page.wait_for_selector(".hist-chart", timeout=10000)


def _open_energy(page: Page, neighbours: bool = True) -> None:
    """Open the Energy tab. With the mocked ledger the live view also fetches
    the days before the window (for dragging); wait for those too, so later
    request expectations don't catch them."""
    _open_cycles(page)
    with page.expect_request(lambda r: "/api/energy/daily" in r.url, timeout=10000):
        if not neighbours:
            page.get_by_role("tab", name="Energy").click()
        else:
            with page.expect_response(lambda r: "/api/energy/daily" in r.url and "to=" in r.url, timeout=10000):
                page.get_by_role("tab", name="Energy").click()


def _drag(page: Page, x: float, y: float, dx: float, release: bool = True) -> None:
    page.mouse.move(x, y)
    page.mouse.down()
    page.mouse.move(x + dx, y, steps=12)
    if release:
        page.mouse.up()


class TestEnergyPage:
    def test_energy_tab_draws_daily_bars(self, mocked_energy: Page, page_errors):
        page = mocked_energy
        _open_energy(page)

        expect(page.locator(".en-chart")).to_be_visible()
        expect(page.get_by_role("tab", name="Energy")).to_have_attribute("aria-selected", "true")
        # 30 days by default: one hit target per day, stacked bars, COP dots
        # (plus the 30 days before, drawn off the plot for dragging).
        expect(page.locator(".en-chart .tsbars-hit")).to_have_count(30)
        assert page.locator(".en-chart .tsbars-bar").count() >= 60
        assert page.locator(".en-chart .tsbars-point").count() == 60
        # Today is still accruing: striped, with its date in the accent.
        assert page.locator(".en-chart .tsbars-bar.provisional").count() >= 1
        expect(page.locator(".en-chart .tsbars-label.accent")).to_have_count(1)
        expect(page.locator(".en-stat", has_text="Electricity")).to_contain_text("kWh")
        expect(page.locator(".en-stat", has_text="COP (est.)")).to_contain_text("2.")
        assert page_errors == []

    def test_live_range_lets_the_controller_pick_today(self, mocked_energy: Page):
        page = mocked_energy
        _open_cycles(page)
        with page.expect_request(lambda r: "/api/energy/daily" in r.url) as req:
            page.get_by_role("tab", name="Energy").click()
        query = _query(req.value.url)
        assert "to" not in query
        assert query["from"] == (TODAY - dt.timedelta(days=29)).isoformat()

    def test_range_buttons_widen_the_window(self, mocked_energy: Page):
        page = mocked_energy
        _open_energy(page)

        with page.expect_request(lambda r: "/api/energy/daily" in r.url) as req:
            page.locator('[data-action="energy-range"][data-range="90"]').click()
        assert _query(req.value.url)["from"] == (TODAY - dt.timedelta(days=89)).isoformat()
        expect(page.locator(".en-chart .tsbars-hit")).to_have_count(90)
        expect(page.locator('[data-action="energy-range"][data-range="90"]')).to_have_attribute(
            "aria-pressed", "true"
        )

    def test_paging_back_and_returning_to_latest(self, mocked_energy: Page):
        page = mocked_energy
        _open_energy(page)
        expect(page.locator('[data-action="energy-next"]')).to_be_disabled()

        with page.expect_request(lambda r: "/api/energy/daily" in r.url) as req:
            page.locator('[data-action="energy-prev"]').click()
        query = _query(req.value.url)
        assert query["to"] == (TODAY - dt.timedelta(days=30)).isoformat()
        assert query["from"] == (TODAY - dt.timedelta(days=59)).isoformat()

        latest = page.locator('[data-action="energy-latest"]')
        expect(latest).to_be_visible()
        with page.expect_request(lambda r: "/api/energy/daily" in r.url) as req:
            latest.click()
        assert "to" not in _query(req.value.url)
        expect(page.locator('[data-action="energy-next"]')).to_be_disabled()

    def test_selecting_a_day_shows_its_breakdown(self, mocked_energy: Page, page_errors):
        page = mocked_energy
        _open_energy(page)

        day = (TODAY - dt.timedelta(days=3)).isoformat()
        page.locator(f'.en-chart .tsbars-hit[data-key="{day}"]').click()
        detail = page.locator(f'.en-detail[data-energy-day="{day}"]')
        expect(detail).to_be_visible()
        expect(detail.locator(".en-table tbody tr", has_text="Heating")).to_be_visible()
        expect(detail.locator(".en-table tbody tr", has_text="Hot water")).to_be_visible()
        expect(detail.locator(".en-facts")).to_contain_text("9 starts")
        expect(detail.locator(".en-facts")).to_contain_text("2 defrosts")
        # The compressor runs for the day come from the cycle history.
        detail.locator(".en-runs, .en-runs-msg").first.wait_for(timeout=15000)
        expect(page.locator(".en-chart .tsbars-sel")).to_be_visible()

        page.locator('[data-action="energy-close"]').click()
        expect(page.locator(".en-detail")).to_have_count(0)
        assert page_errors == []

    def test_outdoor_while_running_with_day_average_fallback(self, mocked_energy: Page, page_errors):
        page = mocked_energy
        _open_energy(page)
        expect(page.locator(".en-stat", has_text="Avg outdoor while running")).to_contain_text("Day avg")

        days = [TODAY - dt.timedelta(days=i) for i in (3, 4)]
        ran, old = sorted(days, key=lambda d: d.toordinal() % 2, reverse=True)
        page.locator(f'.en-chart .tsbars-hit[data-key="{ran.isoformat()}"]').click()
        stat = page.locator(f'.en-detail[data-energy-day="{ran.isoformat()}"] .en-stat', has_text="Outdoor")
        expect(stat).to_contain_text("Outdoor while running")
        expect(stat).to_contain_text("Day avg")

        page.locator(f'.en-chart .tsbars-hit[data-key="{old.isoformat()}"]').click()
        stat = page.locator(f'.en-detail[data-energy-day="{old.isoformat()}"] .en-stat', has_text="Outdoor")
        expect(stat).not_to_contain_text("while running")
        expect(stat).not_to_contain_text("Day avg")
        assert page_errors == []

    def test_drag_pans_by_whole_days(self, mocked_energy: Page, page_errors):
        page = mocked_energy
        _open_energy(page)
        rng = page.locator(".en-range-text")
        latest_text = rng.inner_text()
        box = page.locator(f'.en-chart .tsbars-hit[data-key="{(TODAY - dt.timedelta(days=15)).isoformat()}"]').bounding_box()
        x, y, day_px = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2, box["width"]

        # Mid-drag the bars (and the days before the window) slide; the axes don't.
        ticks = page.locator(".en-chart .tsbars-axis text").all_inner_texts()
        _drag(page, x, y, day_px * 3.4, release=False)
        expect(page.locator(".en-chart")).to_have_class(re.compile("panning"))
        assert "translate" in page.locator(".en-chart .tsbars-pan").first.get_attribute("transform")
        assert page.locator(".en-chart .tsbars-axis text").all_inner_texts() == ticks
        # Release snaps to whole days (3.4 -> 3) without opening a day.
        with page.expect_request(lambda r: "/api/energy/daily" in r.url and "to=" in r.url) as req:
            page.mouse.up()
        query = _query(req.value.url)
        assert query["to"] == (TODAY - dt.timedelta(days=3)).isoformat()
        assert query["from"] == (TODAY - dt.timedelta(days=32)).isoformat()
        expect(page.locator('[data-action="energy-latest"]')).to_be_visible()
        expect(page.locator('[data-action="energy-next"]')).to_be_enabled()
        expect(rng).not_to_have_text(latest_text)
        expect(page.locator(".en-detail")).to_have_count(0)

        # Dragging well past today stops at the live view.
        with page.expect_request(lambda r: "/api/energy/daily" in r.url and "to=" not in r.url):
            _drag(page, x, y, -day_px * 10)
        expect(page.locator('[data-action="energy-next"]')).to_be_disabled()
        expect(rng).to_have_text(latest_text)
        expect(page.locator(".en-detail")).to_have_count(0)
        assert page_errors == []

    def test_drag_past_today_springs_back(self, mocked_energy: Page, page_errors):
        page = mocked_energy
        _open_energy(page)
        hit = page.locator(f'.en-chart .tsbars-hit[data-key="{(TODAY - dt.timedelta(days=15)).isoformat()}"]')
        expect(hit).to_have_count(1)  # neighbour bars render after their fetch
        box = hit.bounding_box()
        _drag(page, box["x"], box["y"] + box["height"] / 2, -box["width"] * 6)
        chart = page.locator(".en-chart")
        expect(chart).not_to_have_class(re.compile("panning"))
        expect(chart.locator(".tsbars-pan[transform]")).to_have_count(0)
        expect(page.locator('[data-action="energy-next"]')).to_be_disabled()
        expect(page.locator(".en-detail")).to_have_count(0)
        assert page_errors == []

    def test_year_view_fetches_half_a_year_either_side(self, mocked_energy: Page):
        page = mocked_energy
        page.route("**/api/energy/totals", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body=json.dumps({"reset_id": 1, "since": (TODAY - dt.timedelta(days=1000)).isoformat(),
                             "through": TODAY.isoformat(), "includes_today": True,
                             "electrical_wh": 1, "thermal_wh": {}, "covered_electrical_wh": {}})))
        _open_energy(page)
        with page.expect_request(lambda r: "/api/energy/daily" in r.url and "to=" in r.url) as req:
            page.locator('[data-action="energy-range"][data-range="365"]').click()
        query = _query(req.value.url)
        assert query["to"] == (TODAY - dt.timedelta(days=365)).isoformat()
        assert query["from"] == (TODAY - dt.timedelta(days=365 + 181)).isoformat()

    def test_energy_page_loads_against_the_device(self, dashboard_page: Page, page_errors):
        _open_energy(dashboard_page, neighbours=False)
        expect(
            dashboard_page.locator(".en-chart").or_(
                dashboard_page.get_by_text("No energy recorded yet.")
            )
        ).to_be_visible(timeout=10000)
        assert page_errors == []


def _seed_history(base_url: str) -> None:
    r = requests.post(
        f"{base_url}/api/test/populate-temperature-history", timeout=30, verify=False
    )
    r.raise_for_status()


class TestCyclesPowerPanel:
    def test_power_panel_draws_and_toggle_persists(
        self, dashboard_page: Page, base_url: str, page_errors
    ):
        page = dashboard_page
        page.evaluate(
            "localStorage.removeItem('arcticHistPower'); localStorage.removeItem('arcticHistChart')"
        )
        _seed_history(base_url)
        _open_cycles(page)

        chip = page.locator('.hist-chart [data-tschart-chip="power"]')
        area = page.locator('.hist-chart [data-layer="power"] .tschart-fill')
        expect(chip).to_have_attribute("aria-pressed", "true")
        expect(area).to_have_count(1)
        expect(page.get_by_text("kWh of electricity used in this window")).to_be_visible()

        try:
            chip.click()
            expect(chip).to_have_attribute("aria-pressed", "false")
            expect(area).to_have_count(0)
            expect(page.get_by_text("kWh of electricity used in this window")).to_have_count(0)
            assert page.evaluate("localStorage.getItem('arcticHistPower')") == "0"

            page.reload()
            page.wait_for_selector(".hist-chart", timeout=15000)
            expect(chip).to_have_attribute("aria-pressed", "false")
            expect(area).to_have_count(0)
        finally:
            page.evaluate(
                "localStorage.removeItem('arcticHistPower'); localStorage.removeItem('arcticHistChart')"
            )
        assert page_errors == []

class TestRunSummaries:
    def test_cycles_runs_show_cop_and_outdoor(
        self, dashboard_page: Page, base_url: str, page_errors
    ):
        page = dashboard_page
        _seed_history(base_url)
        _open_cycles(page)
        # The fixture's last run (hot water) has a stored summary: COP 2.3 at 5 °C.
        perf = page.locator(".en-run-perf", has_text="COP (est.) 2.3")
        expect(perf.first).to_be_visible(timeout=15000)
        expect(perf.first).to_contain_text(re.compile(r"Outdoor (5°C|41°F)"))
        assert page_errors == []
