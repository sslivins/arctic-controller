"""Web dashboard tests for the notification bell (mirrors the on-device bell)."""

import requests
import urllib3
from playwright.sync_api import Page, expect

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


def _add_notification(base_url: str, ntype: int = 0, message: str = "Firmware v99.0.0 available"):
    r = requests.post(
        f"{base_url}/api/test/notification-mock",
        json={"type": ntype, "message": message},
        timeout=10,
        verify=False,
    )
    r.raise_for_status()
    return r.json()


def _reset_notifications(base_url: str):
    requests.post(
        f"{base_url}/api/test/notification-mock-reset",
        json={},
        timeout=10,
        verify=False,
    )


class TestNotificationBellWeb:
    def test_notifications_endpoint_reports_active(self, base_url: str):
        _reset_notifications(base_url)
        try:
            _add_notification(base_url)
            r = requests.get(f"{base_url}/api/notifications", timeout=10, verify=False)
            r.raise_for_status()
            data = r.json()
            assert data["count"] >= 1
            keys = [n["key"] for n in data["notifications"]]
            assert "firmware_update" in keys
        finally:
            _reset_notifications(base_url)

    def test_bell_shows_badge_and_panel(self, dashboard_page: Page, base_url: str):
        _reset_notifications(base_url)
        try:
            _add_notification(base_url)
            # Reload so loadCore() picks up the seeded notification.
            dashboard_page.reload(wait_until="domcontentloaded")
            dashboard_page.wait_for_selector(".rail", timeout=10000)

            # Badge shows the active count.
            expect(dashboard_page.locator(".notif-badge")).to_be_visible()

            # Opening the bell reveals the notification item.
            dashboard_page.locator(".notif-btn").click()
            expect(dashboard_page.locator(".notif-panel")).to_be_visible()
            expect(
                dashboard_page.locator(".notif-item", has_text="Firmware v99.0.0 available")
            ).to_be_visible()
        finally:
            _reset_notifications(base_url)

    def test_firmware_notification_navigates_to_firmware(self, dashboard_page: Page, base_url: str):
        _reset_notifications(base_url)
        try:
            _add_notification(base_url)
            dashboard_page.reload(wait_until="domcontentloaded")
            dashboard_page.wait_for_selector(".rail", timeout=10000)

            dashboard_page.locator(".notif-btn").click()
            dashboard_page.locator(".notif-item", has_text="Firmware").click()

            # Firmware settings page renders its heading.
            expect(
                dashboard_page.get_by_role("heading", name="Firmware")
            ).to_be_visible()
        finally:
            _reset_notifications(base_url)
