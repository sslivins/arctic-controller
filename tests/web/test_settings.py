"""Tests for the settings workspace."""

import json
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


def perf_config(supply=None, return_=None):
    hp = {"source": "heat_pump", "host": "", "port": 502, "unit_id": 1,
          "register": 0, "register_type": "input", "object_type": "analog_input",
          "object_instance": None, "value_type": "int16", "scale": 0.01,
          "no_reading": "0x8000", "device_instance": None, "device_name": None,
          "object_name": None, "rom_id": None}
    return {"flow_lpm": 40, "fluid": "water", "glycol_pct": 0,
            "limits": {"flow_min_lpm": 1, "flow_max_lpm": 300, "glycol_max_pct": 60,
                       "glycol_step_pct": 5},
            "status": {},
            "sensors": {"supply": supply or dict(hp), "return": return_ or dict(hp)}}


def heat_pump_sensor_with_stale_network_fields():
    sensor = perf_config()["sensors"]["supply"]
    sensor.update({"source": "heat_pump", "host": "192.168.9.3", "port": 502,
                   "object_instance": 7, "device_name": "Old device",
                   "object_name": "Old sensor"})
    return sensor


def assert_summary_change_buttons_inline(device_row, sensor_row):
    device_box = device_row.bounding_box()
    sensor_box = sensor_row.bounding_box()
    device_change = device_row.get_by_role("button", name="Change").bounding_box()
    sensor_change = sensor_row.get_by_role("button", name="Change").bounding_box()
    assert device_box and sensor_box and device_change and sensor_change
    assert abs(device_change["width"] - sensor_change["width"]) < 1
    assert device_box["y"] <= device_change["y"] <= device_box["y"] + device_box["height"]
    assert sensor_box["y"] <= sensor_change["y"] <= sensor_box["y"] + sensor_box["height"]


def request_json(request):
    data = request.post_data_json
    return data() if callable(data) else data


