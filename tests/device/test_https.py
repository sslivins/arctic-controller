"""
Test: HTTP / HTTPS mode and the administrator certificate lifecycle

The controller runs in one of two modes, chosen at boot:

  HTTP mode (no administrator certificate, the default)
    Port 80 serves the full web UI and API. Port 443 is closed. The
    self-signed device identity is used only by the Home Assistant server on
    8443, never by the web UI.

  HTTPS mode (administrator certificate installed)
    Port 443 serves the web UI and API with that certificate. Port 80 serves
    only /api/health plus a 404 telling the user to use HTTPS -- never a
    redirect, and never the full UI.

Uploading a certificate restarts the controller into HTTPS mode; deleting it
restarts it into HTTP mode. This module walks that lifecycle and leaves the
controller in the mode it found it in. If it starts in HTTPS mode, the
administrator certificate can only be put back when ARCTIC_ADMIN_CERT_FILE
and ARCTIC_ADMIN_KEY_FILE name its PEM files (CI's "HTTP to HTTPS" step writes
them), so without them the lifecycle tests are skipped rather than discarding
a certificate they cannot restore.

The test generates a self-signed EC certificate at runtime -- no external
cert files or DNS records are needed. Requests use verify=False, so the
hostname mismatch does not matter.
"""

import datetime
import os
import time
from pathlib import Path
from urllib.parse import urlsplit

import pytest
import requests
import urllib3

from device_client import DeviceClient

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

ARCTIC_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY", "")


def _read_pem(env: str) -> str:
    path = os.environ.get(env, "")
    return Path(path).read_text() if path and Path(path).is_file() else ""


ADMIN_CERT = _read_pem("ARCTIC_ADMIN_CERT_FILE")
ADMIN_KEY = _read_pem("ARCTIC_ADMIN_KEY_FILE")

HOST = urlsplit(ARCTIC_URL).netloc
HTTP_URL = f"http://{HOST}"
HTTPS_URL = f"https://{HOST}"

# Covers the 500 ms response delay, a full boot and the servers starting.
RESTART_TIMEOUT = 120.0


def _headers() -> dict:
    return {"X-API-Key": API_KEY} if API_KEY else {}


def _get(base: str, path: str, **kwargs) -> requests.Response:
    kwargs.setdefault("timeout", 10)
    return requests.get(f"{base}{path}", headers=_headers(), verify=False, **kwargs)


def _https_open() -> bool:
    try:
        return _get(HTTPS_URL, "/api/health", timeout=5).status_code == 200
    except requests.RequestException:
        return False


def _mode() -> str:
    """'https' when port 443 answers, otherwise 'http'."""
    return "https" if _https_open() else "http"


def _base() -> str:
    return HTTPS_URL if _mode() == "https" else HTTP_URL


def _tls_status(base: str) -> dict:
    r = _get(base, "/api/tls/status")
    r.raise_for_status()
    return r.json()


def _wait_for_mode(mode: str, timeout: float = RESTART_TIMEOUT) -> bool:
    """Wait until the controller has restarted into ``mode``.

    The wrong mode can never satisfy the check: in HTTP mode 443 is closed,
    and in HTTPS mode port 80 has no /api/tls/status route.
    """
    base = HTTPS_URL if mode == "https" else HTTP_URL
    want_certs = mode == "https"
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            status = _tls_status(base)
            if (status.get("has_certs") is want_certs
                    and status.get("https_active") is want_certs
                    and (mode == "https" or not _https_open())):
                return True
        except (requests.RequestException, ValueError):
            pass
        time.sleep(2)
    return False


def _follow(device: DeviceClient, mode: str) -> None:
    """Point the shared device client at ``mode`` after a restart into it.

    The suite's autouse fixtures talk to the controller through ``device``
    before every test, so it must use whichever port is serving. A restart
    also clears the in-memory test lock.
    """
    device.base_url = HTTPS_URL if mode == "https" else HTTP_URL
    device.lock(ttl_seconds=900)


def _upload(base: str, cert_pem: str, key_pem: str) -> requests.Response:
    return requests.post(
        f"{base}/api/tls/certificate", headers=_headers(),
        json={"cert": cert_pem, "key": key_pem}, verify=False, timeout=15,
    )


def _delete(base: str) -> requests.Response:
    return requests.delete(
        f"{base}/api/tls/certificate", headers=_headers(), verify=False, timeout=15,
    )


