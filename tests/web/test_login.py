"""Tests for the web login flow."""

from playwright.sync_api import Page, expect

from conftest import WEB_PASSWORD, WEB_USERNAME


class TestLoginPage:
    def test_login_form_visible(self, login_page: Page):
        expect(login_page.locator(".login-card")).to_be_visible()
        expect(login_page.locator('input[name="username"]')).to_be_visible()
        expect(login_page.locator('input[name="password"]')).to_be_visible()

    def test_login_success(self, login_page: Page):
        login_page.locator('input[name="username"]').fill(WEB_USERNAME)
        login_page.locator('input[name="password"]').fill(WEB_PASSWORD)
        login_page.locator('button[type="submit"]').click()
        expect(login_page.locator(".rail")).to_be_visible(timeout=10000)
        expect(login_page.locator(".login-card")).not_to_be_visible()

    def test_wrong_password_stays_on_login(self, login_page: Page):
        login_page.locator('input[name="username"]').fill("wrong")
        login_page.locator('input[name="password"]').fill("wrong")
        login_page.locator('button[type="submit"]').click()
        expect(login_page.locator(".login-card")).to_be_visible()

    def test_navigation_hidden_before_login(self, login_page: Page):
        expect(login_page.locator(".rail")).not_to_be_visible()
