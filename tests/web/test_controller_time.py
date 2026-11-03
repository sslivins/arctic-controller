"""Times on the dashboard are the controller's clock, wherever the browser is."""

import datetime as dt
import json
import re
import time

import pytest
from playwright.sync_api import Page, expect

# The browser sits in Los Angeles; each test chooses the controller's zone.
pytestmark = pytest.mark.browser_context_args(timezone_id="America/Los_Angeles")

JST = "JST-9"
PACIFIC = "PST8PDT,M3.2.0,M11.1.0"


def _clock_texts(t: dt.datetime) -> set:
    """The 24 h and 12 h renderings of a time with seconds."""
    return {f"{t.hour:02}:{t.minute:02}:{t.second:02}",
            f"{t.hour % 12 or 12}:{t.minute:02}:{t.second:02} {'AM' if t.hour < 12 else 'PM'}"}


def test_event_times_use_the_controller_clock(controller_zone):
    page = controller_zone(JST)
    ts = int(time.time()) - 60
    body = {"total": 1, "offset": 0, "count": 1, "current_boot_id": 1, "events": [
        {"type": "compressor_on", "category": "equipment", "timestamp": ts, "boot_id": 1,
         "uptime_ms": 1000, "payload": 0}]}
    page.route("**/api/events?*", lambda route: route.fulfill(
        status=200, content_type="application/json", body=json.dumps(body)))
    page.locator(".rail .nav-link", has_text="History").click()
    page.locator(".hist-seg-btn", has_text="Events").click()

    tokyo = dt.timezone(dt.timedelta(hours=9))
    at = dt.datetime.fromtimestamp(ts, tokyo)
    shown = page.locator("#event-list .event-time")
    expect(shown).to_have_count(1)
    assert shown.inner_text() in _clock_texts(at)
    today = dt.datetime.now(tokyo).date()
    expect(page.locator("#event-list .event-day")).to_have_text(
        "Today" if at.date() == today else "Yesterday")


def test_clock_popover_shows_both_times_when_zones_differ(controller_zone):
    page = controller_zone(JST)
    page.route("**/api/location", lambda route: route.fulfill(
        status=200, content_type="application/json",
        body=json.dumps({"valid": True, "name": "Testville", "latitude": 35.7, "longitude": 139.7,
                         "iana_tz": "Asia/Tokyo", "tz_auto": False, "derived_posix": JST})))
    button = page.locator(".clock-btn")
    expect(button).to_have_class(re.compile(r"\bdiffers\b"))
    button.click()
    panel = page.locator("[data-clock-panel]")
    expect(panel).to_contain_text("Controller time")
    expect(panel).to_contain_text("Testville · JST, UTC+9")
    expect(panel).to_contain_text("Your time")
    expect(panel).to_contain_text("Los Angeles")
    expect(panel).to_contain_text("use controller time")

    page.get_by_role("heading", name="Home", exact=True).click()
    expect(panel).to_have_count(0)


@pytest.mark.parametrize("posix, zone", [
    ("<PCT>7", "PCT, UTC−7"),   # BC's official name since going permanent
    ("<-07>7", "UTC−7"),        # no settled name (Yukon): just the offset
])
def test_clock_popover_names_quoted_zones(controller_zone, posix, zone):
    page = controller_zone(posix)
    page.locator(".clock-btn").click()
    sub = page.locator("[data-clock-panel] .cp-sub").first
    expect(sub).to_contain_text(zone)
    expect(sub).not_to_contain_text("-07,")


def test_clock_in_the_same_zone_is_plain(controller_zone):
    page = controller_zone(PACIFIC)
    button = page.locator(".clock-btn")
    expect(button).not_to_have_class(re.compile(r"\bdiffers\b"))
    button.click()
    panel = page.locator("[data-clock-panel]")
    expect(panel).to_contain_text("Controller time")
    expect(panel).to_contain_text("internet time")
    expect(panel).not_to_contain_text("Your time")


def _box(page: Page, selector: str) -> dict:
    """An element's box, measured in one go: the status poll re-renders the
    topbar, so a locator can lose its element between two calls."""
    return page.wait_for_function(
        """s => { const e = document.querySelector(s); if (!e) return null;
                  const r = e.getBoundingClientRect();
                  return r.width ? {x: r.x, width: r.width, height: r.height} : null; }""",
        arg=selector).json_value()


def test_phone_clock_sits_by_the_logo_and_opens_full_width(controller_zone):
    page = controller_zone(JST)
    page.set_viewport_size({"width": 390, "height": 844})
    short = page.locator(".clock-btn .clk-short")
    expect(short).to_be_visible()
    expect(page.locator(".clock-btn .clk-long")).to_be_hidden()
    assert not re.search(r"\d:\d\d:\d\d", short.inner_text()), "no seconds on a phone"

    button = _box(page, ".clock-btn")
    assert button["height"] >= 44, button
    assert button["x"] < _box(page, ".hp-status")["x"], "clock sits left of the status icons"

    page.locator(".clock-btn").click()
    panel = page.locator("[data-clock-panel]")
    expect(panel).to_be_visible()
    box = _box(page, "[data-clock-panel]")
    assert box["x"] >= 0 and box["x"] + box["width"] <= 390, box
    assert box["width"] > 300, box
