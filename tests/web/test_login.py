"""Tests for the web login flow."""

import re

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


def _route_login(page: Page, status: int, headers: dict | None = None):
    """Stub POST /login so failures don't trip the controller's real throttle."""
    body = '{"error":"Too many failed sign-in attempts. Try again shortly."}' \
        if status == 429 else '{"error":"Invalid credentials"}'
    page.route("**/login", lambda route: route.fulfill(
        status=status, content_type="application/json",
        headers=headers or {}, body=body))


def _submit(page: Page, username: str, password: str):
    page.locator('input[name="username"]').fill(username)
    page.locator('input[name="password"]').fill(password)
    with page.expect_response("**/login"):
        page.locator('button[type="submit"]').click()


class TestLoginFailureFeedback:
    def test_repeat_failure_changes_the_message(self, login_page: Page):
        _route_login(login_page, 401)
        _submit(login_page, "admin", "nope")
        message = login_page.locator(".login-message")
        expect(message).to_have_text("Incorrect username or password.")
        _submit(login_page, "admin", "nope")
        expect(message).to_have_text("Incorrect username or password (attempt 2).")
        expect(login_page.locator(".login-card")).to_have_class(re.compile(r"\bshake\b"))

    def test_failure_keeps_username_and_focuses_password(self, login_page: Page):
        _route_login(login_page, 401)
        _submit(login_page, "admin", "nope")
        expect(login_page.locator('input[name="username"]')).to_have_value("admin")
        password = login_page.locator('input[name="password"]')
        expect(password).to_have_value("")
        expect(password).to_be_focused()
        expect(password).to_have_attribute("aria-invalid", "true")

    def test_recovery_is_suggested_after_repeated_failures(self, login_page: Page):
        _route_login(login_page, 401)
        forgot = login_page.locator('[data-action="forgot-password"]')
        for _ in range(2):
            _submit(login_page, "admin", "nope")
        expect(forgot).not_to_have_class(re.compile(r"\battention\b"))
        _submit(login_page, "admin", "nope")
        expect(forgot).to_have_class(re.compile(r"\battention\b"))
        expect(login_page.locator(".login-hint")).to_be_visible()

    def test_throttled_sign_in_counts_down_and_reenables(self, login_page: Page):
        _route_login(login_page, 429, {"Retry-After": "2"})
        _submit(login_page, "admin", "nope")
        submit = login_page.locator('button[type="submit"]')
        expect(login_page.locator(".login-message")).to_contain_text("Try again in")
        expect(submit).to_be_disabled()
        expect(submit).to_be_enabled(timeout=5000)
        expect(login_page.locator(".login-message")).to_have_text("You can try signing in again.")
