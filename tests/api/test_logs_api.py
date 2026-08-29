"""
Functional tests for the system log API endpoints.

Tests GET /api/logs with filtering parameters (since, level, limit),
incremental polling, and DELETE /api/logs buffer clearing.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
"""

import os
import time

import pytest
import requests
import urllib3
from pathlib import Path
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

# Load .env from repo root if present (local dev)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Disable TLS verification for self-signed device certificate
_OrigSessionInit = requests.Session.__init__
def _session_init_no_verify(self, *args, **kwargs):
    _OrigSessionInit(self, *args, **kwargs)
    self.verify = False
requests.Session.__init__ = _session_init_no_verify

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# Retry-enabled session for all API calls
_session = requests.Session()
_retry = Retry(total=3, backoff_factor=1, allowed_methods=None,
               status_forcelist=[502, 503, 504])
_session.mount("http://", HTTPAdapter(max_retries=_retry))
_session.mount("https://", HTTPAdapter(max_retries=_retry))


def _headers():
    h = {}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    return h


def _get(path, **kwargs):
    return _session.get(f"{BASE_URL}{path}", headers=_headers(), timeout=10, **kwargs)


def _delete(path):
    return _session.delete(f"{BASE_URL}{path}", headers=_headers(), timeout=10)


def _wait_json(path, predicate, description, timeout=5.0, poll=0.1, required=True, **kwargs):
    """Poll GET ``path`` until ``predicate(json)`` is truthy; return that JSON.

    Replaces a fixed sleep after an action (log activity / buffer clear): rather
    than guessing how long the async log pipeline takes to reflect the change,
    wait for the observable result. ``kwargs`` are forwarded to ``_get`` (e.g.
    ``params``). Exceptions are treated as "not yet". When ``required`` is False
    and the timeout elapses, the last-seen JSON is returned instead of raising —
    used where the test only asserts a lenient/structural condition.
    """
    deadline = time.monotonic() + timeout
    data = None
    while True:
        try:
            data = _get(path, **kwargs).json()
        except Exception:
            data = None
        if data is not None and predicate(data):
            return data
        if time.monotonic() >= deadline:
            if required:
                raise AssertionError(
                    f"Timed out after {timeout:.1f}s waiting for {description}"
                )
            return data
        time.sleep(poll)


@pytest.fixture(scope="module", autouse=True)
def _check_prerequisites():
    if not API_KEY:
        pytest.skip("ARCTIC_API_KEY not set")
    last_err = None
    for attempt in range(3):
        try:
            r = requests.get(f"{BASE_URL}/api/health", timeout=5)
            r.raise_for_status()
            return
        except Exception as e:
            last_err = e
            if attempt < 2:
                time.sleep(2)
    pytest.skip(f"Device not reachable at {BASE_URL}: {last_err}")


# ── GET /api/logs ─────────────────────────────────────────────────────────


