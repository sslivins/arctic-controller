"""Tests for the Status page: grouped readings with OEM code pills, the
summary card, fault banner and defrost panel."""

import pytest
from playwright.sync_api import Page, expect


def _open_status(page: Page, status: dict | None = None, errors: list | None = None,
                 fahrenheit: bool = False) -> Page:
    """Open Status with /api/heatpump/status patched to a steady heating run
    plus ``status`` overrides (nested dicts are merged)."""
    def patch_status(route):
        resp = route.fetch()
        body = resp.json()
        body.update(connected=True, data_age_s=2, has_error=False, defrosting=False, defrost=None,
                    compressor=True, pump=True, fans=True, operation="heating", pump_on_s=600)
        body["temperatures"].update(outlet=45, inlet=40, outdoor=5, outdoor_coil=0, suction=3,
                                    discharge=78, indoor_coil=47, ipm=50)
        body["readings"].update(primary_eev=180, power_consumption=3000, heat_out=10400, cop=3.4,
                                perf_source="heat_pump", supply_c=45, return_c=40, delta_t=5)
        for key, value in (status or {}).items():
            if isinstance(value, dict) and isinstance(body.get(key), dict):
                body[key].update(value)
            else:
                body[key] = value
        route.fulfill(response=resp, json=body)

    def patch_errors(route):
        resp = route.fetch()
        body = resp.json()
        body["active"] = errors
        route.fulfill(response=resp, json=body)

    def patch_prefs(route):
        if route.request.method != "GET":
            route.continue_()
            return
        resp = route.fetch()
        body = resp.json()
        body["temp_unit"] = "fahrenheit"
        route.fulfill(response=resp, json=body)

    page.route("**/api/heatpump/status", patch_status)
    if errors is not None:
        page.route("**/api/heatpump/errors", patch_errors)
    if fahrenheit:
        page.route("**/api/preferences", patch_prefs)
    page.reload(wait_until="domcontentloaded")
    page.locator(".rail .nav-link:visible, .mobile-nav .nav-link:visible", has_text="Status").first.click()
    expect(page.locator("[data-status='summary'] [data-fresh='live']")).to_be_visible(timeout=15000)
    return page


def _row(page: Page, key: str):
    return page.locator(f".rd[data-reading='{key}']")


