"""Tests for TLS controls and server-side prerequisites."""

import os

import pytest
import requests
from playwright.sync_api import Page, expect

from conftest import API_KEY

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")


def headers():
    return {"X-API-Key": API_KEY} if API_KEY else {}


def set_auth(web: bool, api: bool):
    return requests.post(
        f"{BASE_URL}/api/auth/config",
        json={"web_auth_enabled": web, "api_auth_enabled": api},
        headers=headers(), timeout=5, verify=False,
    )


def open_security(page: Page):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text="Security").click()
    page.locator('form[data-form="auth-config"]').wait_for()
    page.locator('form[data-form="tls"]').wait_for()


@pytest.fixture(autouse=True)
def restore_auth():
    yield
    if API_KEY:
        set_auth(False, False)


class TestTlsUI:
    def test_tls_card_and_pem_fields(self, dashboard_page: Page):
        open_security(dashboard_page)
        form = dashboard_page.locator('form[data-form="tls"]')
        expect(form).to_be_visible()
        assert form.locator("textarea").count() == 2

    def test_tls_button_reflects_auth_prerequisite(self, dashboard_page: Page):
        open_security(dashboard_page)
        button = dashboard_page.locator('form[data-form="tls"] button[type="submit"]')
        if dashboard_page.locator(".notice", has_text="Enable both authentication").count():
            expect(button).to_be_disabled()
        else:
            expect(button).to_be_enabled()


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_cert_upload_rejected_without_both_auth_methods():
    response = set_auth(False, True)
    if response.status_code == 403:
        pytest.skip("Auth cannot be changed while TLS is provisioned")
    response = requests.post(
        f"{BASE_URL}/api/tls/certificate",
        json={"cert": "fake-cert", "key": "fake-key"},
        headers=headers(), timeout=5, verify=False,
    )
    assert response.status_code == 403
