"""Tests for TLS controls and server-side prerequisites."""

from playwright.sync_api import Page, expect

def open_security(page: Page):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text="Security").click()
    page.locator('form[data-form="tls"]').wait_for()


class TestTlsUI:
    def test_tls_card_and_pem_fields(self, dashboard_page: Page):
        open_security(dashboard_page)
        form = dashboard_page.locator('form[data-form="tls"]')
        expect(form).to_be_visible()
        assert form.locator("textarea").count() == 2

    def test_tls_button_reflects_auth_prerequisite(self, dashboard_page: Page):
        open_security(dashboard_page)
        button = dashboard_page.locator('form[data-form="tls"] button[type="submit"]')
        expect(button).to_be_enabled()


def test_authentication_cannot_be_disabled(dashboard_page: Page):
    status = dashboard_page.evaluate(
        """async () => {
          const response = await fetch("/api/auth/config", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({
              web_auth_enabled: false,
              api_auth_enabled: false,
            }),
          });
          return response.status;
        }"""
    )
    assert status == 403