class TestStatusPage:
    def test_rows_lead_with_a_code_pill(self, dashboard_page: Page):
        page = _open_status(dashboard_page)
        expect(_row(page, "outlet").locator(".code")).to_have_text("o3")
        expect(_row(page, "discharge").locator(".code")).to_have_text("A1")
        # Derived readings keep an empty pill-width gap so names line up.
        expect(_row(page, "water_dt").locator(".code.blank")).to_have_count(1)
        assert "o3" not in _row(page, "outlet").locator(".nm").inner_text()

    def test_summary_and_chips(self, dashboard_page: Page):
        page = _open_status(dashboard_page)
        expect(page.locator("[data-stat='now'] .v")).to_have_text("Heating")
        expect(page.locator("[data-chip='compressor']")).to_have_attribute("data-on", "true")
        expect(page.locator("[data-chip='defrost']")).to_have_text("Defrost Off")
        expect(page.locator("[data-status='defrost']")).to_have_count(0)
        expect(page.locator("[data-status='fault']")).to_have_count(0)

    def test_eev_shows_steps_and_percent_open(self, dashboard_page: Page):
        row = _row(_open_status(dashboard_page), "primary_eev")
        expect(row.locator(".val")).to_have_text("180steps")
        expect(row.locator(".sub")).to_have_text("36% open")

    def test_negative_estimate_is_not_shown(self, dashboard_page: Page):
        page = _open_status(dashboard_page, {"temperatures": {"suction": -4}})
        expect(_row(page, "superheat").locator(".val")).to_have_text("--")
        expect(_row(page, "superheat").locator(".sub")).to_have_text("Below zero: sensors disagree")
        expect(_row(page, "coil_approach").locator(".val")).to_have_text("5°C")

    def test_water_dt_blank_when_pump_off(self, dashboard_page: Page):
        page = _open_status(dashboard_page, {"pump": False, "pump_on_s": None,
                                             "readings": {"delta_t": None}})
        expect(_row(page, "water_dt").locator(".val")).to_have_text("--")
        expect(_row(page, "cop").locator(".val")).to_have_text("--")

    def test_unit_source_has_no_supply_return_rows(self, dashboard_page: Page):
        page = _open_status(dashboard_page)
        expect(_row(page, "water_dt").locator(".val")).to_have_text("+5°C")
        expect(_row(page, "water_dt").locator(".sub")).to_have_text("Outlet − inlet")
        expect(_row(page, "supply")).to_have_count(0)

    def test_external_source_shows_tenths(self, dashboard_page: Page):
        page = _open_status(dashboard_page, {"readings": {
            "perf_source": "external", "supply_c": 38.6, "return_c": 34.1, "delta_t": 4.5}})
        expect(_row(page, "supply").locator(".val")).to_have_text("38.6°C")
        expect(_row(page, "return").locator(".val")).to_have_text("34.1°C")
        dt = _row(page, "water_dt")
        expect(dt.locator(".val")).to_have_text("+4.5°C")
        expect(dt.locator(".sub")).to_have_text("Supply − return · external sensors")

    def test_deltas_convert_without_offset_in_fahrenheit(self, dashboard_page: Page):
        page = _open_status(dashboard_page, fahrenheit=True)
        expect(_row(page, "outlet").locator(".val")).to_have_text("113°F")
        expect(_row(page, "water_dt").locator(".val")).to_have_text("+9°F")

    def test_fault_banner(self, dashboard_page: Page):
        page = _open_status(dashboard_page, {"has_error": True}, errors=[
            {"code": "P04", "name": "Discharge temperature protection", "description": "", "occurred": 0,
             "resolution": "", "help_url": ""}])
        alert = page.locator("[data-status='fault']")
        expect(alert.locator(".code")).to_have_text("P04")
        expect(alert).to_contain_text("Discharge temperature protection")

    def test_defrost_panel_and_paused_estimates(self, dashboard_page: Page):
        page = _open_status(dashboard_page, {
            "defrosting": True, "operation": "defrost", "defrost": {"elapsed_s": 125, "start_coil": -3},
            "temperatures": {"outlet": 33, "inlet": 38}, "readings": {"delta_t": -5}})
        panel = page.locator("[data-status='defrost']")
        expect(panel.locator("[data-defrost-elapsed]")).to_have_text("2m 05s")
        expect(panel).to_contain_text("Coil at start")
        expect(page.locator("[data-chip='defrost']")).to_have_attribute("data-on", "true")
        for key in ("heat_out", "cop"):
            expect(_row(page, key).locator(".val")).to_have_text("--")
            expect(_row(page, key).locator(".sub")).to_have_text("Paused during defrost")
        expect(_row(page, "superheat").locator(".sub")).to_have_text("Hidden during defrost")
        dt = _row(page, "water_dt")
        expect(dt).to_have_class("rd warn")
        expect(dt.locator(".val")).to_have_text("−5°C")
        expect(dt.locator(".sub")).to_have_text("Reversed during defrost")

    @pytest.mark.parametrize("width,folded", [(1280, False), (390, True)])
    def test_phone_folds_secondary_groups(self, dashboard_page: Page, width: int, folded: bool):
        dashboard_page.set_viewport_size({"width": width, "height": 900})
        page = _open_status(dashboard_page)
        for group in ("water", "perf"):
            expect(page.locator(f"[data-group='{group}']")).to_have_attribute("open", "")
        for group in ("refrig", "outdoor", "elec"):
            details = page.locator(f"[data-group='{group}']")
            if folded:
                expect(details).not_to_have_attribute("open", "")
            else:
                expect(details).to_have_attribute("open", "")
        if folded:
            # The user's choice survives the 5 s poll re-render (the second poll
            # starts only after the first one has re-rendered).
            page.locator("[data-group='refrig'] > summary").click()
            for _ in range(2):
                with page.expect_response("**/api/heatpump/status", timeout=15000):
                    pass
            expect(page.locator("[data-group='refrig']")).to_have_attribute("open", "")


class TestAdvancedParams:
    def test_rows_lead_with_a_code_pill(self, dashboard_page: Page):
        dashboard_page.locator(".rail .nav-link", has_text="Control").click()
        row = dashboard_page.locator("form.ap-row").first
        expect(row.locator(".ap-title .code")).to_have_text(row.get_attribute("data-key"), timeout=15000)
        assert " · " not in row.locator(".ap-title strong").inner_text()
        expect(dashboard_page.locator("details.ap-group > summary .cnt").first).to_be_visible()
