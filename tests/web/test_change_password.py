"""Tests for credential management in the Security workspace."""

import os
import time

import pytest
import requests
from playwright.sync_api import Page, expect

from conftest import API_KEY, WEB_PASSWORD, WEB_USERNAME

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
NEW_PASSWORD = "N3wS3cure!Pass"


def restore():
    for password in (NEW_PASSWORD, WEB_PASSWORD):
        session = requests.Session()
        session.verify = False
        session.post(f"{BASE_URL}/login", json={"username": WEB_USERNAME, "password": password}, timeout=5)
        response = session.post(
            f"{BASE_URL}/api/auth/credentials",
            json={"username": WEB_USERNAME, "password": WEB_PASSWORD},
            timeout=5,
        )
        if response.status_code == 200:
            break
def browser_login(page: Page, password: str):
    page.locator('input[name="username"]').fill(WEB_USERNAME)
    page.locator('input[name="password"]').fill(password)
    page.locator('button[type="submit"]').click()


def open_security(page: Page):
    page.locator('button[aria-label="Settings"]').click()
    page.locator(".settings-nav .nav-link", has_text="Security").click()


@pytest.fixture(autouse=True)
def cleanup():
    yield
    restore()


class TestCredentials:
    def test_credentials_form_is_available(self, dashboard_page: Page):
        open_security(dashboard_page)
        expect(dashboard_page.locator('form[data-form="credentials"]')).to_be_visible()

    @pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
    def test_change_password_and_login(self, page: Page, base_url: str):
        page.goto(base_url, wait_until="domcontentloaded")
        page.wait_for_selector(".login-card")
        browser_login(page, WEB_PASSWORD)
        page.wait_for_selector(".rail", timeout=10000)

        open_security(page)
        form = page.locator('form[data-form="credentials"]')
        form.locator('input[name="password"]').fill(NEW_PASSWORD)
        form.locator('button[type="submit"]').click()
        page.wait_for_timeout(1000)

        page.context.clear_cookies()
        page.goto(base_url, wait_until="domcontentloaded")
        page.wait_for_selector(".login-card")
        browser_login(page, NEW_PASSWORD)
        expect(page.locator(".rail")).to_be_visible(timeout=10000)
