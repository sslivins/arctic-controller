"""
Test: HTTPS / TLS Certificate Lifecycle

Verifies the full TLS certificate lifecycle on the device:
  1. Upload a self-signed certificate
  2. Reboot to activate HTTPS
  3. Confirm HTTPS is serving on port 443
  4. Verify API endpoints work over HTTPS
  5. Delete certificate and reboot back to HTTP-only
  6. Confirm HTTPS is no longer active

These tests require both web auth and API key auth to be enabled
(firmware prerequisite for cert upload). The fixture handles enabling
auth before tests and restoring the original state afterwards.

The test generates a self-signed EC certificate at runtime — no external
cert files or DNS records are needed. DeviceClient uses verify=False,
so hostname mismatch is not an issue.
"""

import os
import subprocess
import tempfile
import time

import pytest
import requests
import urllib3

from device_client import DeviceClient

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

ARCTIC_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY", "")


def _api_headers() -> dict:
    headers = {}
    if API_KEY:
        headers["X-API-Key"] = API_KEY
    return headers


def _api_get(path: str, **kwargs) -> requests.Response:
    return requests.get(
        f"{ARCTIC_URL}{path}", headers=_api_headers(), verify=False, timeout=10, **kwargs
    )


def _api_post(path: str, **kwargs) -> requests.Response:
    return requests.post(
        f"{ARCTIC_URL}{path}", headers=_api_headers(), verify=False, timeout=10, **kwargs
    )


def _api_delete(path: str, **kwargs) -> requests.Response:
    return requests.delete(
        f"{ARCTIC_URL}{path}", headers=_api_headers(), verify=False, timeout=10, **kwargs
    )


def _https_url() -> str:
    """Derive HTTPS URL from the HTTP ARCTIC_URL."""
    return ARCTIC_URL.replace("http://", "https://")


def _tls_status() -> dict:
    return _api_get("/api/tls/status").json()


