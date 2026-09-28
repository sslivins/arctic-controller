"""Tests for device language and temperature-unit preferences."""

from playwright.sync_api import Page, expect


def open_preferences(page: Page):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text="Preferences").click()


class TestPreferences:
    def test_language_options(self, dashboard_page: Page):
        open_preferences(dashboard_page)
        select = dashboard_page.locator('select[name="language"]')
        expect(select).to_be_visible()
        assert select.locator("option").count() == 3

    def test_temperature_unit_options(self, dashboard_page: Page):
        open_preferences(dashboard_page)
        select = dashboard_page.locator('select[name="temp_unit"]')
        expect(select).to_be_visible()
        assert select.locator("option").count() == 2

    def test_demo_mode_control(self, dashboard_page: Page):
        open_preferences(dashboard_page)
        expect(dashboard_page.locator('input[name="demo_mode"]')).to_be_visible()

    def test_demo_mode_checkbox_sits_beside_its_label(self, dashboard_page: Page):
        dashboard_page.set_viewport_size({"width": 1280, "height": 900})
        open_preferences(dashboard_page)
        box = dashboard_page.locator('input[name="demo_mode"]').bounding_box()
        label = dashboard_page.locator("label", has_text="Enable demo mode").bounding_box()
        assert box and label
        assert box["height"] < label["height"] + 1 and abs(
            (box["y"] + box["height"] / 2) - (label["y"] + label["height"] / 2)) < 4, \
            f"checkbox {box} is not on the same line as its label {label}"

    def test_demo_mode_toggle_asks_before_restarting(self, dashboard_page: Page):
        page = dashboard_page
        # Never let this test change the setting or restart the controller.
        writes = []
        page.route("**/api/ota/reboot", lambda r: (writes.append(r.request.url), r.abort()))
        page.route("**/api/preferences", lambda r: (writes.append(r.request.url), r.abort())
                   if r.request.method != "GET" else r.continue_())
        open_preferences(page)
        checkbox = page.locator('input[name="demo_mode"]')
        was_on = checkbox.is_checked()
        checkbox.click()

        dialog = page.locator("#overlay .confirm-dialog")
        expect(dialog).to_be_visible()
        expect(dialog.locator("h3")).to_have_text(
            "Turn off demo mode?" if was_on else "Turn on demo mode?")
        expect(dialog).to_contain_text("Requires a restart.")
        assert checkbox.is_checked() == was_on, "checkbox changed before the user confirmed"

        dialog.locator('[data-action="demo-cancel"]').click()
        expect(page.locator("#overlay")).to_have_count(0)
        assert checkbox.is_checked() == was_on
        assert writes == [], f"cancel still sent {writes}"

    def test_no_permanent_restart_note(self, dashboard_page: Page):
        open_preferences(dashboard_page)
        expect(dashboard_page.locator('input[name="demo_mode"]')).to_be_visible()
        expect(dashboard_page.get_by_text("Changing demo mode requires a restart")).to_have_count(0)

    def test_demo_banner_matches_demo_state(self, dashboard_page: Page):
        demo = dashboard_page.evaluate(
            "() => fetch('/api/heatpump/status').then(r => r.json()).then(s => !!s.demo_mode)")
        banner = dashboard_page.locator(".demo-banner")
        if demo:
            expect(banner).to_have_text("Demo mode")
        else:
            expect(banner).to_have_count(0)
