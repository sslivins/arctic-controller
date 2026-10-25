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
(over the API or from the touchscreen) restarts it into HTTP mode. If port 443
fails to start while a certificate is installed, the controller falls back to
serving everything on port 80 rather than leaving no web UI. This module walks that lifecycle and leaves the
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
# Self-probe every 60 s, three failures 15 s apart before each action, and two
# actions (restart, then fall back) before HTTPS is abandoned.
UNRESPONSIVE_TIMEOUT = 240.0


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
    want_certs = mode == "https"
    return _poll_tls_status(
        HTTPS_URL if want_certs else HTTP_URL,
        lambda s: (s.get("has_certs") is want_certs
                   and s.get("https_active") is want_certs
                   and (want_certs or not _https_open())),
        timeout,
    ) is not None


def _poll(probe, timeout: float):
    """Call ``probe()`` until it returns something truthy; that value, or None.
    Request errors count as "not yet"."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            value = probe()
            if value:
                return value
        except (requests.RequestException, ValueError):
            pass
        time.sleep(2)
    return None


def _poll_tls_status(base: str, ready, timeout: float) -> dict | None:
    """Poll ``base``/api/tls/status until ``ready(status)``; the status, or None."""
    def probe():
        status = _tls_status(base)
        return status if ready(status) else None
    return _poll(probe, timeout)


def _events(base: str) -> dict:
    return _get(base, "/api/events", params={"limit": 100}).json()


def _boot_events(base: str, kind: str) -> list:
    """This boot's events of type ``kind``, newest first."""
    data = _events(base)
    return [e for e in data["events"]
            if e["type"] == kind and e["boot_id"] == data["current_boot_id"]]


def _follow(device: DeviceClient, mode: str) -> None:
    """Point the shared device client at ``mode`` after a restart into it.

    The suite's autouse fixtures talk to the controller through ``device``
    before every test, so it must use whichever port is serving. A reboot
    also clears the in-memory test lock.
    """
    device.base_url = HTTPS_URL if mode == "https" else HTTP_URL
    # An in-place server restart (no reboot) keeps our lock; re-locking would
    # then be refused as "another session".
    status = device.check_lock()
    if not (status.get("locked") and status.get("session_id") == device._session_id):
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


def _kill_servers(device: DeviceClient, base: str, **body) -> None:
    """Stop the web/API servers; the WiFi supervisor restarts them in place."""
    r = device.session.post(f"{base}/api/test/api-server-kill", json=body,
                            verify=False, timeout=10)
    assert r.status_code == 202, r.text[:300]


def _wait_for_https_fallback(timeout: float = RESTART_TIMEOUT) -> dict | None:
    """Wait for HTTP mode with the certificate still installed (443 failed)."""
    return _poll_tls_status(
        HTTP_URL, lambda s: s.get("has_certs") and s.get("https_failed"), timeout)


