#!/usr/bin/env python3
"""Copy real history from one controller onto a test-firmware controller.

A dev unit in demo mode has unnatural history, which makes poor screenshots.
This reads a production controller's telemetry, events, daily energy and run
summaries over its normal API and writes them to a test-firmware unit through
POST /api/test/seed-history (CONFIG_TEST_ENDPOINTS builds only). Each kind
replaces what the target had. Timestamps are shifted so the newest source
sample lands on the target's "now" (whole days for the daily energy), so copy
from a unit in the same time zone. "days" also replaces the target's open
day with the source's totals so far today.

Usage:
    python tools/copy_history.py --source http://192.168.10.19 --source-key KEY \\
        --target http://192.168.1.134 --target-key KEY [--hours 24] [--days 31] \\
        [--kinds telemetry,events,days,runs]

Afterwards restart the target if it should report the copied lifetime energy
totals; the screens and API read the stored history directly.
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import struct
import sys
import time
from pathlib import Path

import requests

WINDOW = 8 * 3600
INTERVAL = 30

# history_telemetry_sample_t, energy::DayRecord, energy::RunRecord (see
# history_storage.h, energy_ledger.h, run_ledger.h) and the seed-history
# event record.
TELEMETRY = struct.Struct("<IIhhhBBhHHHhhB3x")
DAY = struct.Struct("<IhBBHH4II3I3IiHHIII4I3I4h")
RUN = struct.Struct("<7IhBB")
EVENT = struct.Struct("<4I")
assert (TELEMETRY.size, DAY.size, RUN.size, EVENT.size) == (32, 112, 32, 16)

CONNECTED, COMP_VALID, COMP_RUNNING, IN_VALID, OUT_VALID, SET_VALID, TANK_VALID, POWER_VALID = (
    1 << i for i in range(8))
HEAT_VALID, COP_VALID, COIL_VALID, AIR_VALID = (1 << i for i in range(4))
MODES = ("heating", "cooling", "hot_water")
DAY_MIXED, DAY_SETTINGS, DAY_OUTDOOR, DAY_RUN_OUTDOOR = (1 << i for i in range(4))


def event_types() -> dict[str, int]:
    src = (Path(__file__).resolve().parent.parent / "main" / "event_log.cpp").read_text()
    body = re.search(r"s_event_names\[\]\s*=\s*\{(.*?)\};", src, re.DOTALL)
    if not body:
        sys.exit("can't find s_event_names in main/event_log.cpp")
    return {name: i for i, name in enumerate(re.findall(r'"([a-z0-9_]+)"', body.group(1)))}


class Unit:
    def __init__(self, url: str, key: str):
        self.url = url.rstrip("/")
        self.s = requests.Session()
        self.s.headers["X-API-Key"] = key
        self.s.verify = False

    def get(self, path: str, **params):
        r = self.s.get(self.url + path, params=params, timeout=60)
        r.raise_for_status()
        return r.json()

    def seed(self, kind: str, body: bytes) -> None:
        if not body:
            print(f"  {kind}: nothing to copy")
            return
        r = self.s.post(self.url + "/api/test/seed-history", params={"kind": kind}, data=body,
                        headers={"Content-Type": "application/octet-stream"}, timeout=180)
        if not r.ok:
            sys.exit(f"seed {kind}: HTTP {r.status_code} {r.text}")
        print(f"  {kind}: {r.json()['count']} records")


def deci(v) -> int:
    return 0 if v is None else int(v)


def copy_telemetry(src: Unit, hours: int, shift: int, latest: int) -> bytes:
    samples: dict[int, dict] = {}
    end = latest
    while end > latest - hours * 3600:
        w = src.get("/api/heatpump/temperature-history", end=end)
        for s in w["samples"]:
            if s["t"] > latest - hours * 3600:
                samples[s["t"]] = s
        end = w["start"]
    out = bytearray()
    for t in sorted(samples):
        s = samples[t]
        flags = (CONNECTED if s["conn"] else 0) | COMP_VALID | (COMP_RUNNING if s["run"] else 0)
        flags |= IN_VALID if s["in"] is not None else 0
        flags |= OUT_VALID if s["out"] is not None else 0
        flags |= SET_VALID if s["set"] is not None else 0
        flags |= TANK_VALID if s["tank"] is not None else 0
        flags |= POWER_VALID if s["p"] is not None else 0
        flags2 = (HEAT_VALID if s["q"] is not None else 0) | (COP_VALID if s["cop"] is not None else 0)
        flags2 |= COIL_VALID if s.get("coil") is not None else 0
        flags2 |= AIR_VALID if s.get("air") is not None else 0
        cop = 0 if s["cop"] is None else round(float(s["cop"]) * 100)
        out += TELEMETRY.pack(t + shift, 0, deci(s["in"]), deci(s["out"]), deci(s["set"]),
                              s["mode"], flags, deci(s["tank"]), deci(s["p"]), deci(s["q"]), cop,
                              deci(s.get("coil")), deci(s.get("air")), flags2)
    return bytes(out)


def copy_events(src: Unit, since: int, shift: int) -> bytes:
    types = event_types()
    events, offset = [], 0
    while True:
        page = src.get("/api/events", limit=50, offset=offset)
        events += page["events"]
        offset += page["count"]
        if page["count"] == 0 or offset >= page["total"]:
            break
    out = bytearray()
    for e in reversed(events):  # newest first on the wire
        if e["timestamp"] < since or e["type"] not in types:
            continue
        out += EVENT.pack(e["timestamp"] + shift, e["boot_id"], types[e["type"]], e["payload"])
    return bytes(out)


def ymd(d: dt.date) -> int:
    return d.year * 10000 + d.month * 100 + d.day


def tenths(v) -> int:
    return 0 if v is None else round(v * 10)


def pack_day(d: dict, date: dt.date) -> bytes:
    e, o, st = d["electrical_wh"], d.get("outdoor_c"), d.get("settings")
    flags = 0
    if o:
        flags |= DAY_OUTDOOR | (DAY_RUN_OUTDOOR if o.get("run_avg") is not None else 0)
    if st:
        flags |= DAY_SETTINGS | (DAY_MIXED if st.get("mixed") else 0)
    src_s = d["source_seconds"]
    return DAY.pack(
        ymd(date), d["utc_offset_min"], flags, st["glycol_pct"] if st else 0,
        round(st["flow_lpm"] * 10) if st else 0, d["starts"],
        e["heating"], e["cooling"], e["hot_water"], e["standby"], e["unattributed"],
        *(d["thermal_wh"][m] for m in MODES), *(d["covered_electrical_wh"][m] for m in MODES),
        d["defrost"]["thermal_wh"], d["defrost"]["count"], 0, d["defrost"]["seconds"],
        d["coverage_seconds"]["power"], d["coverage_seconds"]["thermal"],
        src_s["unit"], src_s["external"], src_s["fallback"], src_s["settling"],
        *(d["run_seconds"][m] for m in MODES),
        tenths(o and o["min"]), tenths(o and o["max"]), tenths(o and o["avg"]),
        tenths(o and o.get("run_avg")))


def copy_days(src: Unit, days: int, day_shift: int) -> tuple[bytes, bytes]:
    """Closed days, and the open day (today's totals so far)."""
    today = dt.datetime.now().astimezone().date()
    reply = src.get("/api/energy/daily", **{"from": (today - dt.timedelta(days=days)).isoformat(),
                                            "to": today.isoformat()})
    closed, open_day = bytearray(), b""
    for d in reply["days"]:
        date = dt.date.fromisoformat(d["date"]) + dt.timedelta(days=day_shift)
        if not d["final"]:
            if date == today:
                open_day = pack_day(d, date)
        elif date < today:
            closed += pack_day(d, date)
    return bytes(closed), open_day


def copy_runs(src: Unit, days: int, shift: int, latest: int) -> bytes:
    runs: dict[int, dict] = {}
    to = latest + 1
    stop = latest - days * 86400
    while to > stop:
        frm = max(stop, to - 3 * 86400)
        for r in src.get("/api/energy/runs", **{"from": frm, "to": to})["runs"]:
            if not r["open"]:
                runs[r["start"]] = r
        to = frm
    out = bytearray()
    for start in sorted(runs):
        r = runs[start]
        outdoor = r["outdoor_c"]
        out += RUN.pack(0, r["start"] + shift, r["end"] + shift, r["electrical_wh"],
                        r["thermal_wh"], r["covered_electrical_wh"],
                        r["outdoor_seconds"] if outdoor is not None else 0,
                        0 if outdoor is None else round(outdoor * 10),
                        MODES.index(r["mode"]) if r["mode"] in MODES else 0, 0)
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source", required=True)
    ap.add_argument("--source-key", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--target-key", required=True)
    ap.add_argument("--hours", type=int, default=24, help="telemetry to copy")
    ap.add_argument("--days", type=int, default=31, help="events, daily energy and runs to copy")
    ap.add_argument("--kinds", default="telemetry,events,days,runs")
    args = ap.parse_args()
    requests.packages.urllib3.disable_warnings()

    src, dst = Unit(args.source, args.source_key), Unit(args.target, args.target_key)
    latest = src.get("/api/heatpump/temperature-history")["latest_end"]
    shift = round((time.time() - latest) / INTERVAL) * INTERVAL
    day_shift = round(shift / 86400)
    print(f"source newest sample {dt.datetime.fromtimestamp(latest, dt.timezone.utc).astimezone()}; shifting {shift} s")

    kinds = args.kinds.split(",")
    since = latest - args.days * 86400
    if "telemetry" in kinds:
        dst.seed("telemetry", copy_telemetry(src, args.hours, shift, latest))
    if "events" in kinds:
        dst.seed("events", copy_events(src, since, shift))
    if "days" in kinds:
        closed, open_day = copy_days(src, args.days, day_shift)
        dst.seed("days", closed)
        dst.seed("today", open_day)
    if "runs" in kinds:
        dst.seed("runs", copy_runs(src, args.days, shift, latest))


if __name__ == "__main__":
    main()