def route_perf_config(page: Page, current=None, saved_payloads=None):
    state = {"current": current or perf_config()}

    def handler(route):
        if route.request.method == "PUT":
            body = request_json(route.request)
            if saved_payloads is not None:
                saved_payloads.append(body)
            state["current"] = body
        route.fulfill(status=200, content_type="application/json",
                      body=json.dumps(state["current"]))

    page.route("**/api/performance/config", handler)
    return state



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
        route_perf_config(dashboard_page)
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
        expect(card.locator('input[name="host"]')).to_be_hidden()
        expect(card.locator("strong", has_text=re.compile("^Device$"))).to_be_visible()
        card.get_by_role("button", name="Enter address manually").click()
        expect(card.locator('input[name="host"]')).to_be_visible()
        expect(card.get_by_role("button", name="Find sensors")).to_be_visible()
        expect(card.locator('input[name="object_instance"]')).to_be_hidden()
        expect(card.locator('select[name="object_type"]')).to_be_hidden()
        expect(card.locator('input[name="unit_id"]')).to_be_hidden()
        card.locator('select[name="source"]').select_option("heat_pump")
        expect(card.locator('input[name="host"]')).to_be_hidden()

    def test_performance_bacnet_browse_pick(self, dashboard_page: Page):
        route_perf_config(dashboard_page)
        dashboard_page.route("**/api/performance/bacnet/browse", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","device_name":"Thermux Test","model_name":"Thermux","sensors":[{"object_type":"analog_input","object_instance":3,"object_name":"Supply tank","celsius":21.4,"units":62,"reliability":0,"rom_id":"28FF6491631603A2"}]}'))
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        card.locator('select[name="source"]').select_option("bacnet_ip")
        card.get_by_role("button", name="Enter address manually").click()
        card.locator('input[name="host"]').fill("127.0.0.1")
        card.get_by_role("button", name="Find sensors").click()
        card.get_by_role("button", name=re.compile("Supply tank")).click()
        expect(card.locator('input[name="object_instance"]')).to_have_value("3")
        expect(card.locator('select[name="object_type"]')).to_have_value("analog_input")

    def test_performance_bacnet_manual_object_path(self, dashboard_page: Page):
        route_perf_config(dashboard_page)
        dashboard_page.route("**/api/performance/bacnet/browse", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","device_name":"Thermux Test","model_name":"Thermux","device_instance":1234,"sensors":[],"truncated":false}'))
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        card.locator('select[name="source"]').select_option("bacnet_ip")
        card.get_by_role("button", name="Enter address manually").click()
        card.locator('input[name="host"]').fill("127.0.0.1")
        card.get_by_role("button", name="Find sensors").click()
        card.get_by_role("button", name="Enter object manually").click()
        expect(card.locator('input[name="object_instance"]')).to_be_visible()
        expect(card.locator('select[name="object_type"]')).to_be_visible()

    def test_performance_bacnet_discover_pick(self, dashboard_page: Page):
        route_perf_config(dashboard_page)
        dashboard_page.route("**/api/performance/bacnet/discover", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","truncated":false,"devices":[{"host":"127.0.0.1","port":47809,"device_instance":1234,"device_name":"Thermux Test","model_name":"Thermux","vendor_id":15}]}'))
        dashboard_page.route("**/api/performance/bacnet/browse", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","device_name":"Thermux Test","model_name":"Thermux","device_instance":1234,"sensors":[{"object_type":"analog_input","object_instance":3,"object_name":"Supply tank","celsius":21.4,"units":62,"reliability":0,"rom_id":"28FF6491631603A2"}]}'))
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        card.locator('select[name="source"]').select_option("bacnet_ip")
        card.get_by_role("button", name=re.compile("Thermux Test")).click()
        expect(card.locator('input[name="host"]')).to_have_value("127.0.0.1")
        expect(card.locator('input[name="port"]')).to_have_value("47809")
        expect(card.get_by_role("button", name=re.compile("Supply tank"))).to_be_visible()

    def test_performance_bacnet_draft_survives_discovery_pick_and_save(self, dashboard_page: Page):
        saved_payloads = []
        route_perf_config(dashboard_page,
                          perf_config(supply=heat_pump_sensor_with_stale_network_fields()),
                          saved_payloads)
        dashboard_page.route("**/api/performance/bacnet/discover", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","truncated":false,"devices":[{"host":"192.168.1.205","port":47808,"device_instance":205,"device_name":"Thermux Spare","model_name":"Thermux","vendor_id":15}]}'))
        dashboard_page.route("**/api/performance/bacnet/browse", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","device_name":"Thermux Spare","model_name":"Thermux","device_instance":205,"sensors":[{"object_type":"analog_input","object_instance":4,"object_name":"Sensor B","celsius":21.8,"units":62,"reliability":0,"available":true,"rom_id":"28FF6491631603A2"}],"truncated":false}'))

        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        card.locator('select[name="source"]').select_option("bacnet_ip")

        expect(card.locator('select[name="source"]')).to_have_value("bacnet_ip")
        expect(card.get_by_role("button", name="Test sensor")).to_be_hidden()
        expect(card.get_by_text(re.compile(r"Device.*192\.168\.9\.3"))).to_have_count(0)
        pick_device = card.get_by_role("button", name=re.compile("Thermux Spare"))
        expect(pick_device).to_be_visible(timeout=10000)
        assert pick_device.is_visible()
        assert card.locator("#perf-discover-supply").is_visible()
        expect(card.get_by_role("button", name="Test sensor")).to_be_hidden()

        pick_device.click()
        device_row = card.locator(".perf-summary", has_text="Device")
        expect(device_row).to_contain_text("Thermux Spare")
        sensor = card.get_by_role("button", name=re.compile("Sensor B"))
        expect(sensor).to_be_visible()
        assert sensor.is_visible()
        expect(card.get_by_role("button", name="Test sensor")).to_be_hidden()

        sensor.click()
        device_row = card.locator(".perf-summary", has_text="Device")
        sensor_row = card.locator(".perf-summary", has_text="Sensor")
        expect(device_row).to_contain_text("Thermux Spare")
        expect(sensor_row).to_contain_text("Sensor B")
        # The pick already read the sensor, so its reading replaces Test sensor.
        expect(sensor_row).to_contain_text("21.8")
        expect(card.get_by_role("button", name="Test sensor")).to_be_hidden()
        assert_summary_change_buttons_inline(device_row, sensor_row)
        dashboard_page.set_viewport_size({"width": 430, "height": 900})
        assert_summary_change_buttons_inline(device_row, sensor_row)
        dashboard_page.set_viewport_size({"width": 1000, "height": 900})
        assert_summary_change_buttons_inline(device_row, sensor_row)

        with dashboard_page.expect_response(
                lambda r: r.request.method == "PUT" and r.url.endswith("/api/performance/config")):
            card.get_by_role("button", name="Save").click()
        assert saved_payloads
        saved = saved_payloads[0]["sensors"]["supply"]
        assert saved["source"] == "bacnet_ip"
        assert saved["host"] == "192.168.1.205"
        assert saved["port"] == 47808
        assert saved["device_instance"] == 205
        assert saved["device_name"] == "Thermux Spare"
        assert saved["object_type"] == "analog_input"
        assert saved["object_instance"] == 4
        assert saved["object_name"] == "Sensor B"
        assert saved["rom_id"] == "28FF6491631603A2"

    def test_performance_bacnet_manual_object_order_and_validation(self, dashboard_page: Page):
        saved_payloads = []
        route_perf_config(dashboard_page,
                          perf_config(supply=heat_pump_sensor_with_stale_network_fields()),
                          saved_payloads)
        dashboard_page.route("**/api/performance/bacnet/discover", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","truncated":false,"devices":[{"host":"192.168.1.205","port":47808,"device_instance":205,"device_name":"Thermux Spare","model_name":"Thermux","vendor_id":15}]}'))
        dashboard_page.route("**/api/performance/bacnet/browse", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","device_name":"Thermux Spare","model_name":"Thermux","device_instance":205,"sensors":[],"truncated":false}'))

        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        card.locator('select[name="source"]').select_option("bacnet_ip")
        card.get_by_role("button", name=re.compile("Thermux Spare")).click()
        card.get_by_role("button", name="Enter object manually").click()

        device_row = card.locator(".perf-summary", has_text="Device")
        object_type = card.locator('select[name="object_type"]')
        object_instance = card.locator('input[name="object_instance"]')
        expect(device_row).to_contain_text("Thermux Spare")
        expect(object_type).to_be_visible()
        expect(object_instance).to_be_visible()
        expect(object_instance).to_have_value("")
        device_box = device_row.bounding_box()
        type_box = object_type.bounding_box()
        instance_box = object_instance.bounding_box()
        assert device_box and type_box and instance_box
        assert device_box["y"] < type_box["y"] < instance_box["y"]
        expect(card.get_by_role("button", name="Test sensor")).to_be_hidden()
        object_instance.fill("4")
        expect(card.get_by_role("button", name="Test sensor")).to_be_visible()
        object_instance.fill("")

        card.get_by_role("button", name="Save").click()
        expect(dashboard_page.locator(".toast.bad")).to_contain_text("Choose a BACnet object")
        assert saved_payloads == []

    def test_performance_bacnet_list_actions_are_separate_from_choices(self, dashboard_page: Page):
        route_perf_config(dashboard_page)
        dashboard_page.route("**/api/performance/bacnet/discover", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body=json.dumps({"ok": True, "error": "none", "truncated": False, "devices": [
                {"host": f"10.0.0.{i}", "port": 47808, "device_instance": 100 + i,
                 "device_name": f"Thermux {i}", "model_name": "Thermux", "vendor_id": 15}
                for i in range(10)]})))
        dashboard_page.route("**/api/performance/bacnet/browse", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","device_name":"Thermux 0","device_instance":100,"sensors":[{"object_type":"analog_input","object_instance":1,"object_name":"Sensor A","celsius":21.1,"units":62,"reliability":0,"rom_id":""}],"truncated":false}'))
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        card.locator('select[name="source"]').select_option("bacnet_ip")

        choices = card.locator("#perf-discover-supply .perf-choices")
        expect(choices.locator('[data-action="perf-pick-bacnet-device"]')).to_have_count(10)
        expect(choices.locator('[data-action="perf-discover-bacnet"]')).to_have_count(0)
        expect(choices.locator('[data-action="perf-bacnet-manual"]')).to_have_count(0)
        refresh = card.locator('#perf-discover-supply .perf-list-head [data-action="perf-discover-bacnet"]')
        expect(refresh).to_have_attribute("aria-label", "Search again")
        expect(card.locator("#perf-discover-supply .perf-not-listed")).to_contain_text("Not listed?")
        expect(card.locator('#perf-discover-supply .perf-not-listed [data-action="perf-bacnet-manual"]')).to_be_visible()

        card.get_by_role("button", name=re.compile("Thermux 0")).click()
        sensors = card.locator("#perf-browse-supply .perf-choices")
        expect(sensors.locator('[data-action="perf-pick-bacnet"]')).to_have_count(1)
        expect(sensors.locator('[data-action="perf-bacnet-manual-object"]')).to_have_count(0)
        expect(card.locator('#perf-browse-supply .perf-not-listed [data-action="perf-bacnet-manual-object"]')).to_be_visible()

    def test_performance_bacnet_no_devices_offers_plain_actions(self, dashboard_page: Page):
        route_perf_config(dashboard_page)
        dashboard_page.route("**/api/performance/bacnet/discover", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","truncated":false,"devices":[]}'))
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        card.locator('select[name="source"]').select_option("bacnet_ip")
        expect(card.get_by_text("No BACnet devices found")).to_be_visible(timeout=10000)
        expect(card.locator('.perf-choices [data-action="perf-discover-bacnet"]')).to_be_visible()
        expect(card.locator('.perf-choices [data-action="perf-bacnet-manual"]')).to_be_visible()
        expect(card.locator(".perf-not-listed")).to_have_count(0)

    def test_performance_bacnet_change_device_manual_address_is_clean(self, dashboard_page: Page):
        saved = perf_config()["sensors"]["supply"]
        saved.update({"source": "bacnet_ip", "host": "192.168.1.205", "port": 47808,
                      "object_instance": 0, "device_instance": 205,
                      "device_name": "Thermux Spare", "object_name": "Sensor B"})
        cfg = perf_config(supply=saved)
        cfg["status"] = {"sensors": {"supply": {"celsius": 22.9, "age_s": 2, "error": "none",
                                                "object_name": "Sensor B"}}}
        route_perf_config(dashboard_page, cfg)
        dashboard_page.route("**/api/performance/bacnet/discover", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","truncated":false,"devices":[{"host":"192.168.1.205","port":47808,"device_instance":205,"device_name":"Thermux Spare","model_name":"Thermux","vendor_id":15}]}'))

        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        expect(card.get_by_text("Latest reading")).to_be_visible()

        card.locator('[data-action="perf-change-bacnet-device"]').click()
        expect(card.get_by_role("button", name=re.compile("Thermux Spare"))).to_be_visible(timeout=10000)
        expect(card.get_by_text("Latest reading")).to_have_count(0)

        card.get_by_role("button", name="Enter address manually").click()
        expect(card.locator('input[name="host"]')).to_be_visible()
        expect(card.get_by_role("button", name="Find sensors")).to_be_visible()
        expect(card.get_by_text("Searching")).to_have_count(0)
        expect(card.locator("#perf-discover-supply")).to_be_hidden()
        expect(card.get_by_text("Latest reading")).to_have_count(0)
        # Going back is a link, not a second button next to Find sensors.
        expect(card.locator(".perf-choices", has_text="Find sensors").locator('[data-action="perf-discover-bacnet"]')).to_have_count(0)
        back = card.locator('.perf-back .link-btn[data-action="perf-discover-bacnet"]')
        expect(back).to_have_text("Back to search")
        back.click()
        expect(card.get_by_role("button", name=re.compile("Thermux Spare"))).to_be_visible(timeout=10000)

    def test_performance_bacnet_test_only_for_manual_object(self, dashboard_page: Page):
        saved = perf_config()["sensors"]["supply"]
        saved.update({"source": "bacnet_ip", "host": "192.168.1.205", "port": 47808,
                      "object_instance": 0, "device_instance": 205,
                      "device_name": "Thermux Spare", "object_name": "Sensor B"})
        route_perf_config(dashboard_page, perf_config(supply=saved))
        dashboard_page.route("**/api/performance/bacnet/browse", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","device_name":"Thermux Spare","model_name":"Thermux","device_instance":205,"sensors":[{"object_type":"analog_input","object_instance":1,"object_name":"Sensor A","celsius":21.1,"units":62,"reliability":0,"available":true,"rom_id":""}],"truncated":false}'))

        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="supply"]')
        expect(card.locator(".perf-summary", has_text="Sensor")).to_contain_text("Sensor B")
        expect(card.get_by_role("button", name="Test sensor")).to_be_hidden()
        card.locator('[data-action="perf-change-bacnet-sensor"]').click()
        expect(card.get_by_role("button", name=re.compile("Sensor A"))).to_be_visible(timeout=10000)
        expect(card.get_by_role("button", name="Test sensor")).to_be_hidden()
        card.get_by_role("button", name=re.compile("Sensor A")).click()
        sensor_row = card.locator(".perf-summary", has_text="Sensor")
        expect(sensor_row).to_contain_text("Sensor A")
        expect(sensor_row).to_contain_text("21.1")
        expect(card.get_by_role("button", name="Test sensor")).to_be_hidden()

        card.locator('[data-action="perf-change-bacnet-sensor"]').click()
        card.get_by_role("button", name="Enter object manually").click()
        card.locator('input[name="object_instance"]').fill("7")
        expect(card.get_by_role("button", name="Test sensor")).to_be_visible()

    def test_performance_sensor_test_reports_connect_error(self, dashboard_page: Page):
        """Nothing listens on the controller's own port 1, so the test must
        come back with the friendly connection error rather than hang."""
        route_perf_config(dashboard_page)
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="return"]')
        card.locator('select[name="source"]').select_option("modbus_tcp")
        expect(card.locator('select[name="source"]')).to_have_value("modbus_tcp")
        expect(card.locator('input[name="host"]')).to_be_visible()
        card.locator('input[name="host"]').fill("127.0.0.1")
        card.locator('input[name="port"]').fill("1")
        card.get_by_role("button", name="Test sensor").click()
        result = card.locator("#perf-test-return")
        expect(result).to_contain_text("No reading", timeout=15000)
        expect(result).to_contain_text("Couldn't connect")

    def test_performance_sensor_test_adopts_register_type(self, dashboard_page: Page):
        """When the test finds the value in the other register table, the form
        switches to it so Save stores what worked."""
        route_perf_config(dashboard_page)
        dashboard_page.route("**/api/performance/test", lambda route: route.fulfill(
            status=200, content_type="application/json",
            body='{"ok":true,"error":"none","register_type":"holding","celsius":45.5,"thermux":null}'))
        open_settings(dashboard_page, "Heat output & COP")
        card = dashboard_page.locator('form[data-form="perf-sensor"][data-slot="return"]')
        card.locator('select[name="source"]').select_option("modbus_tcp")
        expect(card.locator('select[name="source"]')).to_have_value("modbus_tcp")
        expect(card.locator('input[name="host"]')).to_be_visible()
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

    def test_factory_reset_uses_themed_dialog_and_needs_typed_phrase(self, dashboard_page: Page):
        reset_calls = []
        dashboard_page.route("**/api/factory-reset", lambda route: (reset_calls.append(1), route.abort()))
        native_dialogs = []
        dashboard_page.on("dialog", lambda d: (native_dialogs.append(d.message), d.dismiss()))
        open_settings(dashboard_page, "System")

        dashboard_page.get_by_role("button", name="Erase and reset").click()
        dialog = dashboard_page.get_by_role("alertdialog")
        expect(dialog).to_be_visible()
        expect(dialog).to_contain_text("Factory reset this controller?")
        erase = dialog.get_by_role("button", name="Erase controller")
        expect(erase).to_be_disabled()
        dialog.locator("input").fill("factory")
        expect(erase).to_be_disabled()
        dialog.locator("input").fill("factory-reset")
        expect(erase).to_be_enabled()
        dialog.get_by_role("button", name="Cancel").click()
        expect(dialog).to_have_count(0)

        dashboard_page.get_by_role("button", name="Erase and reset").click()
        expect(dialog).to_be_visible()
        dashboard_page.keyboard.press("Escape")
        expect(dialog).to_have_count(0)

        assert not reset_calls
        assert not native_dialogs