def _open_web_screen(device: DeviceClient) -> None:
    device.click(tag="settings")
    assert device.wait_for_screen("settings", timeout=5.0)
    assert device.wait_for_widget(tag="settings_web", timeout=5.0)
    device.click(tag="settings_web")
    assert device.wait_for_screen("web", timeout=5.0)


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

    def test_https_start_failure_falls_back_to_http(self, device, http_mode):
        """A failed port-443 start serves the full UI on port 80, not nothing."""
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        _kill_servers(device, HTTPS_URL, fail_https_starts=1)

        status = _wait_for_https_fallback()
        assert status is not None, "Controller did not fall back to HTTP"
        assert status["https_active"] is False
        device.base_url = HTTP_URL
        assert not _https_open()
        r = requests.get(f"{HTTP_URL}/", timeout=10)
        assert r.status_code == 200
        assert "text/html" in r.headers.get("Content-Type", "")

        fallback = _boot_events(HTTP_URL, "https_fallback")
        assert fallback, "no https_fallback event"
        assert fallback[0]["category"] == "problems"
        assert fallback[0]["cause"] == "test"

        # The next server restart retries HTTPS with the stored certificate.
        _kill_servers(device, HTTP_URL)
        assert _wait_for_mode("https"), "Controller did not return to HTTPS"
        _follow(device, "https")
        assert _tls_status(HTTPS_URL)["https_failed"] is False

    def test_bad_certificate_falls_back_to_http(self, device, http_mode):
        """A cert/key pair that will not load is caught before 443 starts.

        Otherwise 443 would come up, fail every handshake, and never fall back.
        HTTPS then stays off for the rest of the boot (a restart cannot fix the
        stored pair).
        """
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        _kill_servers(device, HTTPS_URL, fail_cert_checks=1)

        status = _wait_for_https_fallback()
        assert status is not None, "Controller did not fall back to HTTP"
        device.base_url = HTTP_URL
        assert _boot_events(HTTP_URL, "https_fallback")[0]["cause"] == "bad_certificate"

        # A plain restart keeps HTTPS off for this boot...
        restarts = len(_boot_events(HTTP_URL, "web_server_recovered"))
        _kill_servers(device, HTTP_URL)
        assert _poll(lambda: len(_boot_events(HTTP_URL, "web_server_recovered")) > restarts,
                     RESTART_TIMEOUT), "servers were not restarted"
        assert _tls_status(HTTP_URL)["https_failed"] is True
        assert not _https_open()

        # ...until the test hook re-enables it.
        _kill_servers(device, HTTP_URL, reenable_https=True)
        assert _wait_for_mode("https"), "Controller did not return to HTTPS"
        _follow(device, "https")

    def test_port_80_failure_leaves_https_running(self, device, http_mode):
        """Port 80 is secondary in HTTPS mode: a failed start there neither
        takes 443 down nor triggers a fallback, and is retried by itself."""
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        fallbacks = len(_boot_events(HTTPS_URL, "https_fallback"))
        restarts = len(_boot_events(HTTPS_URL, "web_server_recovered"))
        _kill_servers(device, HTTPS_URL, fail_http_starts=1)

        # The supervisor restarts the killed servers (443 comes back), then
        # repairs port 80 on its slower secondary-server cadence.
        assert _poll(lambda: len(_boot_events(HTTPS_URL, "web_server_recovered")) > restarts,
                     RESTART_TIMEOUT), "servers were not restarted"
        assert _wait_for_mode("https"), "HTTPS did not come back"
        r = _poll(lambda: _get(HTTP_URL, "/api/health", timeout=5).status_code == 200,
                  RESTART_TIMEOUT)
        assert r, "port 80 was not repaired"
        assert _https_open()
        assert len(_boot_events(HTTPS_URL, "https_fallback")) == fallbacks

    def test_unresponsive_https_falls_back_to_http(self, device, http_mode):
        """443 running but not answering: restart, then fall back to HTTP.

        The supervisor's loopback /api/health probe is made to fail (it runs
        every minute), so it restarts the servers after three failures and,
        when 443 still does not answer, serves the UI on port 80 instead.
        """
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        _kill_servers(device, HTTPS_URL, fail_probes=6, stop=False)

        status = _wait_for_https_fallback(timeout=UNRESPONSIVE_TIMEOUT)
        assert status is not None, "Controller did not fall back to HTTP"
        device.base_url = HTTP_URL
        assert _boot_events(HTTP_URL, "https_fallback")[0]["cause"] == "unresponsive"
        down = _boot_events(HTTP_URL, "web_server_down")
        assert down and down[0]["cause"] == "unresponsive"
        # Once port 80 answers the probe, the outage is closed.
        assert _poll(lambda: [e for e in _boot_events(HTTP_URL, "web_server_recovered")
                              if e["uptime_ms"] > down[0]["uptime_ms"]],
                     RESTART_TIMEOUT), "no web_server_recovered after the fallback"

        _kill_servers(device, HTTP_URL, reenable_https=True)
        assert _wait_for_mode("https"), "Controller did not return to HTTPS"
        _follow(device, "https")

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

    def test_reupload_restarts_into_https_mode(self, device, http_mode, self_signed_cert):
        assert not _https_open(), "Still in HTTPS mode -- previous test failed"
        cert_pem, key_pem = self_signed_cert
        r = _upload(HTTP_URL, cert_pem, key_pem)
        assert r.status_code == 200, r.text[:300]
        assert _wait_for_mode("https"), "Controller did not restart into HTTPS mode"
        _follow(device, "https")

    def test_touchscreen_removes_certificate(self, device, http_mode):
        """Settings -> Web Interface -> Remove certificate restarts into HTTP."""
        if not _https_open():
            pytest.skip("HTTPS not active -- previous test failed")
        _open_web_screen(device)
        assert device.wait_for_widget(tag="web_remove_cert", timeout=5.0)
        cert_status = device.find_widget(tag="web_cert_status")
        assert cert_status is not None and cert_status.text

        # Cancel leaves the certificate alone.
        device.click(tag="web_remove_cert")
        assert device.wait_for_widget(tag="web_remove_cert_cancel", timeout=5.0)
        device.click(tag="web_remove_cert_cancel")
        device.wait_until(
            "the remove-certificate dialog to close",
            lambda: device.find_widget(tag="web_remove_cert_overlay") is None,
            timeout=5.0)
        assert _tls_status(HTTPS_URL)["has_certs"] is True

        device.click(tag="web_remove_cert")
        assert device.wait_for_widget(tag="web_remove_cert_confirm", timeout=5.0)
        try:
            device.click(tag="web_remove_cert_confirm")
        except requests.RequestException:
            pass  # the restart can race the click's response
        assert _wait_for_mode("http"), "Controller did not restart into HTTP mode"
        _follow(device, "http")
        assert _tls_status(HTTP_URL)["has_certs"] is False
