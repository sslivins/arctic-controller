"""Web dashboard tests for the temperature history view."""

import re

import pytest
import requests
import urllib3
from playwright.sync_api import Page, expect
from playwright.sync_api import TimeoutError as PlaywrightTimeoutError

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


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
    page.wait_for_selector(".hist-chart", timeout=10000)


class TestTemperatureHistoryWeb:
    def test_history_view_renders_chart(self, dashboard_page: Page, base_url: str):
        _seed_history(base_url)
        _open_history(dashboard_page)

        expect(
            dashboard_page.get_by_role("heading", name="Cycle history")
        ).to_be_visible()

        # Inlet, outlet, and setpoint each draw a path.
        assert dashboard_page.locator(".hist-chart path").count() >= 3
        expect(
            dashboard_page.locator(".hist-legend", has_text="Inlet")
        ).to_be_visible()
        expect(
            dashboard_page.locator(".hist-legend", has_text="Setpoint")
        ).to_be_visible()

    def test_axis_labels_are_whole_hours(self, dashboard_page: Page, base_url: str):
        _seed_history(base_url)
        _open_history(dashboard_page)

        # SVG <text> nodes support textContent (not innerText), so use
        # all_text_contents() which reads textContent.
        labels = dashboard_page.locator(".hist-chart text.hist-axis").all_text_contents()
        # 24 h ticks read "14:00"; 12 h ticks read "2 PM".
        clock_labels = [t for t in labels if t and (":" in t or t.endswith("M"))]
        assert clock_labels, "expected time labels on the x-axis"
        # Every x-axis time label is rounded to a whole hour.
        assert all(re.fullmatch(r"\d{1,2}:00|\d{1,2} [AP]M", t) for t in clock_labels)

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

    def test_latest_window_refreshes_live(self, dashboard_page: Page, base_url: str):
        _seed_history(base_url)
        # Take over timers so a minute can pass instantly.
        dashboard_page.clock.install()
        dashboard_page.reload()
        dashboard_page.wait_for_selector(".rail", timeout=15000)
        _open_history(dashboard_page)

        def is_history(request):
            return "/api/heatpump/temperature-history" in request.url

        def assert_no_reload(advance_ms: int):
            with pytest.raises(PlaywrightTimeoutError):
                with dashboard_page.expect_request(is_history, timeout=4000):
                    dashboard_page.clock.run_for(advance_ms)

        assert_no_reload(30_000)

        with dashboard_page.expect_request(is_history, timeout=10000) as reload:
            dashboard_page.clock.run_for(40_000)
        assert "end=" not in reload.value.url
        expect(dashboard_page.locator(".hist-chart")).to_be_visible()

        # Paged back to an older window: it stays put.
        dashboard_page.locator('[data-action="history-prev"]').click()
        expect(dashboard_page.locator('[data-action="history-latest"]')).to_be_enabled()
        assert_no_reload(130_000)


def _post(base_url: str, path: str, body: dict | None = None) -> None:
    r = requests.post(f"{base_url}{path}", json=body, timeout=30, verify=False)
    r.raise_for_status()


class TestHistoryFaultMarkers:
    def test_fault_is_marked_on_the_chart(
        self, dashboard_page: Page, base_url: str
    ):
        _seed_history(base_url)
        _post(base_url, "/api/test/inject-fault", {"code": "P02", "active": True})
        try:
            _post(base_url, "/api/test/inject-fault", {"code": "P02", "active": False})
        finally:
            _post(base_url, "/api/test/clear-faults")

        _open_history(dashboard_page)

        # A full-height red line at the fault onset, labelled with the code.
        assert dashboard_page.locator(".hist-chart line.hist-fault").count() >= 1
        labels = dashboard_page.locator(".hist-chart .hist-fault-label").all_text_contents()
        assert any("P02" in t for t in labels)
        expect(dashboard_page.locator(".hist-legend", has_text="Fault")).to_be_visible()
