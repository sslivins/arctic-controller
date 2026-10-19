"""
Test: the controller recovers on its own when the access point drops it.

Before #360 an AP kick (reason 8, ASSOC_LEAVE) left the controller offline
until the supervisor's 60 s grace and was not counted as a drop at all. The
kick test pins both fixes; the block test walks the long-outage path (wifi_lost
/ wifi_restored events, slow supervisor retries) without a reboot.
"""

import time

from conftest import events_since, status, wait_until

# Fast reconnect fires 2 s after an unplanned drop; association, DHCP and the
# HTTPS server coming back take a few more. The old 60 s grace cannot pass.
KICK_RECOVERY_S = 25.0
# Comfortably past WifiDropTracker::LONG_MS (60 s).
BLOCK_S = 75.0
# After a block lifts the supervisor retries every 30 s; allow two attempts.
UNBLOCK_RECOVERY_S = 90.0


def _connected_with(dev, pred):
    st = status(dev)
    wifi = st.get("wifi") or {}
    return st if wifi.get("state") == "connected" and pred(wifi.get("drops") or {}) else None


def test_ap_kick_recovers_fast_and_is_counted(device, unifi, device_mac):
    before = status(device)
    drops0 = before["wifi"]["drops"]

    t0 = time.monotonic()
    unifi.kick(device_mac)
    after = wait_until(
        lambda: _connected_with(device, lambda d: d.get("total", 0) > drops0["total"]),
        timeout=KICK_RECOVERY_S, desc="controller back on Wi-Fi with the kick counted",
    )
    elapsed = time.monotonic() - t0
    print(f"\nrecovered {elapsed:.1f}s after the kick")

    drops = after["wifi"]["drops"]
    assert drops["total"] == drops0["total"] + 1, drops
    assert drops["short"] == drops0["short"] + 1, drops
    assert drops["long"] == drops0["long"], drops
    assert drops["last_reason"] != 0, drops
    assert after["uptime_ms"] > before["uptime_ms"], "controller rebooted instead of reconnecting"


def test_back_to_back_kicks_both_recover_fast(device, unifi, device_mac):
    """A second kick right after reconnecting is a new outage with its own quick
    retry. The supervisor used to sleep through the reconnect and treat the
    second drop as the first one continuing, leaving it to the 60 s grace."""
    before = status(device)
    for i in range(2):
        drops0 = status(device)["wifi"]["drops"]["total"]
        t0 = time.monotonic()
        unifi.kick(device_mac)
        wait_until(
            lambda: _connected_with(device, lambda d: d.get("total", 0) > drops0),
            timeout=KICK_RECOVERY_S, desc=f"controller back on Wi-Fi after kick {i + 1}",
        )
        print(f"\nkick {i + 1}: recovered in {time.monotonic() - t0:.1f}s")
    after = status(device)
    assert after["wifi"]["drops"]["total"] == before["wifi"]["drops"]["total"] + 2, after["wifi"]
    assert after["uptime_ms"] > before["uptime_ms"], "controller rebooted instead of reconnecting"


def test_long_outage_recovers_without_reboot(device, unifi, device_mac):
    before = status(device)
    drops0 = before["wifi"]["drops"]
    boot_id = device.session.get(f"{device.base_url}/api/events", params={"limit": 1},
                                 timeout=device.timeout).json()["current_boot_id"]

    unifi.block(device_mac)
    blocked_at = time.monotonic()
    try:
        wait_until(lambda: _unreachable(device), timeout=20.0, desc="controller to drop off Wi-Fi")
        time.sleep(max(0.0, BLOCK_S - (time.monotonic() - blocked_at)))
    finally:
        unifi.unblock(device_mac)
    unblocked_at = time.monotonic()

    after = wait_until(
        lambda: _connected_with(device, lambda d: d.get("long", 0) > drops0["long"]),
        timeout=UNBLOCK_RECOVERY_S, poll=1.0, desc="controller back on Wi-Fi after the block",
    )
    print(f"\nrecovered {time.monotonic() - unblocked_at:.1f}s after the unblock "
          f"({time.monotonic() - blocked_at:.1f}s outage)")

    drops = after["wifi"]["drops"]
    assert drops["long"] == drops0["long"] + 1, drops
    assert drops["total"] == drops0["total"] + 1, drops
    assert after["uptime_ms"] > before["uptime_ms"], "controller rebooted instead of reconnecting"

    evs = wait_until(
        lambda: (e := events_since(device, boot_id, before["uptime_ms"]))
        and any(x["type"] == "wifi_restored" for x in e) and e,
        # Drop events are written by the supervisor's 15 s poll, not on reconnect.
        timeout=40.0, desc="wifi_restored event",
    )
    types = [e["type"] for e in evs]
    assert "wifi_lost" in types, types
    assert types.index("wifi_lost") < types.index("wifi_restored"), types
    restored = next(e for e in evs if e["type"] == "wifi_restored")
    assert restored.get("down_s", 0) >= 60, restored


def _unreachable(dev) -> bool:
    try:
        status(dev, timeout=2.0)
        return False
    except Exception:
        return True
