"""Web dashboard tests for the temperature history view."""

import requests
import urllib3
from playwright.sync_api import Page, expect

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
    page.locator(".rail .nav-link", has_text="History").click()
    page.wait_for_selector(".hist-chart", timeout=10000)


class TestTemperatureHistoryWeb:
    def test_history_view_renders_chart(self, dashboard_page: Page, base_url: str):
        _seed_history(base_url)
        _open_history(dashboard_page)

        expect(
            dashboard_page.get_by_role("heading", name="Temperature history")
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

        labels = dashboard_page.locator(".hist-chart text.hist-axis").all_inner_texts()
        clock_labels = [t for t in labels if ":" in t]
        assert clock_labels, "expected time labels on the x-axis"
        # Every x-axis time label is rounded to a whole hour (:00).
        assert all(t.endswith(":00") for t in clock_labels)

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
