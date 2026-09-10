"""Web dashboard tests for the Location card and the status-bar weather.

Both features exist on the touchscreen but were unreachable from a browser
until the /api/location and /api/weather endpoints were added, so these tests
cover the new UI end to end against the real device.

Geocoding and weather are driven through the test-only mock endpoints so the
assertions do not depend on outbound internet access or on the actual weather.
"""

import time

import pytest
import requests
import urllib3
from playwright.sync_api import Page, expect

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

KAMLOOPS = {
    "name": "Kamloops",
    "admin1": "British Columbia",
    "country": "Canada",
    "country_code": "CA",
    "timezone": "America/Vancouver",
    "latitude": 50.6745,
    "longitude": -120.3273,
}


def _post(base_url: str, path: str, body):
    r = requests.post(f"{base_url}{path}", json=body, timeout=10, verify=False)
    r.raise_for_status()
    return r.json()


def _set_geocoding(base_url: str, results):
    return _post(base_url, "/api/test/geocoding-mock", {"results": results})


def _reset_geocoding(base_url: str):
    requests.post(f"{base_url}/api/test/geocoding-mock-reset", json={}, timeout=10, verify=False)


def _set_weather(base_url: str, temp_c: float, code: int):
    return _post(base_url, "/api/test/weather-mock",
                 {"current": {"temperature_2m": temp_c, "weather_code": code}})


def _reset_weather(base_url: str):
    requests.post(f"{base_url}/api/test/weather-mock-reset", json={}, timeout=10, verify=False)


def _apply_weather(page: Page, base_url: str, temp_c: float, code: int, timeout: float = 40.0):
    """Set the weather mock and wait until the device actually reports it.

    Posting the mock only *queues* a refresh.  A real fetch that was already
    in flight -- for instance one started by an earlier test that moved the
    device -- completes afterwards and overwrites the cache with the live
    reading, so a page reloaded immediately can show the wrong weather.  Poll
    until the device serves the mocked values, re-posting if a stray fetch
    lands, and only then let the caller assert against the UI.
    """
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        _set_weather(base_url, temp_c, code)
        for _ in range(10):
            time.sleep(1.0)
            last = page.evaluate("() => fetch('/api/weather').then(r => r.json())")
            if (last.get("valid")
                    and last.get("weather_code") == code
                    and abs(float(last.get("temp_c", 0)) - temp_c) < 0.05):
                return
            if time.time() >= deadline:
                break
    raise AssertionError(
        f"device never reported the mocked weather {temp_c}C/{code}; last was {last}")


def _read_location(page: Page):
    return page.evaluate("() => fetch('/api/location').then(r => r.json())")


def _write_location(page: Page, body):
    return page.evaluate(
        """(body) => fetch('/api/location', {
             method: 'POST',
             headers: { 'Content-Type': 'application/json' },
             body: JSON.stringify(body)
           }).then(r => r.json())""",
        body,
    )


def _restore_location(page: Page, original):
    """Put the device's own location back.

    A device that had never been given a location must not come out of these
    tests pinned to 0,0 in the Gulf of Guinea, so only the timezone mode is
    restored in that case.
    """
    if original.get("valid"):
        _write_location(page, {
            "latitude": original["latitude"],
            "longitude": original["longitude"],
            "name": original.get("name", ""),
            "iana_tz": original.get("iana_tz", ""),
            "tz_auto": original.get("tz_auto", True),
        })
    else:
        _write_location(page, {"tz_auto": original.get("tz_auto", True)})


def open_time_settings(page: Page):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text="Time").click()
    page.locator(".settings-nav .nav-link.active", has_text="Time").wait_for()
    page.locator(".settings-layout .spinner").wait_for(state="detached", timeout=30000)


