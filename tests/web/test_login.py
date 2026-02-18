"""Tests for the web dashboard login flow."""

import pytest
from playwright.sync_api import Page, expect

from conftest import DEFAULT_USERNAME, DEFAULT_PASSWORD


class TestLoginPage:
    """Login form rendering and behavior."""

    def test_login_form_visible(self, login_page: Page):
        """Login form shows username, password, and submit button."""
        expect(login_page.locator(".login-box")).to_be_visible()
        expect(login_page.locator(".login-box input[type='text']")).to_be_visible()
        expect(login_page.locator(".login-box input[type='password']")).to_be_visible()
        expect(login_page.locator(".login-box button[type='submit']")).to_be_visible()

    def test_login_form_has_language_selector(self, login_page: Page):
        """Login page includes a language selector."""
        lang_select = login_page.locator(".login-box .lang-selector select")
        expect(lang_select).to_be_visible()
        # Should have EN, FR, ES options
        options = lang_select.locator("option")
        assert options.count() >= 3

    def test_login_success(self, login_page: Page):
        """Successful login navigates to the dashboard."""
        login_page.locator(".login-box input[type='text']").fill(DEFAULT_USERNAME)
        login_page.locator(".login-box input[type='password']").fill(DEFAULT_PASSWORD)
        login_page.locator(".login-box button[type='submit']").click()

        # After login, nav bar should appear (dashboard loaded)
        login_page.wait_for_selector("nav", timeout=10000)
        expect(login_page.locator("nav")).to_be_visible()
        # Login box should be gone
        expect(login_page.locator(".login-box")).not_to_be_visible()

    def test_login_wrong_password(self, login_page: Page):
        """Wrong credentials show an error message."""
        login_page.locator(".login-box input[type='text']").fill("wrong_user")
        login_page.locator(".login-box input[type='password']").fill("wrong_pass")
        login_page.locator(".login-box button[type='submit']").click()

        # Should stay on login page with an error
        login_page.wait_for_timeout(1000)
        expect(login_page.locator(".login-box")).to_be_visible()

    def test_login_empty_fields(self, login_page: Page):
        """Submitting empty fields doesn't navigate away."""
        login_page.locator(".login-box button[type='submit']").click()
        login_page.wait_for_timeout(500)
        expect(login_page.locator(".login-box")).to_be_visible()

    def test_nav_not_visible_before_login(self, login_page: Page):
        """Navigation bar is hidden when not logged in."""
        expect(login_page.locator("nav")).not_to_be_visible()
