"""
Functional tests for request bodies the device does not expect: chunked
transfer encoding, oversized bodies, and truncated uploads.

Why this exists: every handler reads its body via req->content_len (see
read_integration_body in main/api_server.cpp). esp_http_server does not
support chunked request bodies, so a client that uses "Transfer-Encoding:
chunked" -- which several HTTP libraries do by default when the body is a
stream -- presents as content_len 0. The device must answer that with a
definite error and stay serving. What it must never do is hang the request,
crash, or leak the socket: the API server runs with max_open_sockets = 2, so
two leaked sockets take the whole API down until reboot.

Nothing here asserts that chunked encoding WORKS. It asserts the device
degrades safely when it is used, which is the property that keeps a
misconfigured client from bricking access to the controller.

Refs #217 (T18).
"""

import os
import time

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

# A POST endpoint that changes nothing when the body is unusable.
TARGET = "/api/auth/config"


def _headers(extra=None):
    h = {"Connection": "close"}
    if API_KEY:
        h["X-API-Key"] = API_KEY
    if extra:
        h.update(extra)
    return h


def _health_recovers(attempts=10):
    """The device must still answer after each malformed request.

    Retried rather than checked once: the socket the previous request used may
    take a moment to be reclaimed, and a slow recovery is not the failure this
    file is looking for. A permanent one is.
    """
    last = None
    for _ in range(attempts):
        try:
            resp = requests.get(
                f"{BASE_URL}/api/health", headers=_headers(), timeout=10,
                verify=False,
            )
            if resp.status_code == 200:
                return True
            last = resp.status_code
        except requests.RequestException as exc:
            last = repr(exc)
        time.sleep(2)
    print(f"health never recovered, last result: {last}")
    return False


def _chunked(body_parts):
    """requests sends Transfer-Encoding: chunked for a generator body."""
    def gen():
        for part in body_parts:
            yield part
    return gen()


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_a_chunked_body_is_answered_rather_than_hanging():
    resp = requests.post(
        f"{BASE_URL}{TARGET}",
        headers=_headers({"Content-Type": "application/json"}),
        data=_chunked([b'{"web_auth_enabled":', b" true}"]),
        timeout=20,
        verify=False,
    )
    # Any definite status is acceptable; the device is entitled not to support
    # chunked requests. Silence, a hang, or a 5xx crash page is not.
    assert 400 <= resp.status_code < 500, (
        f"expected a 4xx for an unsupported chunked body, got "
        f"{resp.status_code}: {resp.text[:300]}"
    )
    assert _health_recovers()


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_an_empty_chunked_body_is_answered():
    resp = requests.post(
        f"{BASE_URL}{TARGET}",
        headers=_headers({"Content-Type": "application/json"}),
        data=_chunked([]),
        timeout=20,
        verify=False,
    )
    assert 400 <= resp.status_code < 500, resp.text[:300]
    assert _health_recovers()


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_a_body_larger_than_any_handler_buffer_is_refused():
    # Well past every read_*_body capacity in api_server.cpp. The handler must
    # refuse it on the declared length rather than reading it into a fixed
    # buffer.
    payload = '{"web_auth_enabled": true, "pad": "' + ("A" * 200000) + '"}'
    resp = requests.post(
        f"{BASE_URL}{TARGET}",
        headers=_headers({"Content-Type": "application/json"}),
        data=payload.encode(),
        timeout=30,
        verify=False,
    )
    assert 400 <= resp.status_code < 500, (
        f"expected a 4xx for an oversized body, got {resp.status_code}: "
        f"{resp.text[:300]}"
    )
    assert _health_recovers()


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
def test_a_truncated_upload_does_not_hold_the_socket_forever():
    """Promise more body than is sent, then hang up.

    This is the case that leaks a socket: the handler is blocked in
    httpd_req_recv waiting for bytes that will never arrive. With
    max_open_sockets = 2, two of these would take the API down until reboot,
    so the recovery check below is the whole point of the test.
    """
    import socket
    from urllib.parse import urlparse

    parsed = urlparse(BASE_URL)
    host = parsed.hostname
    port = parsed.port or (443 if parsed.scheme == "https" else 80)

    request = (
        f"POST {TARGET} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"X-API-Key: {API_KEY}\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 5000\r\n"
        "Connection: close\r\n"
        "\r\n"
        '{"web_auth_enabled": true'
    ).encode()

    sock = socket.create_connection((host, port), timeout=10)
    try:
        if parsed.scheme == "https":
            import ssl

            ctx = ssl._create_unverified_context()
            sock = ctx.wrap_socket(sock, server_hostname=host)
        sock.sendall(request)
    finally:
        try:
            sock.close()
        except OSError:
            pass

    assert _health_recovers(attempts=20), (
        "The API stopped answering after a truncated upload. A socket is "
        "almost certainly still held by a handler blocked in httpd_req_recv."
    )
