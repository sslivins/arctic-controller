"""
OTA API safety tests.

Tier 1 tests — safe, no reboot, no real firmware update.
Validates OTA status reporting, concurrent prevention, bad upload rejection,
URL validation, and version consistency.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
  - No OTA update in progress on the device
"""

import os
import re

import pytest
import requests
from pathlib import Path

# Load .env from repo root if present (local dev)
_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")


def _headers():
    h = {}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    return h


def _get(path, **kwargs):
    return requests.get(f"{BASE_URL}{path}", headers=_headers(), timeout=10, **kwargs)


def _post(path, json=None, **kwargs):
    return requests.post(
        f"{BASE_URL}{path}", headers=_headers(), json=json, timeout=10, **kwargs
    )


def _post_raw(path, data=None, content_type="application/octet-stream", **kwargs):
    """POST with raw binary data (for firmware upload tests)."""
    h = _headers()
    h["Content-Type"] = content_type
    return requests.post(
        f"{BASE_URL}{path}", headers=h, data=data, timeout=30, **kwargs
    )


def _get_cmake_version():
    """Read PROJECT_VER from CMakeLists.txt."""
    cmake_path = Path(__file__).resolve().parent.parent.parent / "CMakeLists.txt"
    text = cmake_path.read_text()
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    return m.group(1) if m else None


# ── OTA Status — pending_verify field ─────────────────────────────────────


class TestOtaPendingVerify:
    """Verify the pending_verify field in OTA status response."""

    def test_status_has_pending_verify_field(self):
        """Status response must include the pending_verify boolean."""
        data = _get("/api/ota/status").json()
        assert "pending_verify" in data
        assert isinstance(data["pending_verify"], bool)

    def test_not_pending_after_usb_flash(self):
        """After a USB flash (not OTA), pending_verify should be false."""
        data = _get("/api/ota/status").json()
        assert data["pending_verify"] is False


# ── OTA Status — version reporting ────────────────────────────────────────


class TestOtaVersionReporting:
    """Verify firmware version consistency."""

    def test_version_matches_cmake(self):
        """Reported version must match PROJECT_VER in CMakeLists.txt."""
        cmake_ver = _get_cmake_version()
        assert cmake_ver is not None, "Could not read PROJECT_VER from CMakeLists.txt"
        data = _get("/api/ota/status").json()
        assert data["current_version"] == cmake_ver

    def test_version_is_semver(self):
        """Version should follow semver pattern (major.minor.patch)."""
        data = _get("/api/ota/status").json()
        parts = data["current_version"].split(".")
        assert len(parts) == 3
        assert all(p.isdigit() for p in parts)


# ── OTA Upload — bad data rejection ──────────────────────────────────────


class TestOtaUploadBadData:
    """Verify that uploading invalid firmware data is rejected safely."""

    def test_upload_random_bytes_rejected(self):
        """Random bytes (not starting with 0xE9) should be rejected as 400."""
        # 1KB of 0xFF — not a valid ESP32 binary
        bad_data = b"\xFF" * 1024
        r = _post_raw("/api/ota/upload", data=bad_data)
        assert r.status_code == 400
        data = r.json()
        assert "error" in data or "message" in data

    def test_upload_empty_body_rejected(self):
        """Empty upload should fail without crashing."""
        r = _post_raw("/api/ota/upload", data=b"")
        # Could be 400 or 500 — either is acceptable as long as it doesn't crash
        assert r.status_code in (400, 500)

    def test_device_idle_after_bad_upload(self):
        """After a rejected upload, device should return to idle state."""
        # Send bad data first
        bad_data = b"\x00" * 512
        _post_raw("/api/ota/upload", data=bad_data)

        # Verify device is back to idle
        data = _get("/api/ota/status").json()
        assert data["state"] == "idle"
        assert data["progress"] == 0


# ── OTA Update — URL validation ──────────────────────────────────────────


class TestOtaUrlValidation:
    """Verify URL validation on the /api/ota/update endpoint."""

    def test_reject_non_github_url(self):
        """Non-GitHub URLs should be rejected with 403."""
        r = _post("/api/ota/update", json={"url": "http://evil.com/firmware.bin"})
        assert r.status_code == 403

    def test_reject_http_github_url(self):
        """HTTP (not HTTPS) GitHub URLs should be rejected."""
        r = _post(
            "/api/ota/update",
            json={"url": "http://github.com/sslivins/arctic-controller/releases/download/v1.0.0/firmware.bin"},
        )
        assert r.status_code == 403

    def test_reject_missing_url_field(self):
        """Request without url field should be rejected with 400."""
        r = _post("/api/ota/update", json={"firmware": "test"})
        assert r.status_code == 400

    def test_reject_empty_body(self):
        """Request with no JSON body should be rejected with 400."""
        r = _post("/api/ota/update")
        assert r.status_code == 400


# ── OTA Concurrent prevention ────────────────────────────────────────────


class TestOtaConcurrentPrevention:
    """Verify that only one OTA operation can run at a time.

    Note: ESP-IDF's HTTP server serializes requests, so two uploads can't
    truly overlap at the HTTP level. The real concurrent protection is
    between a URL-based download (FreeRTOS background task) and an upload
    (HTTP handler). That scenario requires a URL download to a slow/valid
    GitHub URL which we don't want in automated tests.

    We verify the API contract here: if a download IS in progress,
    the upload endpoint should reject with 409. We test this indirectly
    by starting a URL download to a non-existent (but allowed) GitHub URL
    and immediately trying to upload.
    """

    def test_upload_rejected_during_url_download(self):
        """Upload should be rejected with 409 while a URL download is running."""
        import time

        # Start a URL-based OTA download to a valid-prefix but non-existent URL.
        # The download task will run in the background and eventually fail.
        fake_url = (
            "https://github.com/sslivins/arctic-controller/"
            "releases/download/v0.0.0/firmware_nonexistent.bin"
        )
        r1 = _post("/api/ota/update", json={"url": fake_url})
        assert r1.status_code == 200, f"Failed to start download: {r1.text}"

        # Immediately attempt an upload — should be rejected
        bad_data = b"\xE9" + (b"\x00" * 1024)
        r2 = _post_raw("/api/ota/upload", data=bad_data)
        assert r2.status_code == 409, (
            f"Expected 409 during download, got {r2.status_code}: {r2.text}"
        )

        # Wait for the background download to fail (DNS/404)
        for _ in range(30):
            time.sleep(1)
            data = _get("/api/ota/status").json()
            if data["state"] in ("idle", "failed"):
                break

        # Device should be back to idle or failed (not stuck)
        data = _get("/api/ota/status").json()
        assert data["state"] in ("idle", "failed"), (
            f"Expected idle or failed, got {data['state']}"
        )
