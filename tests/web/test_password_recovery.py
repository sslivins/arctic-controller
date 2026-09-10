"""Tests for the "Forgot password?" recovery flow on the login page.

Recovery is authorised by the six-digit one-time code shown on the
controller's own screen, so these tests read the code from the test-only
pairing endpoint rather than from the display. That endpoint opens exactly
the same window the Security screen opens, so the flow being exercised is
the real one.
"""

import os

import pytest
import requests
from conftest import API_KEY, WEB_PASSWORD, WEB_USERNAME
from playwright.sync_api import Page, expect

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
RECOVERED_PASSWORD = "R3covered!Pass"


def open_pairing_window() -> str:
    """Open a one-time-code window and return the code."""
    response = requests.post(
        f"{BASE_URL}/api/test/ha-pairing-window", timeout=5, verify=False
    )
    assert response.status_code == 200, (
        f"could not open a pairing window: {response.status_code}"
    )
    code = response.json()["code"]
    assert len(code) == 6, f"unexpected code {code!r}"
    return code


def restore_password():
    """Put the original password back, whichever one is currently live."""
    for password in (RECOVERED_PASSWORD, WEB_PASSWORD):
        session = requests.Session()
        session.verify = False
        try:
            login = session.post(
                f"{BASE_URL}/login",
                json={"username": WEB_USERNAME, "password": password},
                timeout=5,
            )
            if login.status_code != 200:
                continue
            restored = session.post(
                f"{BASE_URL}/api/auth/credentials",
                json={"username": WEB_USERNAME, "password": WEB_PASSWORD},
                timeout=5,
            )
            if restored.status_code == 200:
                return
        except (
            requests.exceptions.ConnectionError,
            requests.exceptions.Timeout,
        ):
            continue

    raise AssertionError("Could not restore the original web password")


@pytest.fixture(autouse=True)
def cleanup():
    yield
    restore_password()


@pytest.fixture
def mobile_page(playwright, browser, base_url):
    """A page in a touch-emulating phone context.

    Uses a real device descriptor rather than just a small viewport so that
    `(pointer: coarse)` matches and `tap()` is available -- a narrow desktop
    window would silently keep the fine-pointer styling and prove nothing
    about the phone experience.
    """
    from conftest import _enable_web_auth, _ensure_auth_disabled

    _enable_web_auth(base_url)
    context = browser.new_context(
        **playwright.devices["iPhone 13"], ignore_https_errors=True
    )
    page = context.new_page()
    page.goto(base_url, wait_until="domcontentloaded")
    page.wait_for_selector(".login-card", timeout=10000)
    yield page
    context.close()
    _ensure_auth_disabled(base_url)


