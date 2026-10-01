"""Tests for the settings workspace."""

import re

from playwright.sync_api import Page, expect


def open_settings(page: Page, section: str):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text=section).click()
    page.locator(".settings-nav .nav-link.active", has_text=section).wait_for()
    # settingsPage() renders the nav immediately but shows only a spinner in
    # the panel body until state.settingsLoaded flips (see main/web/index.html).
    # Waiting on the nav link alone therefore races the async load, and the
    # 5s default expect() timeout would intermittently fire against a panel
    # that was still spinning rather than one that was genuinely missing.
    page.locator(".settings-layout .spinner").wait_for(state="detached",
                                                       timeout=30000)


class TestSettingsWorkspace:
    def test_all_sections_present(self, dashboard_page: Page):
        dashboard_page.locator('button[aria-label="Settings"]').click()
        labels = dashboard_page.locator(".settings-nav .nav-link").all_inner_texts()
        assert labels == ["WiFi", "Firmware", "Time & Location", "Display", "Preferences",
                          "Security", "Home Assistant", "Heat output & COP", "Diagnostics", "System"]

    def test_wifi_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "WiFi")
        expect(dashboard_page.get_by_role("button", name="Scan")).to_be_visible()
        expect(dashboard_page.locator('form[data-form="wifi"]')).to_be_visible()

    def test_firmware_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Firmware")
        expect(dashboard_page.get_by_role("button", name="Check for updates")).to_be_visible()
        expect(dashboard_page.locator('input[type="file"]')).to_have_attribute("accept", ".bin,application/octet-stream")

    def test_check_for_updates_shows_progress(self, dashboard_page: Page):
        """The update check takes a couple of seconds; the button must show
        that it's working instead of doing nothing until the result lands."""
        open_settings(dashboard_page, "Firmware")
        held = []
        dashboard_page.route("**/api/ota/releases", lambda route: held.append(route))
        try:
            dashboard_page.get_by_role("button", name="Check for updates").click()
            checking = dashboard_page.get_by_role("button", name="Checking for updates…")
            expect(checking).to_be_visible()
            expect(checking).to_be_disabled()
            expect(checking.locator(".btn-spinner")).to_be_visible()
            expect(dashboard_page.get_by_role("button", name="Check for updates")).to_have_count(0)
            assert len(held) == 1
            held.pop().continue_()
            expect(dashboard_page.get_by_role("button", name="Check for updates")).to_be_enabled(timeout=30000)
            expect(checking).to_have_count(0)
        finally:
            dashboard_page.unroute("**/api/ota/releases")
            for route in held:
                route.continue_()

    def test_time_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Time")
        expect(dashboard_page.locator('select[name="timezone"]')).to_be_visible()
        expect(dashboard_page.get_by_role("button", name="Sync now")).to_be_visible()

    def test_display_controls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Display")
        slider = dashboard_page.locator('input[name="brightness"]')
        expect(slider).to_be_visible()
        expect(slider).to_have_attribute("min", "5")
        expect(slider).to_have_attribute("max", "100")

    def test_security_and_tls(self, dashboard_page: Page):
        open_settings(dashboard_page, "Security")
        tls_form = dashboard_page.locator('form[data-form="tls"]')
        expect(
            dashboard_page.locator(
                ".notice",
                has_text="Web login and API-key authentication are always required",
            )
        ).to_be_visible()
        expect(tls_form).to_be_visible()
        assert tls_form.locator("textarea").count() == 2

    def test_performance_controls(self, dashboard_page: Page):
        """Nothing here is saved: the checks only drive the form in place."""
        open_settings(dashboard_page, "Heat output & COP")
        expect(dashboard_page.get_by_role("heading", name="Current estimate")).to_be_visible()
        flow = dashboard_page.locator('input[name="flow_lpm"]')
        expect(flow).to_have_attribute("min", "1")
        expect(flow).to_have_attribute("max", "300")

        fluid = dashboard_page.locator('select[name="fluid"]')
        glycol = dashboard_page.locator('input[name="glycol_pct"]')
        fluid.select_option("water")
        expect(glycol).to_be_disabled()
        fluid.select_option("propylene_glycol")
        expect(glycol).to_be_enabled()
        expect(glycol).to_have_attribute("max", "60")
        expect(glycol).to_have_attribute("step", "5")

        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        expect(card.get_by_role("heading", name="Heat Pump Supply")).to_be_visible()
        card.locator('select[name="source"]').select_option("modbus_tcp")
        expect(card.locator('input[name="host"]')).to_be_visible()
        expect(card.get_by_role("button", name="Test sensor")).to_be_visible()
        card.locator("summary", has_text="Advanced").click()
        card.locator('select[name="value_type"]').select_option("float32")
        expect(card.locator('select[name="no_reading"]')).to_be_disabled()
        card.locator('select[name="value_type"]').select_option("int16")
        expect(card.locator('select[name="no_reading"]')).to_be_enabled()
        card.locator('select[name="source"]').select_option("bacnet_ip")
        expect(card.locator('input[name="host"]')).to_be_visible()
        expect(card.locator('input[name="object_instance"]')).to_be_visible()
        expect(card.locator('select[name="object_type"]')).to_be_visible()
        expect(card.get_by_role("button", name="Find sensors")).to_be_visible()
        expect(card.locator('input[name="unit_id"]')).to_be_hidden()
        card.locator('select[name="source"]').select_option("heat_pump")
        expect(card.locator('input[name="host"]')).to_be_hidden()

    def test_performance_bacnet_browse_pick(self, dashboard_page: Page):
        dashboard_page.route("**/api/performance/bacnet/browse", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","device_name":"Thermux Test","model_name":"Thermux","sensors":[{"object_type":"analog_input","object_instance":3,"object_name":"Supply tank","celsius":21.4,"units":62,"reliability":0,"rom_id":"28FF6491631603A2"}]}'))
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        card.locator('select[name="source"]').select_option("bacnet_ip")
        card.locator('input[name="host"]').fill("127.0.0.1")
        card.get_by_role("button", name="Find sensors").click()
        card.get_by_role("button", name=re.compile("Supply tank")).click()
        expect(card.locator('input[name="object_instance"]')).to_have_value("3")
        expect(card.locator('select[name="object_type"]')).to_have_value("analog_input")

    def test_performance_sensor_test_reports_connect_error(self, dashboard_page: Page):
        """Nothing listens on the controller's own port 1, so the test must
        come back with the friendly connection error rather than hang."""
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="return"]')
        card.locator('select[name="source"]').select_option("modbus_tcp")
        card.locator('input[name="host"]').fill("127.0.0.1")
        card.locator('input[name="port"]').fill("1")
        card.get_by_role("button", name="Test sensor").click()
        result = card.locator("#perf-test-return")
        expect(result).to_contain_text("No reading", timeout=15000)
        expect(result).to_contain_text("Couldn't connect")

    def test_performance_sensor_test_adopts_register_type(self, dashboard_page: Page):
        """When the test finds the value in the other register table, the form
        switches to it so Save stores what worked."""
        dashboard_page.route("**/api/performance/test", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","register_type":"holding","celsius":45.5,"thermux":null}'))
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="return"]')
        card.locator('select[name="source"]').select_option("modbus_tcp")
        card.locator('input[name="host"]').fill("127.0.0.1")
        card.get_by_role("button", name="Test sensor").click()
        result = card.locator("#perf-test-return")
        expect(result).to_contain_text("Reading OK")
        expect(result).to_contain_text("Register type set to Holding")
        expect(card.locator('select[name="register_type"]')).to_have_value("holding")

    def test_diagnostics(self, dashboard_page: Page):
        open_settings(dashboard_page, "Diagnostics")
        expect(dashboard_page.locator("#log-container")).to_be_visible()
        expect(dashboard_page.locator('a[href="/api/heatpump/diagnostic"]')).to_be_visible()
        expect(dashboard_page.locator('a[href="/api/screenshot?format=jpeg"]')).to_be_visible()

    def test_system_and_factory_reset(self, dashboard_page: Page):
        open_settings(dashboard_page, "System")
        expect(dashboard_page.get_by_role("button", name="Restart controller")).to_be_visible()
        expect(dashboard_page.get_by_role("button", name="Erase and reset")).to_be_visible()
