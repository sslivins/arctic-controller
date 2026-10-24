#!/usr/bin/env python3
"""CI: check HTTP mode, install an administrator certificate, check HTTPS mode.

A controller without an administrator certificate serves the web UI and API
over plain HTTP. Installing one restarts it into HTTPS mode, after which port
80 serves only /api/health and a "use HTTPS" 404. The suites run in HTTPS
mode, so this runs between the upgrade checks (which run in HTTP mode) and the
suites, and proves each half of the switch:

  1. HTTP mode works: the dashboard and the authenticated API answer on port
     80, the controller reports no certificate, and port 443 is closed.
  2. Uploading a certificate over HTTP is accepted and restarts the controller.
  3. HTTP no longer serves the UI or API: / and /api/* are 404 (not a
     redirect), and only /api/health is left.
  4. HTTPS works, with the uploaded certificate: the controller presents it,
     reports HTTPS active, and the authenticated and test APIs answer.

The certificate is a throwaway self-signed one generated here, so no real
private key is ever sent over plain HTTP. It is written to --cert-dir for
tests/device/test_https.py, which removes and restores it.

Env: ARCTIC_URL (only the host is used), ARCTIC_API_KEY, and GITHUB_ENV, to
which ARCTIC_URL, ARCTIC_ADMIN_CERT_FILE and ARCTIC_ADMIN_KEY_FILE are
appended for the later steps.
"""

import argparse
import datetime
import os
import socket
import ssl
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit

import requests
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

RESTART_TIMEOUT = 150.0


class CheckError(Exception):
    pass


def generate_cert(host: str) -> tuple[str, str]:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "arctic-ci")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name).issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=2))
        .add_extension(x509.SubjectAlternativeName([x509.DNSName(host)]), critical=False)
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


def presented_cert_der(host: str) -> bytes:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with socket.create_connection((host, 443), timeout=10) as sock:
        with ctx.wrap_socket(sock, server_hostname=host) as tls:
            return tls.getpeercert(binary_form=True)


class Device:
    def __init__(self, host: str, key: str):
        self.host = host
        self.http = f"http://{host}"
        self.https = f"https://{host}"
        self.s = requests.Session()
        self.s.verify = False
        self.s.headers["X-API-Key"] = key

    def req(self, method: str, url: str, **kw) -> requests.Response:
        kw.setdefault("timeout", 15)
        kw.setdefault("allow_redirects", False)
        return self.s.request(method, url, **kw)

    def get(self, url: str, **kw) -> requests.Response:
        return self.req("GET", url, **kw)

    def port_open(self, port: int) -> bool:
        try:
            with socket.create_connection((self.host, port), timeout=5):
                return True
        except OSError:
            return False


def expect(cond: bool, msg: str) -> None:
    if not cond:
        raise CheckError(msg)
    print(f"  ok: {msg}")


def check_http_mode(d: Device) -> int:
    print("HTTP mode")
    r = d.get(f"{d.http}/")
    expect(r.status_code == 200 and "<html" in r.text.lower(),
           f"the dashboard is served over HTTP (HTTP {r.status_code})")
    r = d.get(f"{d.http}/api/tls/status")
    expect(r.status_code == 200, f"the authenticated API answers over HTTP (HTTP {r.status_code})")
    tls = r.json()
    expect(tls.get("has_certs") is False and tls.get("https_active") is False,
           f"no administrator certificate and HTTPS inactive ({tls})")
    expect(not d.port_open(443), "port 443 is closed")
    health = d.get(f"{d.http}/api/health").json()
    return int(health.get("uptime_ms", 0))


def enable_auth(d: Device) -> None:
    cfg = d.get(f"{d.http}/api/auth/config").json()
    if not (cfg.get("web_auth_enabled") and cfg.get("api_auth_enabled")):
        r = d.req("POST", f"{d.http}/api/auth/config",
                  json={"web_auth_enabled": True, "api_auth_enabled": True})
        expect(r.status_code == 200, f"web and API auth enabled for the upload (HTTP {r.status_code})")