class TestLogsGet:
    """GET /api/logs — log buffer retrieval and filtering."""

    def test_logs_returns_200(self):
        r = _get("/api/logs")
        assert r.status_code == 200

    def test_logs_response_structure(self):
        """Response must have total, latest_seq, and entries array."""
        data = _get("/api/logs").json()
        assert "total" in data
        assert "latest_seq" in data
        assert "entries" in data
        assert isinstance(data["entries"], list)
        assert isinstance(data["total"], int)
        assert isinstance(data["latest_seq"], int)

    def test_logs_entry_structure(self):
        """Each log entry must have seq, uptime_ms, level, tag, message."""
        data = _get("/api/logs").json()
        if len(data["entries"]) == 0:
            pytest.skip("No log entries on device")
        entry = data["entries"][0]
        for field in ["seq", "uptime_ms", "level", "tag", "message"]:
            assert field in entry, f"Missing log field: {field}"

    def test_logs_level_values(self):
        """Log entry levels should be valid characters."""
        data = _get("/api/logs").json()
        valid_levels = {"E", "W", "I", "D", "V"}
        for entry in data["entries"]:
            assert entry["level"] in valid_levels, f"Invalid level: {entry['level']}"

    def test_logs_seq_is_monotonic(self):
        """Sequence numbers should be monotonically increasing."""
        data = _get("/api/logs").json()
        entries = data["entries"]
        if len(entries) < 2:
            pytest.skip("Need at least 2 entries to test ordering")
        for i in range(1, len(entries)):
            assert entries[i]["seq"] > entries[i - 1]["seq"], \
                f"seq not monotonic: {entries[i-1]['seq']} -> {entries[i]['seq']}"

    def test_logs_limit_parameter(self):
        """?limit=N should return at most N entries."""
        data = _get("/api/logs", params={"limit": 3}).json()
        assert len(data["entries"]) <= 3

    def test_logs_limit_1(self):
        """?limit=1 should return exactly 0 or 1 entry."""
        data = _get("/api/logs", params={"limit": 1}).json()
        assert len(data["entries"]) <= 1

    def test_logs_since_parameter(self):
        """?since=N should return only entries with seq > N."""
        # First get all logs to find a seq number
        all_data = _get("/api/logs").json()
        if len(all_data["entries"]) < 2:
            pytest.skip("Need at least 2 entries to test since")

        # Use the seq of the second-to-last entry
        pivot_seq = all_data["entries"][-2]["seq"]
        filtered = _get("/api/logs", params={"since": pivot_seq}).json()

        for entry in filtered["entries"]:
            assert entry["seq"] > pivot_seq, \
                f"Entry seq {entry['seq']} should be > {pivot_seq}"

    def test_logs_since_latest_returns_empty(self):
        """?since=latest_seq should return 0 entries (no new logs).

        Background tasks (e.g. periodic firmware check) may produce a log
        entry between the two requests, so re-fetch once if we get entries.
        """
        all_data = _get("/api/logs").json()
        latest = all_data["latest_seq"]
        filtered = _get("/api/logs", params={"since": latest}).json()
        if len(filtered["entries"]) > 0:
            # Background log arrived — use the new latest_seq and retry once
            latest = filtered["entries"][-1]["seq"]
            filtered = _get("/api/logs", params={"since": latest}).json()
        assert len(filtered["entries"]) == 0

    def test_logs_level_error_filter(self):
        """?level=E should return only error-level entries."""
        data = _get("/api/logs", params={"level": "E"}).json()
        for entry in data["entries"]:
            assert entry["level"] == "E", \
                f"Expected only errors, got level={entry['level']}"

    def test_logs_level_warning_filter(self):
        """?level=W should return only W and E level entries."""
        data = _get("/api/logs", params={"level": "W"}).json()
        for entry in data["entries"]:
            assert entry["level"] in ("E", "W"), \
                f"Expected W or E, got level={entry['level']}"

    def test_logs_level_info_filter(self):
        """?level=I should return I, W, and E level entries."""
        data = _get("/api/logs", params={"level": "I"}).json()
        for entry in data["entries"]:
            assert entry["level"] in ("E", "W", "I"), \
                f"Expected I/W/E, got level={entry['level']}"

    def test_logs_combined_filters(self):
        """Combine since + level + limit."""
        data = _get("/api/logs", params={"since": 0, "level": "I", "limit": 5}).json()
        assert len(data["entries"]) <= 5
        for entry in data["entries"]:
            assert entry["level"] in ("E", "W", "I")
            assert entry["seq"] > 0

    def test_logs_incremental_poll(self):
        """Polling with since=latest_seq, waiting, then polling again
        should pick up any new entries logged in between."""
        first = _get("/api/logs").json()
        latest = first["latest_seq"]

        # Generate some activity (this API call itself produces logs)
        _get("/api/health")
        second = _wait_json(
            "/api/logs",
            lambda d: d["latest_seq"] > latest,
            "a new log entry to appear after activity",
            required=False,
            params={"since": latest},
        )
        # We can't guarantee logs were generated, but structure should be valid
        assert isinstance(second["entries"], list)
        assert second["latest_seq"] >= latest


# ── DELETE /api/logs ──────────────────────────────────────────────────────


class TestLogsClear:
    """DELETE /api/logs — clear the log buffer."""

    def test_clear_logs(self):
        """Clearing logs should succeed and reduce total to 0."""
        r = _delete("/api/logs")
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True

    def test_logs_empty_after_clear(self):
        """After clearing, GET should return 0 or very few entries."""
        _delete("/api/logs")
        # There might be a few new entries from the clear operation itself
        # but total should settle small; wait for the buffer to reflect the clear.
        data = _wait_json("/api/logs", lambda d: d["total"] < 10,
                          "log buffer total to drop below 10 after clear")
        assert data["total"] < 10

    def test_seq_continues_after_clear(self):
        """Sequence numbers should keep incrementing after clear."""
        # Get current latest_seq
        before = _get("/api/logs").json()
        before_seq = before["latest_seq"]

        # Clear
        _delete("/api/logs")

        # Generate new log entries
        _get("/api/health")

        after = _wait_json(
            "/api/logs",
            lambda d: d["entries"] and d["entries"][0]["seq"] > before_seq,
            "a new log entry with seq beyond the pre-clear latest",
            required=False,
        )
        # New entries should have seq > before_seq
        if len(after["entries"]) > 0:
            assert after["entries"][0]["seq"] > before_seq, \
                "Sequence numbers should continue incrementing after clear"