class TestPasswordRecovery:
    def test_forgot_password_opens_the_recovery_form(self, login_page: Page):
        login_page.locator('[data-action="forgot-password"]').click()
        form = login_page.locator('form[data-form="change-credentials"]')
        expect(form).to_be_visible()
        expect(form.locator('input[name="pairing_code"]')).to_be_visible()

    def test_code_help_is_collapsed_until_the_info_icon_is_tapped(
        self, login_page: Page
    ):
        """The instructions live behind (i) so the form stays short on a phone."""
        login_page.locator('[data-action="forgot-password"]').click()
        info = login_page.locator('[data-action="toggle-code-help"]')
        panel = login_page.locator("#code-help")

        expect(info).to_be_visible()
        expect(panel).to_be_hidden()
        expect(info).to_have_attribute("aria-expanded", "false")

        info.click()
        expect(panel).to_be_visible()
        expect(info).to_have_attribute("aria-expanded", "true")
        expect(panel).to_contain_text("Show One-Time Code")

        info.click()
        expect(panel).to_be_hidden()
        expect(info).to_have_attribute("aria-expanded", "false")

    def test_code_help_does_not_discard_what_was_already_typed(
        self, login_page: Page
    ):
        """Toggling help must not go through render(), which rebuilds the form."""
        login_page.locator('[data-action="forgot-password"]').click()
        form = login_page.locator('form[data-form="change-credentials"]')
        form.locator('input[name="password"]').fill("part-way-through")

        login_page.locator('[data-action="toggle-code-help"]').click()
        expect(login_page.locator("#code-help")).to_be_visible()
        expect(form.locator('input[name="password"]')).to_have_value(
            "part-way-through"
        )

    def test_code_help_is_tappable_on_a_phone_viewport(self, mobile_page: Page):
        """A phone has no hover, so the control must work by tap alone.

        This runs in a real touch-emulating context rather than just a narrow
        window, so the `(pointer: coarse)` rule that enlarges the icon to a
        finger-sized target is actually exercised.
        """
        mobile_page.locator('[data-action="forgot-password"]').click()

        info = mobile_page.locator('[data-action="toggle-code-help"]')
        box = info.bounding_box()
        assert box is not None, "info icon is not laid out"
        assert box["width"] >= 24 and box["height"] >= 24, (
            f"touch target is only {box['width']}x{box['height']}"
        )

        info.tap()
        expect(mobile_page.locator("#code-help")).to_be_visible()

        # The panel must not force the page sideways on a narrow screen.
        overflow = mobile_page.evaluate(
            "() => document.documentElement.scrollWidth"
            " - document.documentElement.clientWidth"
        )
        assert overflow <= 1, f"page overflows horizontally by {overflow}px"

    def test_cancel_returns_to_sign_in(self, login_page: Page):
        login_page.locator('[data-action="forgot-password"]').click()
        expect(
            login_page.locator('form[data-form="change-credentials"]')
        ).to_be_visible()
        login_page.locator('[data-action="cancel-recovery"]').click()
        expect(login_page.locator('form[data-form="login"]')).to_be_visible()

    def test_recovery_without_a_valid_code_is_rejected(self, login_page: Page):
        """The whole security of this flow is the code, so prove it is checked."""
        login_page.locator('[data-action="forgot-password"]').click()
        form = login_page.locator('form[data-form="change-credentials"]')
        form.locator('input[name="pairing_code"]').fill("000000")
        form.locator('input[name="username"]').fill(WEB_USERNAME)
        form.locator('input[name="password"]').fill(RECOVERED_PASSWORD)
        form.locator('input[name="confirm_password"]').fill(RECOVERED_PASSWORD)
        form.locator('button[type="submit"]').click()

        # Still on the recovery form, with the failure explained.
        expect(form).to_be_visible()
        expect(login_page.locator(".notice.bad")).to_be_visible(timeout=10000)

        # And the original password still works.
        verify = requests.post(
            f"{BASE_URL}/login",
            json={"username": WEB_USERNAME, "password": WEB_PASSWORD},
            timeout=5,
            verify=False,
        )
        assert verify.status_code == 200

    @pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
    def test_recovery_with_a_valid_code_sets_a_new_password(
        self, page: Page, base_url: str
    ):
        code = open_pairing_window()

        page.goto(base_url, wait_until="domcontentloaded")
        page.wait_for_selector(".login-card")
        page.locator('[data-action="forgot-password"]').click()

        form = page.locator('form[data-form="change-credentials"]')
        form.locator('input[name="pairing_code"]').fill(code)
        form.locator('input[name="username"]').fill(WEB_USERNAME)
        form.locator('input[name="password"]').fill(RECOVERED_PASSWORD)
        form.locator('input[name="confirm_password"]').fill(RECOVERED_PASSWORD)
        form.locator('button[type="submit"]').click()

        # Success drops back to the sign-in form; the new password works there.
        expect(page.locator('form[data-form="login"]')).to_be_visible(
            timeout=10000
        )
        page.locator('input[name="username"]').fill(WEB_USERNAME)
        page.locator('input[name="password"]').fill(RECOVERED_PASSWORD)
        page.locator('button[type="submit"]').click()
        expect(page.locator(".rail")).to_be_visible(timeout=10000)