def _generate_self_signed_cert() -> tuple[str, str]:
    """Generate a self-signed EC P-256 certificate and private key.

    Returns (cert_pem, key_pem) as strings.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        key_path = f"{tmpdir}/key.pem"
        cert_path = f"{tmpdir}/cert.pem"

        # Generate EC private key
        subprocess.run(
            ["openssl", "ecparam", "-genkey", "-name", "prime256v1",
             "-noout", "-out", key_path],
            check=True, capture_output=True,
        )

        # Generate self-signed certificate (valid 1 day)
        subprocess.run(
            ["openssl", "req", "-new", "-x509", "-key", key_path,
             "-out", cert_path, "-days", "1",
             "-subj", "/CN=arctic-test"],
            check=True, capture_output=True,
        )

        with open(cert_path) as f:
            cert_pem = f.read()
        with open(key_path) as f:
            key_pem = f.read()

    return cert_pem, key_pem


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def self_signed_cert() -> tuple[str, str]:
    """Generate a self-signed cert once for the module."""
    return _generate_self_signed_cert()


@pytest.fixture(scope="module")
def auth_enabled(device: DeviceClient):
    """Ensure both web and API auth are enabled (required for cert upload).

    Restores previous auth config after the module completes.
    """
    # Read current config
    r = _api_get("/api/auth/config")
    original = r.json()

    # Enable both if not already
    if not original.get("web_auth_enabled") or not original.get("api_auth_enabled"):
        _api_post(
            "/api/auth/config",
            json={"web_auth_enabled": True, "api_auth_enabled": True},
        )

    yield

    # Restore original auth config
    _api_post(
        "/api/auth/config",
        json={
            "web_auth_enabled": original.get("web_auth_enabled", True),
            "api_auth_enabled": original.get("api_auth_enabled", True),
        },
    )


@pytest.fixture(autouse=True, scope="module")
def clean_tls_state(device: DeviceClient, auth_enabled):
    """Ensure no leftover certs before tests and clean up after."""
    status = _tls_status()
    if status.get("has_certs"):
        _api_delete("/api/tls/certificate")
        device.reboot()
        assert device.wait_for_device(timeout=30.0), "Device did not come back after clearing certs"

    yield

    # Clean up: delete any certs left by tests
    try:
        status = _tls_status()
        if status.get("has_certs"):
            _api_delete("/api/tls/certificate")
            device.reboot()
            device.wait_for_device(timeout=30.0)
    except Exception:
        pass  # best effort cleanup


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestTlsStatus:
    """Verify /api/tls/status reports correct state."""

    def test_no_certs_initially(self, device: DeviceClient, auth_enabled):
        """With no certs provisioned, status should reflect that."""
        status = _tls_status()
        assert status["has_certs"] is False
        assert status["https_active"] is False

    def test_status_requires_auth(self):
        """TLS status endpoint requires API key authentication."""
        r = requests.get(f"{ARCTIC_URL}/api/tls/status", verify=False, timeout=10)
        assert r.status_code == 401


class TestCertUpload:
    """Verify certificate upload validation."""

    def test_upload_requires_auth(self, self_signed_cert):
        """Cert upload without API key returns 401."""
        cert_pem, key_pem = self_signed_cert
        r = requests.post(
            f"{ARCTIC_URL}/api/tls/certificate",
            json={"cert": cert_pem, "key": key_pem},
            verify=False, timeout=10,
        )
        assert r.status_code == 401

    def test_upload_rejects_invalid_cert(self, auth_enabled):
        """Non-PEM cert is rejected with 400."""
        r = _api_post(
            "/api/tls/certificate",
            json={"cert": "not a valid cert", "key": "-----BEGIN PRIVATE KEY-----\nfake\n-----END PRIVATE KEY-----"},
        )
        assert r.status_code == 400
        assert "PEM" in r.json().get("error", "")

    def test_upload_rejects_invalid_key(self, self_signed_cert, auth_enabled):
        """Non-PEM key is rejected with 400."""
        cert_pem, _ = self_signed_cert
        r = _api_post(
            "/api/tls/certificate",
            json={"cert": cert_pem, "key": "not a valid key"},
        )
        assert r.status_code == 400
        assert "PEM" in r.json().get("error", "")

    def test_upload_rejects_missing_fields(self, auth_enabled):
        """Missing cert or key field returns 400."""
        r = _api_post("/api/tls/certificate", json={"cert": "something"})
        assert r.status_code == 400


class TestHttpsLifecycle:
    """Full lifecycle: upload → reboot → HTTPS active → delete → reboot → HTTP only."""

    def test_upload_cert(self, device: DeviceClient, self_signed_cert, auth_enabled):
        """Upload self-signed cert succeeds and sets has_certs."""
        cert_pem, key_pem = self_signed_cert
        r = _api_post(
            "/api/tls/certificate",
            json={"cert": cert_pem, "key": key_pem},
        )
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True

        # Certs stored but HTTPS not active until reboot
        status = _tls_status()
        assert status["has_certs"] is True
        assert status["https_active"] is False

    def test_reboot_activates_https(self, device: DeviceClient, self_signed_cert, auth_enabled):
        """After uploading cert and rebooting, HTTPS should be active on port 443."""
        # Ensure cert is uploaded
        cert_pem, key_pem = self_signed_cert
        status = _tls_status()
        if not status.get("has_certs"):
            r = _api_post(
                "/api/tls/certificate",
                json={"cert": cert_pem, "key": key_pem},
            )
            assert r.status_code == 200

        device.reboot()
        assert device.wait_for_device(timeout=30.0), "Device did not come back after reboot"
        time.sleep(2)  # give HTTPS server time to start

        # HTTPS should now be active
        https_url = _https_url()
        r = requests.get(f"{https_url}/api/health", verify=False, timeout=10)
        assert r.status_code == 200
        data = r.json()
        assert data["status"] == "ok"

    def test_api_works_over_https(self, device: DeviceClient, self_signed_cert, auth_enabled):
        """Production API endpoints work over HTTPS."""
        # Ensure HTTPS is active (cert uploaded + rebooted from previous test)
        https_url = _https_url()
        try:
            r = requests.get(f"{https_url}/api/health", verify=False, timeout=10)
            if r.status_code != 200:
                pytest.skip("HTTPS not active — previous test may have failed")
        except Exception:
            pytest.skip("HTTPS not reachable")

        # Test authenticated endpoint over HTTPS
        r = requests.get(
            f"{https_url}/api/tls/status",
            headers=_api_headers(), verify=False, timeout=10,
        )
        assert r.status_code == 200
        status = r.json()
        assert status["has_certs"] is True
        assert status["https_active"] is True

    def test_delete_cert_and_revert_to_http(self, device: DeviceClient, auth_enabled):
        """Deleting cert and rebooting reverts to HTTP-only."""
        https_url = _https_url()

        # Delete cert (try HTTPS first since that's where routes are registered
        # when HTTPS is active, fall back to HTTP)
        deleted = False
        for url in [https_url, ARCTIC_URL]:
            try:
                r = requests.delete(
                    f"{url}/api/tls/certificate",
                    headers=_api_headers(), verify=False, timeout=10,
                )
                if r.status_code == 200:
                    deleted = True
                    break
            except Exception:
                continue

        assert deleted, "Failed to delete certificate on both HTTP and HTTPS"

        device.reboot()
        assert device.wait_for_device(timeout=30.0), "Device did not come back after reboot"

        # HTTPS should no longer be active
        status = _tls_status()
        assert status["has_certs"] is False
        assert status["https_active"] is False

        # Port 443 should refuse connections
        with pytest.raises(Exception):
            requests.get(f"{https_url}/api/health", verify=False, timeout=3)
