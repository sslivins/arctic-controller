"""
Connection-lifetime contract for HTTP error responses.

An error response must not cost the client its connection. This is not a
cosmetic concern on this device -- it is issue #234.

esp_http_server's default behaviour, when no handler is registered for an
error code, is to send the response and then close the socket. Its own source
says so (components/esp_http_server/src/httpd_txrx.c): "If no handler is
registered for this error default behavior is to send the HTTP error response
and return failure for closure of underlying socket".

On a TLS server that means every wrong-method request forces a fresh session
handshake for the next one, and leaves a device-side TIME_WAIT PCB behind.
The API schema fuzz walks every endpoint with every method, which ran this at
roughly five handshakes per second for minutes. Internal (non-PSRAM) RAM is
the scarce resource on this part, and it drained until mDNS could not
allocate its 176-byte receive buffer. mDNS receives on the lwIP tcpip thread,
so that starvation stalled the thread itself, and the device stopped
completing TCP handshakes -- surfacing as ConnectTimeout in unrelated suites.

These tests use http.client rather than requests deliberately. requests
transparently opens a replacement connection when the server closes one, which
would hide exactly the regression being guarded here.

Prerequisites:
  - Device reachable at ARCTIC_URL
  - ARCTIC_API_KEY env var set
"""

import http.client
import os
import ssl
from pathlib import Path
from urllib.parse import urlparse

import pytest

_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# A URI that certainly exists and is GET-only, so a non-GET yields 405 (URI
# matched, method did not) rather than 404 (URI never matched).
PROBE_PATH = "/api/health"
WRONG_METHOD = "DELETE"


def _connect():
    """Open a single connection we control the lifetime of."""
    parsed = urlparse(BASE_URL)
    host = parsed.hostname
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    if parsed.scheme == "https":
        ctx = ssl._create_unverified_context()
        return http.client.HTTPSConnection(host, port, timeout=10, context=ctx)
    return http.client.HTTPConnection(host, port, timeout=10)


def _headers():
    return {"X-API-Key": API_KEY} if API_KEY else {}


def _request(conn, method, path):
    """Issue one request and fully drain it so the connection stays reusable."""
    conn.request(method, path, headers=_headers())
    resp = conn.getresponse()
    resp.read()
    return resp.status


@pytest.fixture
def conn():
    try:
        c = _connect()
        c.connect()
    except OSError as exc:
        pytest.skip(f"Device unreachable at {BASE_URL}: {exc}")
    yield c
    c.close()


def test_wrong_method_still_answers_405(conn):
    """Guard the premise: the probe really does produce a 405, not a 404."""
    status = _request(conn, WRONG_METHOD, PROBE_PATH)
    assert status == 405, (
        f"{WRONG_METHOD} {PROBE_PATH} returned {status}, not 405. This test "
        "cannot measure 405 connection handling if the probe no longer "
        "produces one -- pick a different URI/method pair."
    )


def test_405_does_not_close_the_connection(conn):
    """The connection must survive a 405 and serve the next request.

    Without a registered HTTPD_405_METHOD_NOT_ALLOWED handler that returns
    ESP_OK, the device closes the socket after the 405 and this second request
    fails on the dead connection.
    """
    assert _request(conn, WRONG_METHOD, PROBE_PATH) == 405

    try:
        status = _request(conn, "GET", PROBE_PATH)
    except (http.client.HTTPException, OSError) as exc:
        pytest.fail(
            f"The device closed the connection after a 405: reusing it raised "
            f"{type(exc).__name__}: {exc}. A 405 handler returning ESP_OK is "
            f"missing, so every wrong-method request now costs a full TLS "
            f"handshake and a TIME_WAIT PCB. See issue #234."
        )

    assert status == 200, (
        f"connection survived the 405 but the follow-up GET returned {status}"
    )


def test_repeated_405s_do_not_churn_connections(conn):
    """The fuzz sends thousands of these; one connection must absorb them.

    This is the shape that actually exhausted internal RAM: not a single 405,
    but a sustained stream of them each forcing a new TLS session.
    """
    for i in range(20):
        try:
            status = _request(conn, WRONG_METHOD, PROBE_PATH)
        except (http.client.HTTPException, OSError) as exc:
            pytest.fail(
                f"Connection died after {i} consecutive 405s "
                f"({type(exc).__name__}: {exc}). Each wrong-method request is "
                f"tearing down the connection, which is the internal-RAM "
                f"drain behind issue #234."
            )
        assert status == 405, f"request {i} returned {status}, expected 405"

    # And the connection is still usable for real traffic afterwards.
    assert _request(conn, "GET", PROBE_PATH) == 200
