"""
Functional tests for POST /api/tls/certificate input validation.

Why this exists: the endpoint used to accept anything containing the strings
"-----BEGIN CERTIFICATE-----" and "PRIVATE KEY-----". Base64 garbage between
those markers was written to NVS and only failed on the NEXT BOOT, when the
HTTPS server refused to start -- taking the web UI down, with no way to reach
the device over HTTPS to undo it. The certificate is now parsed before it is
stored, so bad input is a 400 the user can see and act on.

Every case here is REJECTED input. Nothing in this file ever stores a
certificate: a test that replaced the identity of the shared CI controller
would break every later HTTPS test in the run.

Prerequisites:
  - Device reachable at ARCTIC_URL
  - ARCTIC_API_KEY env var set

Refs #217 (T07).
"""

import base64
import os

import pytest
import requests
import urllib3
from pathlib import Path
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

_session = requests.Session()
_retry = Retry(total=3, backoff_factor=1, allowed_methods=None,
               status_forcelist=[502, 503, 504])
_session.mount("http://", HTTPAdapter(max_retries=_retry))
_session.mount("https://", HTTPAdapter(max_retries=_retry))
_session.verify = False


@pytest.fixture(scope="module", autouse=True)
def _release_keepalive_socket():
    """Close the module Session once this file is done.

    requests keeps the TLS connection alive after the last test, so an unclosed
    module-level Session holds one of the device's sockets for the whole pytest
    run. The HTTPS server allows max_open_sockets = 7 and each live connection
    also pins a TCP PCB in internal RAM, which is scarce and fragments (#234).
    Leaking one here made a later test's 8 KB OTA task allocation fail.
    """
    yield
    _session.close()


ENDPOINT = "/api/tls/certificate"

# PEM-shaped and completely meaningless: the markers are right, the payload is
# not a DER structure at all. This is the case the old marker check accepted.
GARBAGE_BODY = base64.b64encode(b"this is not a certificate" * 8).decode()
PEM_SHAPED_GARBAGE = (
    "-----BEGIN CERTIFICATE-----\n"
    + "\n".join(GARBAGE_BODY[i:i + 64] for i in range(0, len(GARBAGE_BODY), 64))
    + "\n-----END CERTIFICATE-----\n"
)

# A syntactically plausible key. It is never stored, because the certificate is
# rejected first; it exists so the request fails for the reason under test
# rather than for a missing field.
PEM_SHAPED_KEY = (
    "-----BEGIN PRIVATE KEY-----\n"
    + "\n".join(GARBAGE_BODY[i:i + 64] for i in range(0, len(GARBAGE_BODY), 64))
    + "\n-----END PRIVATE KEY-----\n"
)

# Truncated mid-body: valid header and footer, incomplete base64.
TRUNCATED_CERT = (
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBkTCB+wIJAJ\n"
    "-----END CERTIFICATE-----\n"
)

# Header and footer that do not agree.
MISMATCHED_MARKERS = (
    "-----BEGIN CERTIFICATE-----\n"
    + GARBAGE_BODY
    + "\n-----END PRIVATE KEY-----\n"
)


def _headers():
    h = {}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    return h


def _post_cert(cert, key=PEM_SHAPED_KEY):
    return _session.post(
        f"{BASE_URL}{ENDPOINT}",
        headers=_headers(),
        json={"cert": cert, "key": key},
        timeout=15,
    )


def _https_still_works():
    """The device must still be serving after every rejected request."""
    resp = _session.get(f"{BASE_URL}/api/health", headers=_headers(), timeout=10)
    return resp.status_code == 200


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
@pytest.mark.parametrize(
    "name,cert",
    [
        ("pem shaped garbage", PEM_SHAPED_GARBAGE),
        ("truncated body", TRUNCATED_CERT),
        ("mismatched markers", MISMATCHED_MARKERS),
    ],
    ids=["pem_shaped_garbage", "truncated_body", "mismatched_markers"],
)
def test_an_unparseable_certificate_is_rejected(name, cert):
    resp = _post_cert(cert)
    assert resp.status_code == 400, (
        f"{name}: expected 400, got {resp.status_code}: {resp.text[:300]}. "
        "An unparseable certificate that is accepted here breaks HTTPS on the "
        "next boot, when it can no longer be fixed over the network."
    )
    # A 500 would mean it reached the storage layer; a 400 means it was
    # recognised as bad input.
    assert "cert" in resp.text.lower()


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_text_that_is_not_pem_at_all_is_rejected():
    resp = _post_cert("hello, I am a certificate, honest")
    assert resp.status_code == 400, resp.text[:300]


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_missing_fields_are_rejected():
    resp = _session.post(
        f"{BASE_URL}{ENDPOINT}", headers=_headers(), json={"cert": PEM_SHAPED_GARBAGE},
        timeout=15,
    )
    assert resp.status_code == 400, resp.text[:300]


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_an_oversized_certificate_is_rejected_without_wedging_the_device():
    # TLS_MAX_CERT_LEN is 4000; go well past it.
    huge = (
        "-----BEGIN CERTIFICATE-----\n" + ("A" * 8000) + "\n-----END CERTIFICATE-----\n"
    )
    resp = _post_cert(huge)
    assert resp.status_code == 400, resp.text[:300]
    assert _https_still_works(), (
        "The device stopped answering after an oversized certificate was "
        "rejected."
    )


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_rejected_certificates_leave_the_device_serving():
    for cert in (PEM_SHAPED_GARBAGE, TRUNCATED_CERT, "not pem"):
        _post_cert(cert)
    assert _https_still_works()