class TestLocationCard:
    """The Location card in Settings > Time."""

    def test_card_is_present_with_search_and_auto_timezone(self, dashboard_page: Page):
        open_time_settings(dashboard_page)
        expect(dashboard_page.locator("section.card.loc-card")).to_be_visible()
        expect(dashboard_page.locator('form[data-form="location-search"]')).to_be_visible()
        expect(dashboard_page.locator('input[data-action="toggle-tz-auto"]')).to_be_visible()

    def test_searching_lists_the_matching_places(self, dashboard_page: Page, base_url: str):
        original = _read_location(dashboard_page)
        try:
            _set_geocoding(base_url, [KAMLOOPS])
            open_time_settings(dashboard_page)
            dashboard_page.locator("#loc-query").fill("kamloops")
            dashboard_page.locator('form[data-form="location-search"] button[type="submit"]').click()

            result = dashboard_page.locator(".loc-result")
            expect(result).to_have_count(1)
            expect(result).to_contain_text("Kamloops, British Columbia, CA")
            expect(result).to_contain_text("America/Vancouver")
        finally:
            _reset_geocoding(base_url)
            _restore_location(dashboard_page, original)

    def test_picking_a_result_saves_the_location(self, dashboard_page: Page, base_url: str):
        original = _read_location(dashboard_page)
        try:
            _set_geocoding(base_url, [KAMLOOPS])
            open_time_settings(dashboard_page)
            dashboard_page.locator("#loc-query").fill("kamloops")
            dashboard_page.locator('form[data-form="location-search"] button[type="submit"]').click()
            dashboard_page.locator(".loc-result").first.click()

            # The card reflects the new location and the result list is cleared.
            expect(dashboard_page.locator("#loc-name")).to_have_text("Kamloops, British Columbia, CA")
            expect(dashboard_page.locator(".loc-result")).to_have_count(0)

            saved = _read_location(dashboard_page)
            assert saved["valid"] is True
            assert saved["name"] == "Kamloops, British Columbia, CA"
            assert abs(saved["latitude"] - KAMLOOPS["latitude"]) < 0.001
        finally:
            _reset_geocoding(base_url)
            _restore_location(dashboard_page, original)

    def test_a_failed_search_is_reported_rather_than_silently_empty(
            self, dashboard_page: Page, base_url: str):
        try:
            _post(base_url, "/api/test/geocoding-mock", {"__error__": True})
            open_time_settings(dashboard_page)
            dashboard_page.locator("#loc-query").fill("kamloops")
            dashboard_page.locator('form[data-form="location-search"] button[type="submit"]').click()

            expect(dashboard_page.locator(".notice.bad")).to_be_visible()
            expect(dashboard_page.locator(".loc-result")).to_have_count(0)
        finally:
            _reset_geocoding(base_url)

    def test_no_matches_says_so(self, dashboard_page: Page, base_url: str):
        try:
            _set_geocoding(base_url, [])
            open_time_settings(dashboard_page)
            dashboard_page.locator("#loc-query").fill("nowhere at all")
            dashboard_page.locator('form[data-form="location-search"] button[type="submit"]').click()

            expect(dashboard_page.locator(".notice", has_text="No matching places found")).to_be_visible()
        finally:
            _reset_geocoding(base_url)

    def test_automatic_timezone_disables_the_manual_selector(self, dashboard_page: Page):
        original = _read_location(dashboard_page)
        try:
            _write_location(dashboard_page, {"tz_auto": True})
            open_time_settings(dashboard_page)
            # The device ignores the manual roller in auto mode; the web form
            # must not offer an edit that would be silently overwritten.
            expect(dashboard_page.locator('select[name="timezone"]')).to_be_disabled()

            dashboard_page.locator('input[data-action="toggle-tz-auto"]').click()
            expect(dashboard_page.locator('select[name="timezone"]')).to_be_enabled()
            assert _read_location(dashboard_page)["tz_auto"] is False
        finally:
            _restore_location(dashboard_page, original)


class TestStatusBarWeather:
    """Temperature and weather icon in the top bar."""

    def test_weather_appears_with_icon_and_temperature(self, dashboard_page: Page, base_url: str):
        try:
            _apply_weather(dashboard_page, base_url, -5.0, 71)  # snow
            dashboard_page.reload(wait_until="domcontentloaded")
            dashboard_page.wait_for_selector(".rail", timeout=10000)

            weather = dashboard_page.locator(".status-item.wx")
            expect(weather).to_be_visible()
            expect(weather.locator("svg.wx-icon")).to_be_visible()
            expect(weather).to_contain_text("-5°C")
            expect(weather).to_have_attribute("data-wx-code", "71")
            expect(weather).to_have_attribute("title", "Snow")
        finally:
            _reset_weather(base_url)

    def test_icon_changes_with_the_weather_code(self, dashboard_page: Page, base_url: str):
        try:
            _apply_weather(dashboard_page, base_url, -5.0, 71)  # snow
            dashboard_page.reload(wait_until="domcontentloaded")
            snowy = dashboard_page.locator(".status-item.wx svg.wx-icon").inner_html()

            _apply_weather(dashboard_page, base_url, 22.0, 0)  # clear
            dashboard_page.reload(wait_until="domcontentloaded")
            clear = dashboard_page.locator(".status-item.wx svg.wx-icon").inner_html()

            # A single hard-coded glyph for every condition would pass the test
            # above; this is what proves the code actually selects artwork.
            assert snowy != clear
        finally:
            _reset_weather(base_url)

    def test_temperature_follows_the_unit_preference(self, dashboard_page: Page, base_url: str):
        original = dashboard_page.evaluate(
            "() => fetch('/api/preferences').then(r => r.json())")
        try:
            _apply_weather(dashboard_page, base_url, -5.0, 71)
            dashboard_page.evaluate(
                """() => fetch('/api/preferences', {
                     method: 'PATCH',
                     headers: { 'Content-Type': 'application/json' },
                     body: JSON.stringify({ temp_unit: 'fahrenheit' })
                   })""")
            dashboard_page.reload(wait_until="domcontentloaded")
            dashboard_page.wait_for_selector(".rail", timeout=10000)

            expect(dashboard_page.locator(".status-item.wx")).to_contain_text("23°F")
        finally:
            _reset_weather(base_url)
            dashboard_page.evaluate(
                """(unit) => fetch('/api/preferences', {
                     method: 'PATCH',
                     headers: { 'Content-Type': 'application/json' },
                     body: JSON.stringify({ temp_unit: unit })
                   })""",
                original.get("temp_unit", "celsius"))

    def test_weather_is_hidden_when_there_is_nothing_to_report(
            self, dashboard_page: Page, base_url: str):
        # An unavailable reading must leave the top bar clean rather than
        # showing a placeholder that reads like a real measurement.
        _reset_weather(base_url)
        valid = dashboard_page.evaluate(
            "() => fetch('/api/weather').then(r => r.json()).then(d => d.valid)")
        if valid:
            pytest.skip("device has a live weather reading; nothing to assert")
        dashboard_page.reload(wait_until="domcontentloaded")
        dashboard_page.wait_for_selector(".rail", timeout=10000)
        expect(dashboard_page.locator(".status-item.wx")).to_have_count(0)