def upload(d: Device, cert: str, key: str) -> None:
    print("Certificate upload over HTTP")
    r = d.req("POST", f"{d.http}/api/tls/certificate", json={"cert": cert, "key": key})
    expect(r.status_code == 200, f"upload accepted (HTTP {r.status_code}: {r.text[:200]})")
    body = r.json()
    expect(body.get("restarting") is True and body.get("scheme") == "https",
           f"the controller restarts into HTTPS ({body})")


def wait_for_https(d: Device, uptime_before: int) -> None:
    deadline = time.time() + RESTART_TIMEOUT
    last = "no answer"
    while time.time() < deadline:
        try:
            r = d.get(f"{d.https}/api/health", timeout=5)
            if r.status_code == 200:
                uptime = int(r.json().get("uptime_ms", 0))
                print(f"  HTTPS answered (uptime {uptime} ms, was {uptime_before} ms before the upload)")
                expect(uptime < uptime_before, "the controller restarted")
                return
            last = f"HTTP {r.status_code}"
        except (requests.RequestException, ValueError) as e:
            last = type(e).__name__
        time.sleep(2)
    raise CheckError(f"HTTPS did not come up within {RESTART_TIMEOUT:.0f}s of the upload ({last})")


def check_https_mode(d: Device, cert_pem: str) -> None:
    print("HTTP after the upload")
    r = d.get(f"{d.http}/")
    expect(r.status_code == 404 and "Location" not in r.headers,
           f"/ over HTTP is a 404, not a redirect (HTTP {r.status_code})")
    r = d.get(f"{d.http}/api/ota/status")
    expect(r.status_code == 404, f"the API is not served over HTTP (HTTP {r.status_code})")
    r = d.get(f"{d.http}/api/health")
    expect(r.status_code == 200, f"/api/health is still served over HTTP (HTTP {r.status_code})")

    print("HTTPS mode")
    expect(presented_cert_der(d.host) == ssl.PEM_cert_to_DER_cert(cert_pem),
           "port 443 presents the uploaded certificate")
    r = d.get(f"{d.https}/api/tls/status")
    expect(r.status_code == 200, f"the authenticated API answers over HTTPS (HTTP {r.status_code})")
    tls = r.json()
    expect(tls.get("has_certs") is True and tls.get("https_active") is True,
           f"administrator certificate installed and HTTPS active ({tls})")
    r = d.get(f"{d.https}/")
    expect(r.status_code == 200 and "<html" in r.text.lower(),
           f"the dashboard is served over HTTPS (HTTP {r.status_code})")
    # The test endpoints register after the core routes; the suites need them.
    deadline = time.time() + 30
    while True:
        r = d.get(f"{d.https}/api/test/stack-watermarks")
        if r.status_code == 200 or time.time() > deadline:
            break
        time.sleep(2)
    expect(r.status_code == 200, f"the test endpoints answer over HTTPS (HTTP {r.status_code})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--cert-dir", required=True, type=Path)
    args = ap.parse_args()

    host = urlsplit(os.environ["ARCTIC_URL"]).netloc
    d = Device(host, os.environ["ARCTIC_API_KEY"])
    cert, key = generate_cert(host.split(":")[0])
    args.cert_dir.mkdir(parents=True, exist_ok=True)
    cert_file = args.cert_dir / "fullchain.pem"
    key_file = args.cert_dir / "privkey.pem"
    cert_file.write_text(cert)
    key_file.write_text(key)
    key_file.chmod(0o600)

    try:
        uptime = check_http_mode(d)
        enable_auth(d)
        upload(d, cert, key)
        wait_for_https(d, uptime)
        check_https_mode(d, cert)
    except (CheckError, requests.RequestException, ValueError, OSError) as e:
        print(f"::error::HTTP to HTTPS transition failed: {e}")
        return 1

    env = os.environ.get("GITHUB_ENV")
    if env:
        with open(env, "a", encoding="utf-8") as f:
            f.write(f"ARCTIC_URL={d.https}\n")
            f.write(f"ARCTIC_ADMIN_CERT_FILE={cert_file.resolve()}\n")
            f.write(f"ARCTIC_ADMIN_KEY_FILE={key_file.resolve()}\n")
    print(f"OK: HTTP mode, upload and HTTPS mode verified; the suites use {d.https}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
