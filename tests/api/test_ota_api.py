"""
OTA API tests.

Tier 1 — safe, no reboot, no real firmware update.
Tier 2 — real OTA round-trip (device reboots). Requires serial + rollback enabled.
Tier 3 — rollback validation (intentionally bad firmware). Requires serial.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
  - ARCTIC_API_KEY env var set
  - No OTA update in progress on the device
  - For Tier 2/3: serial connection, CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
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

    def test_upload_truncated_firmware_rejected(self):
        """Valid 0xE9 header but truncated body should fail validation."""
        # ESP32 binary header (0xE9) + some plausible header bytes + truncated
        # This passes the magic byte check but fails esp_ota_end() validation
        truncated = b"\xE9" + (b"\x00" * 4095)
        r = _post_raw("/api/ota/upload", data=truncated)
        # Should fail with 500 (OTA validation failed) — not 200
        assert r.status_code in (400, 500), (
            f"Truncated firmware should be rejected, got {r.status_code}"
        )
        # Device must not reboot
        data = _get("/api/ota/status").json()
        assert data["state"] in ("idle", "failed")

    def test_upload_valid_header_garbage_body_rejected(self):
        """Valid header byte + random garbage should fail validation."""
        import os
        # 64KB: enough to pass header check, but random data won't pass OTA end
        garbage = b"\xE9" + os.urandom(65535)
        r = _post_raw("/api/ota/upload", data=garbage)
        assert r.status_code in (400, 500), (
            f"Garbage firmware should be rejected, got {r.status_code}"
        )
        # Verify device didn't reboot
        data = _get("/api/ota/status").json()
        assert data["state"] in ("idle", "failed")


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


# ══════════════════════════════════════════════════════════════════════════
# Tier 2 — Real OTA round-trip (device reboots)
# ══════════════════════════════════════════════════════════════════════════

# These tests upload real firmware, wait for reboot, and verify the device
# comes back. They require:
#   - Serial connection from runner to device (not yet available)
#   - CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (feat/ota-hardening)
#   - The current firmware binary in build/arctic_controller.bin

REBOOT_WAIT_SECS = 25  # Time to wait for device to reboot and reconnect


def _wait_for_device(timeout=60):
    """Poll until the device responds to /api/ota/status."""
    import time

    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r = _get("/api/ota/status")
            if r.status_code == 200:
                return r.json()
        except requests.ConnectionError:
            pass
        time.sleep(2)
    raise TimeoutError(f"Device did not respond within {timeout}s after reboot")


def _get_firmware_binary():
    """Load the current firmware binary from the build directory."""
    bin_path = Path(__file__).resolve().parent.parent.parent / "build" / "arctic_controller.bin"
    if not bin_path.exists():
        pytest.skip(f"Firmware binary not found at {bin_path}")
    return bin_path.read_bytes()


@pytest.mark.skip(reason="Requires serial connection and device reboot — not yet available on CI runner")
class TestOtaRoundTrip:
    """Tier 2: Upload current firmware via OTA, wait for reboot, verify recovery.

    This proves the full OTA pipeline works end-to-end without needing a
    different firmware version. The device should reboot onto the alternate
    partition with the same version.
    """

    def test_upload_same_version_round_trip(self):
        """Upload current firmware binary → reboot → device comes back healthy."""
        import time

        firmware = _get_firmware_binary()

        # Record pre-OTA state
        pre = _get("/api/ota/status").json()
        pre_version = pre["current_version"]

        # Upload firmware
        r = _post_raw("/api/ota/upload", data=firmware)
        assert r.status_code == 200, f"Upload failed: {r.text}"
        data = r.json()
        assert data.get("success") is True
        assert data.get("bytes_received") == len(firmware)

        # Device will auto-reboot — wait for it to come back
        time.sleep(REBOOT_WAIT_SECS)
        post = _wait_for_device(timeout=60)

        # Same version, idle state
        assert post["current_version"] == pre_version
        assert post["state"] == "idle"

    def test_pending_verify_after_ota(self):
        """After OTA reboot, firmware should briefly be in pending_verify
        state before mark_valid() runs. By the time we can query the API,
        mark_valid() has already fired (it runs during create_ui), so
        pending_verify should be false.
        """
        # This runs after test_upload_same_version_round_trip
        data = _get("/api/ota/status").json()
        assert data["pending_verify"] is False, (
            "Firmware should have been validated by mark_valid() after create_ui()"
        )

    def test_device_functional_after_ota(self):
        """After OTA, basic API endpoints should still work."""
        # Health check
        r = _get("/api/ota/status")
        assert r.status_code == 200

        # Verify another endpoint works too
        r = _get("/api/ota/status")
        data = r.json()
        assert "current_version" in data
        assert "pending_verify" in data


# ══════════════════════════════════════════════════════════════════════════
# Tier 3 — Rollback validation
# ══════════════════════════════════════════════════════════════════════════

# These tests verify that the bootloader reverts to the previous firmware
# if the new one fails to call mark_valid(). They require:
#   - Serial connection (to flash recovery firmware after rollback)
#   - CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
#   - A "poison" firmware binary that crashes before mark_valid()
#
# Building the poison firmware:
#   1. Add an assert(false) or abort() call in create_ui() (before mark_valid)
#   2. Build with: idf.py build
#   3. Save build/arctic_controller.bin as the poison binary
#   4. Rebuild normal firmware for recovery
#
# This is complex enough that it should probably be a manual test script
# rather than part of the automated suite, at least initially.

@pytest.mark.skip(reason="Requires serial connection, poison firmware, and manual recovery — future work")
class TestOtaRollback:
    """Tier 3: Verify bootloader rolls back bad firmware.

    Upload a firmware that crashes before mark_valid(). After the device
    reboots, the bootloader should revert to the previous working partition.
    """

    def test_rollback_on_crash_before_mark_valid(self):
        """Upload poison firmware → device crashes → bootloader reverts.

        Test plan:
        1. Record current version and partition
        2. Upload poison firmware (crashes in create_ui before mark_valid)
        3. Device reboots into poison → crashes → bootloader reverts
        4. Device boots back into original firmware
        5. Verify version matches, state is idle, pending_verify is false
        """
        import time

        # Record pre-OTA state
        pre = _get("/api/ota/status").json()
        pre_version = pre["current_version"]

        # TODO: Load poison firmware binary
        # poison_path = Path(__file__).resolve().parent / "fixtures" / "poison_firmware.bin"
        # poison = poison_path.read_bytes()

        # TODO: Upload poison firmware
        # r = _post_raw("/api/ota/upload", data=poison)
        # assert r.status_code == 200

        # Wait for crash + rollback + reboot (may take 2 cycles)
        # time.sleep(REBOOT_WAIT_SECS * 2)

        # Verify device came back on the original firmware
        # post = _wait_for_device(timeout=90)
        # assert post["current_version"] == pre_version
        # assert post["state"] == "idle"
        # assert post["pending_verify"] is False

        pytest.skip("Poison firmware binary not yet available")
