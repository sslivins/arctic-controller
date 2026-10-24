"""Web dashboard tests for the Energy history page and the Cycles power panel.

The chart tests serve /api/energy/* from a deterministic fixture so they do not
depend on how much energy the bench controller has recorded; one smoke test
and the power-panel tests run against the real device.
"""

import datetime as dt
import json
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


def _open_energy(page: Page) -> None:
    _open_cycles(page)
    with page.expect_request(lambda r: "/api/energy/daily" in r.url, timeout=10000):
        page.get_by_role("tab", name="Energy").click()


class TestEnergyPage:
    def test_energy_tab_draws_daily_bars(self, mocked_energy: Page, page_errors):
        page = mocked_energy
        _open_energy(page)

        expect(page.locator(".en-chart")).to_be_visible()
        expect(page.get_by_role("tab", name="Energy")).to_have_attribute("aria-selected", "true")
        # 30 days by default: one hit target per day, stacked bars, COP dots.
        expect(page.locator(".en-chart .en-hit")).to_have_count(30)
        assert page.locator(".en-chart .en-bar").count() >= 30
        assert page.locator(".en-chart .en-cop").count() == 30
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
        expect(page.locator(".en-chart .en-hit")).to_have_count(90)
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
        page.locator(f'.en-hit[data-date="{day}"]').click()
        detail = page.locator(f'.en-detail[data-energy-day="{day}"]')
        expect(detail).to_be_visible()
        expect(detail.locator(".en-table tbody tr", has_text="Heating")).to_be_visible()
        expect(detail.locator(".en-table tbody tr", has_text="Hot water")).to_be_visible()
        expect(detail.locator(".en-facts")).to_contain_text("9 starts")
        expect(detail.locator(".en-facts")).to_contain_text("2 defrosts")
        # The compressor runs for the day come from the cycle history.
        detail.locator(".en-runs, .en-runs-msg").first.wait_for(timeout=15000)
        expect(page.locator(f'.en-chart .en-selected')).to_have_count(1)

        page.locator('[data-action="energy-close"]').click()
        expect(page.locator(".en-detail")).to_have_count(0)
        assert page_errors == []

    def test_outdoor_while_running_with_day_average_fallback(self, mocked_energy: Page, page_errors):
        page = mocked_energy
        _open_energy(page)
        expect(page.locator(".en-stat", has_text="Avg outdoor while running")).to_contain_text("Day avg")

        days = [TODAY - dt.timedelta(days=i) for i in (3, 4)]
        ran, old = sorted(days, key=lambda d: d.toordinal() % 2, reverse=True)
        page.locator(f'.en-hit[data-date="{ran.isoformat()}"]').click()
        stat = page.locator(f'.en-detail[data-energy-day="{ran.isoformat()}"] .en-stat', has_text="Outdoor")
        expect(stat).to_contain_text("Outdoor while running")
        expect(stat).to_contain_text("Day avg")

        page.locator(f'.en-hit[data-date="{old.isoformat()}"]').click()
        stat = page.locator(f'.en-detail[data-energy-day="{old.isoformat()}"] .en-stat', has_text="Outdoor")
        expect(stat).not_to_contain_text("while running")
        expect(stat).not_to_contain_text("Day avg")
        assert page_errors == []

    def test_energy_page_loads_against_the_device(self, dashboard_page: Page, page_errors):
        _open_energy(dashboard_page)
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
        page.evaluate("localStorage.removeItem('arcticHistPower')")
        _seed_history(base_url)
        _open_cycles(page)

        toggle = page.locator('[data-action="history-power"]')
        expect(toggle).to_have_attribute("aria-pressed", "true")
        assert page.locator(".hist-chart .hist-power-area").count() >= 1
        expect(page.get_by_text("kWh of electricity used in this window")).to_be_visible()

        try:
            toggle.click()
            expect(toggle).to_have_attribute("aria-pressed", "false")
            expect(page.locator(".hist-chart .hist-power-area")).to_have_count(0)
            assert page.evaluate("localStorage.getItem('arcticHistPower')") == "0"

            page.reload()
            page.wait_for_selector(".hist-chart", timeout=15000)
            expect(page.locator('[data-action="history-power"]')).to_have_attribute(
                "aria-pressed", "false"
            )
            expect(page.locator(".hist-chart .hist-power-area")).to_have_count(0)
        finally:
            page.evaluate("localStorage.removeItem('arcticHistPower')")
        assert page_errors == []