def _generate_self_signed_cert() -> tuple[str, str]:
    """A self-signed EC P-256 certificate and its key, as PEM strings."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "arctic-test")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name).issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=1))
        .sign(key, hashes.SHA256())
    )
    return (
        cert.public_bytes(serialization.Encoding.PEM).decode(),
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        ).decode(),
    )


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def self_signed_cert() -> tuple[str, str]:
    return _generate_self_signed_cert()


@pytest.fixture(scope="module")
def auth_enabled(device: DeviceClient):
    """Both auth methods must be on before a certificate can be uploaded."""
    base = _base()
    original = _get(base, "/api/auth/config").json()
    if not original.get("web_auth_enabled") or not original.get("api_auth_enabled"):
        requests.post(
            f"{base}/api/auth/config", headers=_headers(), verify=False, timeout=10,
            json={"web_auth_enabled": True, "api_auth_enabled": True},
        )
    yield
    base = _base()
    requests.post(
        f"{base}/api/auth/config", headers=_headers(), verify=False, timeout=10,
        json={
            "web_auth_enabled": original.get("web_auth_enabled", True),
            "api_auth_enabled": original.get("api_auth_enabled", True),
        },
    )


@pytest.fixture(scope="module")
def http_mode(device: DeviceClient, auth_enabled):
    """Put the controller in HTTP mode for the lifecycle, and back afterwards."""
    started_in = _mode()
    if started_in == "https":
        if not (ADMIN_CERT and ADMIN_KEY):
            # ARCTIC_ADMIN_CERT_FILE / ARCTIC_ADMIN_KEY_FILE are needed to put it back.
            pytest.skip("Cannot restore the administrator certificate afterwards")
        assert _delete(HTTPS_URL).status_code == 200
        assert _wait_for_mode("http"), "Controller did not restart into HTTP mode"
        _follow(device, "http")

    yield

    try:
        if started_in == "https":
            if _mode() == "http":
                _upload(HTTP_URL, ADMIN_CERT, ADMIN_KEY)
            restored = _wait_for_mode("https")
        else:
            if _mode() == "https":
                _delete(HTTPS_URL)
            restored = _wait_for_mode("http")
        if not restored:
            print(f"WARNING: could not restore {started_in.upper()} mode after "
                  "the HTTPS tests; later suites may fail.")
        _follow(device, started_in)
    except Exception as e:
        print(f"WARNING: HTTPS teardown error: {e}")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestCertUploadValidation:
    """Rejected uploads never store anything, so they need no mode change."""

    def test_status_requires_auth(self):
        r = requests.get(f"{_base()}/api/tls/status", verify=False, timeout=10)
        assert r.status_code == 401

    def test_upload_requires_auth(self, self_signed_cert):
        cert_pem, key_pem = self_signed_cert
        r = requests.post(
            f"{_base()}/api/tls/certificate",
            json={"cert": cert_pem, "key": key_pem}, verify=False, timeout=10,
        )
        assert r.status_code == 401

    def test_upload_rejects_invalid_cert(self, auth_enabled):
        r = _upload(_base(), "not a valid cert",
                    "-----BEGIN PRIVATE KEY-----\nfake\n-----END PRIVATE KEY-----")
        assert r.status_code == 400
        assert "PEM" in r.json().get("error", "")

    def test_upload_rejects_invalid_key(self, self_signed_cert, auth_enabled):
        cert_pem, _ = self_signed_cert
        r = _upload(_base(), cert_pem, "not a valid key")
        assert r.status_code == 400
        assert "PEM" in r.json().get("error", "")

    def test_upload_rejects_missing_fields(self, auth_enabled):
        r = requests.post(
            f"{_base()}/api/tls/certificate", headers=_headers(),
            json={"cert": "something"}, verify=False, timeout=10,
        )
        assert r.status_code == 400


class TestHttpMode:
    """Without an administrator certificate the dashboard is plain HTTP."""

    def test_status_reports_http_mode(self, http_mode):
        status = _tls_status(HTTP_URL)
        assert status["has_certs"] is False
        assert status["https_active"] is False
        assert status["integration_identity"] is True

    def test_port_443_is_closed(self, http_mode):
        assert not _https_open()

    def test_dashboard_is_served_over_http(self, http_mode):
        r = requests.get(f"{HTTP_URL}/", allow_redirects=False, timeout=10)
        assert r.status_code == 200
        assert "text/html" in r.headers.get("Content-Type", "")


class TestHttpsLifecycle:
    """Upload -> restart into HTTPS mode -> delete -> restart into HTTP mode."""

    def test_upload_restarts_into_https_mode(self, device, http_mode, self_signed_cert):
        cert_pem, key_pem = self_signed_cert
        r = _upload(HTTP_URL, cert_pem, key_pem)
        assert r.status_code == 200, r.text[:300]
        data = r.json()
        assert data["success"] is True
        assert data["restarting"] is True
        assert data["scheme"] == "https"
        assert _wait_for_mode("https"), "Controller did not restart into HTTPS mode"
        _follow(device, "https")

    def test_api_works_over_https(self, http_mode):
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        status = _tls_status(HTTPS_URL)
        assert status["has_certs"] is True
        assert status["https_active"] is True
        r = requests.get(f"{HTTPS_URL}/", verify=False, timeout=10)
        assert r.status_code == 200

    def test_http_root_is_a_404_not_a_redirect(self, http_mode):
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        r = requests.get(f"{HTTP_URL}/", allow_redirects=False, timeout=10)
        assert r.status_code == 404
        assert "Location" not in r.headers
        assert "https" in r.text.lower()

    def test_http_api_is_not_served_in_https_mode(self, http_mode):
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        r = requests.get(f"{HTTP_URL}/api/tls/status", headers=_headers(),
                         allow_redirects=False, timeout=10)
        assert r.status_code == 404
        assert "Location" not in r.headers

    def test_http_health_still_served_in_https_mode(self, http_mode):
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        r = requests.get(f"{HTTP_URL}/api/health", allow_redirects=False, timeout=10)
        assert r.status_code == 200

    def test_delete_restarts_into_http_mode(self, device, http_mode):
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        r = _delete(HTTPS_URL)
        assert r.status_code == 200, r.text[:300]
        data = r.json()
        assert data["restarting"] is True
        assert data["scheme"] == "http"
        assert _wait_for_mode("http"), "Controller did not restart into HTTP mode"
        _follow(device, "http")
        r = requests.get(f"{HTTP_URL}/", timeout=10)
        assert r.status_code == 200
