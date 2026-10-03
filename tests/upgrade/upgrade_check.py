#!/usr/bin/env python3
"""Upgrade migration check: does the new firmware still read what the old one wrote?

A field device updates from the current release with its settings, event log
and temperature history in place. Nothing else in CI proves the new firmware
reads that data correctly: the device suites run against state the test
build wrote itself. This script closes that gap using only the production API,
so the release being upgraded from is the oracle for what its own data means.

    seed      write known, non-default settings and some events (old release)
    snapshot  record everything the old release reports about them
    compare   after the OTA, check the new release reports the same thing
    restore   put the settings back to the values seen before seeding

Rules for compare:
  * Every value the old release reported must be reported unchanged. New keys
    and new events are fine; missing or changed ones are not.
  * An endpoint the old release did not have (404) is skipped. An endpoint it
    did have must still answer.
  * Intentional format changes are listed, with a reason, in
    migration_exceptions.json next to this script.

Env: ARCTIC_URL (http(s)://host), ARCTIC_API_KEY.
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

import requests
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

EXCEPTIONS_PATH = Path(__file__).resolve().parent / "migration_exceptions.json"

# Settings snapshotted and compared. The keys dropped are live state rather
# than stored data, so they legitimately differ across a reboot.
SETTINGS = {
    "/api/preferences": set(),
    "/api/time/config": {"synced"},
    "/api/display/brightness": set(),
    "/api/performance/config": {"status", "limits"},
    "/api/location": set(),
    "/api/auth/config": {"authenticated"},
    "/api/tls/status": {"https_active"},
}
EVENTS = "/api/events"
HISTORY = "/api/heatpump/temperature-history"
HISTORY_MIN_SAMPLES = 3

# Non-default values written by seed. Each is read back on the old release.
SEED_PREFERENCES = {"device_name": "upgrade-check", "temp_unit": "fahrenheit", "language": "fr"}
SEED_TIMEZONE = "PST8PDT,M3.2.0,M11.1.0"
SEED_BRIGHTNESS = 37
SEED_LOCATION = {
    "latitude": 50.8833, "longitude": -119.8833, "name": "Sun Peaks",
    "iana_tz": "America/Vancouver", "tz_auto": False,
}
SEED_PERF = {
    "flow_lpm": 23.5,
    "fluid": "propylene_glycol",
    "glycol_pct": 30,
    "sensors": {
        "supply": {
            "source": "modbus_tcp", "host": "192.0.2.10", "port": 1502, "unit_id": 7,
            "register": 40, "register_type": "holding", "value_type": "int16", "scale": 0.1,
        },
        "return": {
            "source": "bacnet_ip", "host": "192.0.2.11", "object_type": "analog_input",
            "object_instance": 3, "device_instance": 1234,
            "device_name": "Upgrade Check", "object_name": "Return Temp",
        },
    },
}


class CheckError(Exception):
    pass


class Device:
    def __init__(self, url: str, key: str):
        self.url = url.rstrip("/")
        self.s = requests.Session()
        self.s.verify = False
        self.s.headers["X-API-Key"] = key

    def req(self, method: str, path: str, body=None, timeout: float = 15.0) -> requests.Response:
        return self.s.request(method, self.url + path, json=body, timeout=timeout)

    def get(self, path: str):
        """GET JSON, or None for a 404 (the endpoint is not in this release)."""
        r = self.req("GET", path)
        if r.status_code == 404:
            return None
        if r.status_code != 200:
            raise CheckError(f"GET {path} -> HTTP {r.status_code}: {r.text[:200]}")
        return r.json()

    def write(self, method: str, path: str, body=None) -> bool:
        """Write; False for a 404 (not in this release), raises on any other error."""
        r = self.req(method, path, body)
        if r.status_code == 404:
            return False
        if r.status_code != 200:
            raise CheckError(f"{method} {path} {json.dumps(body)} -> HTTP {r.status_code}: {r.text[:200]}")
        return True


def wait_until(fn, timeout: float, desc: str, poll: float = 2.0):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = fn()
            if last:
                return last
        except (requests.RequestException, ValueError, CheckError) as e:
            last = e
        time.sleep(poll)
    raise CheckError(f"timed out after {timeout:.0f}s waiting for {desc} (last: {last!r})")


def wait_ready(dev: Device, timeout: float = 180) -> dict:
    return wait_until(lambda: dev.get("/api/ota/status"), timeout, "the device to answer /api/ota/status")


def boot_id(dev: Device):
    return (dev.get(f"{EVENTS}?limit=1") or {}).get("current_boot_id")


def reboot(dev: Device):
    before = boot_id(dev)
    try:
        dev.req("POST", "/api/ota/reboot", timeout=10)
    except requests.RequestException:
        pass
    wait_until(lambda: boot_id(dev) not in (None, before), 180, "the device to come back from a reboot")


def read_events(dev: Device) -> list:
    events, offset = [], 0
    while True:
        page = dev.get(f"{EVENTS}?offset={offset}&limit=128")
        if page is None:
            raise CheckError(f"{EVENTS} is missing")
        events.extend(page["events"])
        offset += page["count"]
        if page["count"] == 0 or offset >= page["total"]:
            return events


def read_settings(dev: Device) -> dict:
    out = {}
    for path, drop in SETTINGS.items():
        data = dev.get(path)
        out[path] = None if data is None else {k: v for k, v in data.items() if k not in drop}
    return out


def _matches(want, got) -> bool:
    if isinstance(want, dict):
        return isinstance(got, dict) and all(_matches(v, got.get(k)) for k, v in want.items())
    if isinstance(want, float) or isinstance(got, float):
        return isinstance(got, (int, float)) and not isinstance(got, bool) and abs(float(want) - float(got)) < 1e-6
    return want == got


def _seed_diff(want, got, path=""):
    """Compare a seed read-back. Keys the release does not report at all are
    fields it predates: return them as unsupported rather than as failures."""
    bad, unsupported = {}, []
    for k, v in want.items():
        p = f"{path}.{k}" if path else k
        if not isinstance(got, dict) or k not in got:
            unsupported.append(p)
        elif isinstance(v, dict) and isinstance(got[k], dict):
            b, u = _seed_diff(v, got[k], p)
            bad.update(b)
            unsupported += u
        elif not _matches(v, got[k]):
            bad[p] = (v, got[k])
    return bad, unsupported

# ---------------------------------------------------------------------------
# seed / restore
# ---------------------------------------------------------------------------

def cmd_seed(dev: Device, args) -> int:
    wait_ready(dev)
    wait_until(lambda: (dev.get("/api/time") or {}).get("ntp_synced"), 180,
               "the clock to sync (events and history need real timestamps)")
    defaults = read_settings(dev)
    Path(args.defaults).write_text(json.dumps(defaults, indent=2))
    print(f"pre-seed settings saved to {args.defaults}")

    # Demo mode lets the heat pump controls (and so the events below) work
    # without a bus, and it is what the bench runs in. It needs a reboot.
    if not (dev.get("/api/preferences") or {}).get("demo_mode"):
        dev.write("PATCH", "/api/preferences", {"demo_mode": True})
        reboot(dev)
        print("demo mode on")

    time_body = {"timezone": SEED_TIMEZONE,
                 "format_24h": not (defaults["/api/time/config"] or {}).get("format_24h", False)}
    steps = [
        # (method, path, body, path to read back, values it must report)
        # GET reports the language as a display name; the code is language_code.
        ("PATCH", "/api/preferences", SEED_PREFERENCES, "/api/preferences",
         {**{k: v for k, v in SEED_PREFERENCES.items() if k != "language"},
          "language_code": SEED_PREFERENCES["language"]}),
        ("POST", "/api/location", SEED_LOCATION, "/api/location", SEED_LOCATION),
        ("POST", "/api/time/config", time_body, "/api/time/config", time_body),
        ("PUT", "/api/display/brightness", {"brightness": SEED_BRIGHTNESS},
         "/api/display/brightness", {"brightness": SEED_BRIGHTNESS}),
        ("PUT", "/api/performance/config", SEED_PERF, "/api/performance/config", SEED_PERF),
    ]
    skipped = []
    for method, path, body, check_path, expected in steps:
        if not dev.write(method, path, body):
            skipped.append(path)
            continue
        got = dev.get(check_path) or {}
        bad, unsupported = _seed_diff(expected, got)
        if bad:
            raise CheckError(f"{check_path} did not keep the seeded values (wanted, got): {bad}")
        print(f"seeded {path}" + (f" (not reported by this release: {', '.join(unsupported)})"
                                  if unsupported else ""))

    # Start from an empty log so the comparison is about data written here.
    dev.write("DELETE", EVENTS)
    mode = (dev.get("/api/heatpump/status") or {}).get("mode")
    writes = [
        ("/api/heatpump/mode", {"mode": "heating" if mode == "cooling" else "cooling"}),
        ("/api/heatpump/setpoints", {"heating": 41, "hot_water": 49}),
        ("/api/heatpump/power", {"on": False}),
        ("/api/heatpump/power", {"on": True}),
    ]
    for path, body in writes:
        r = dev.req("PUT", path, body)
        if r.status_code != 200 or not r.json().get("success"):
            raise CheckError(f"PUT {path} {body} -> HTTP {r.status_code}: {r.text[:200]}")
    events = wait_until(lambda: (lambda e: e if len(e) >= len(writes) else None)(read_events(dev)),
                        30, f"at least {len(writes)} events to be logged")
    print(f"event log holds {len(events)} event(s): {[e['type'] for e in events]}")

    def history():
        h = dev.get(HISTORY)
        return h if h and len(h.get("samples", [])) >= HISTORY_MIN_SAMPLES else None
    wait_until(history, 180, f"{HISTORY_MIN_SAMPLES} temperature history samples", poll=5)
    print("temperature history has samples")

    if skipped:
        print(f"not in this release, skipped: {', '.join(skipped)}")
    return 0


def cmd_restore(dev: Device, args) -> int:
    wait_ready(dev)
    d = json.loads(Path(args.defaults).read_text())
    p = d.get("/api/preferences")
    if p:
        body = {k: p[k] for k in ("device_name", "temp_unit") if k in p}
        if "language_code" in p:
            body["language"] = p["language_code"]
        dev.write("PATCH", "/api/preferences", body)
    loc = d.get("/api/location")
    if loc:
        # A location cannot be unset, but the timezone mode can go back.
        dev.write("POST", "/api/location", {"tz_auto": loc.get("tz_auto", True)})
    t = d.get("/api/time/config")
    if t:
        dev.write("POST", "/api/time/config", {"timezone": t["timezone"], "format_24h": t["format_24h"]})
    b = d.get("/api/display/brightness")
    if b:
        dev.write("PUT", "/api/display/brightness", b)
    perf = d.get("/api/performance/config")
    if perf:
        dev.write("PUT", "/api/performance/config",
                  {k: perf[k] for k in ("flow_lpm", "fluid", "glycol_pct", "sensors") if k in perf})
    print("settings restored to their pre-seed values")
    return 0


# ---------------------------------------------------------------------------
# snapshot / compare
# ---------------------------------------------------------------------------

def take_snapshot(dev: Device, end=None) -> dict:
    ota = wait_ready(dev)
    return {
        "firmware": {k: ota.get(k) for k in ("current_version", "build_sha")},
        "settings": read_settings(dev),
        "events": read_events(dev),
        "history": dev.get(HISTORY + (f"?end={end}" if end else "")),
    }


def cmd_snapshot(dev: Device, args) -> int:
    snap = take_snapshot(dev)
    Path(args.out).write_text(json.dumps(snap, indent=2))
    print(f"snapshot of {snap['firmware']}: {len(snap['events'])} events, "
          f"{len((snap['history'] or {}).get('samples', []))} history samples -> {args.out}")
    return 0


def load_exceptions(path: Path = EXCEPTIONS_PATH) -> list:
    if not path.exists():
        return []
    rules = json.loads(path.read_text()).get("ignore", [])
    for r in rules:
        if not r.get("path") or not r.get("reason"):
            raise CheckError(f"{path.name}: every entry needs a path and a reason: {r!r}")
    return [r["path"] for r in rules]


def _ignored(where: str, ignored: list) -> bool:
    return any(where == p or where.startswith((p + ".", p + "[")) for p in ignored)


def diff(old, new, where: str, ignored: list, out: list):
    """Every value in old must be present and equal in new. Extra keys are fine."""
    if _ignored(where, ignored):
        return
    if isinstance(old, dict):
        if not isinstance(new, dict):
            out.append(f"{where}: was an object, now {new!r}")
            return
        for k, v in old.items():
            if k not in new:
                if not _ignored(f"{where}.{k}", ignored):
                    out.append(f"{where}.{k}: missing (was {v!r})")
            else:
                diff(v, new[k], f"{where}.{k}", ignored, out)
    elif isinstance(old, list):
        if not isinstance(new, list) or len(new) != len(old):
            out.append(f"{where}: {old!r} -> {new!r}")
            return
        for i, (a, b) in enumerate(zip(old, new)):
            diff(a, b, f"{where}[{i}]", ignored, out)
    elif not _matches(old, new):
        out.append(f"{where}: {old!r} -> {new!r}")


def compare(old: dict, new: dict, ignored: list) -> list:
    problems = []
    for path, before in old["settings"].items():
        if before is None:
            continue  # not in the old release
        after = new["settings"].get(path)
        if after is None:
            problems.append(f"{path}: answered on the old release, missing on the new one")
        else:
            diff(before, after, path, ignored, problems)

    # Every old event must still be there, unchanged and in the same order.
    # New ones (booting the new firmware, say) may be interleaved.
    new_events = iter(new["events"])
    for i, ev in enumerate(old["events"]):
        if not any(_event_same(ev, cand, ignored) for cand in new_events):
            first = []
            diff(ev, new["events"][i] if i < len(new["events"]) else None, EVENTS, ignored, first)
            problems.append(f"{EVENTS}: event #{i} written by the old release is missing or changed "
                            f"({ev.get('type')}); e.g. {first[:3]}")
            break

    old_h, new_h = old.get("history"), new.get("history")
    if old_h is not None:
        if new_h is None:
            problems.append(f"{HISTORY}: answered on the old release, missing on the new one")
        else:
            by_t = {s["t"]: s for s in new_h.get("samples", [])}
            for s in old_h.get("samples", []):
                if s["t"] not in by_t:
                    problems.append(f"{HISTORY}: sample t={s['t']} written by the old release is missing")
                    break
                diff(s, by_t[s["t"]], f"{HISTORY}.samples", ignored, problems)
    return problems


def _event_same(old: dict, new: dict, ignored: list) -> bool:
    out = []
    diff(old, new, EVENTS, ignored, out)
    return not out


def cmd_compare(dev: Device, args) -> int:
    old = json.loads(Path(args.snapshot).read_text())
    new = take_snapshot(dev, end=(old.get("history") or {}).get("end"))
    if args.out:
        Path(args.out).write_text(json.dumps(new, indent=2))
    if new["firmware"] == old["firmware"]:
        raise CheckError(f"the device is still running {old['firmware']}; nothing to compare")
    problems = compare(old, new, load_exceptions())
    print(f"{old['firmware']} -> {new['firmware']}")
    print(f"compared {sum(v is not None for v in old['settings'].values())} settings endpoints, "
          f"{len(old['events'])} events, {len((old.get('history') or {}).get('samples', []))} history samples")
    if problems:
        print("\nData written by the old release does not read back the same on the new one:")
        for p in problems:
            print(f"  - {p}")
        print(f"\nIf a change is intentional, list it in tests/upgrade/{EXCEPTIONS_PATH.name} with a reason.")
        return 1
    print("OK: everything the old release stored reads back unchanged")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("seed").add_argument("--defaults", required=True)
    sub.add_parser("snapshot").add_argument("--out", required=True)
    p = sub.add_parser("compare")
    p.add_argument("snapshot")
    p.add_argument("--out")
    sub.add_parser("restore").add_argument("--defaults", required=True)
    args = ap.parse_args()

    url = os.environ.get("ARCTIC_URL", "").replace("http://", "https://", 1)
    key = os.environ.get("ARCTIC_API_KEY", "")
    if not url or not key:
        print("ARCTIC_URL and ARCTIC_API_KEY are required", file=sys.stderr)
        return 2
    dev = Device(url, key)
    commands = {"seed": cmd_seed, "snapshot": cmd_snapshot, "compare": cmd_compare, "restore": cmd_restore}
    try:
        return commands[args.cmd](dev, args)
    except CheckError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
