"""
Requests that http_parser rejects before any URI handler runs.

esp_http_server answers those with HTTPD_400_BAD_REQUEST. Its built-in response
is a text/html "Bad request syntax" page, but docs/openapi.yaml documents a
JSON BadRequest on every operation, so the firmware registers
http_bad_request_handler (main/api_server.cpp) to keep that contract. The
connection must then be closed, because parsing stopped part way through the
request and the bytes after it cannot be trusted to start a new one.

The requests are hand-written on a raw socket: HTTP client libraries refuse to
send these bytes at all.

Prerequisites:
  - Device reachable at ARCTIC_URL (default http://arctic.local)
"""

import json
import os
import socket
import ssl
from pathlib import Path
from urllib.parse import urlparse

import pytest
import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

_env_file = Path(__file__).resolve().parent.parent.parent / ".env"
if _env_file.exists():
    from dotenv import load_dotenv
    load_dotenv(_env_file)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")

# Each is rejected by the stock ESP-IDF http_parser as well as by the fixed one
# in espressif/esp-idf#19140. A CTL as the *second* byte of a header value is
# the one position the stock parser always checks.
MALFORMED_HEADERS = {
    "ctl_in_header_value": b"X-Probe: a\x15b\r\n",
    "nul_in_header_value": b"X-Probe: a\x00b\r\n",
    "ctl_in_header_name": b"X-Pr\x01be: value\r\n",
    "space_in_header_name": b"X Probe: value\r\n",
}


def _connect():
    parsed = urlparse(BASE_URL)
    host = parsed.hostname
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError as exc:
        pytest.skip(f"Device unreachable at {BASE_URL}: {exc}")
    if parsed.scheme == "https":
        sock = ssl._create_unverified_context().wrap_socket(
            sock, server_hostname=host)
    return sock, host


def _send_raw(header_line: bytes):
    """Send one malformed GET and read until the device closes the socket.

    Returns (status, headers, body, closed). closed is False if the device
    kept the connection open past the read timeout.
    """
    sock, host = _connect()
    request = (
        b"GET /api/health HTTP/1.1\r\n"
        b"Host: " + host.encode() + b"\r\n"
        + header_line
        + b"\r\n"
    )
    data = b""
    closed = False
    try:
        sock.sendall(request)
        sock.settimeout(5)
        while True:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                break
            except (ConnectionResetError, ssl.SSLError):
                closed = True
                break
            if not chunk:
                closed = True
                break
            data += chunk
    finally:
        sock.close()

    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1]) if lines and lines[0] else None
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    return status, headers, body, closed


def _health_ok():
    # Retries with backoff cover the moment the closed socket takes to be
    # reclaimed; a device that stays down still fails.
    session = requests.Session()
    retry = Retry(total=8, connect=8, read=8, backoff_factor=0.5,
                  allowed_methods=None, status_forcelist=[502, 503, 504])
    session.mount("http://", HTTPAdapter(max_retries=retry))
    session.mount("https://", HTTPAdapter(max_retries=retry))
    try:
        r = session.get(f"{BASE_URL}/api/health", timeout=5, verify=False,
                        headers={"Connection": "close"})
        return r.status_code == 200
    except requests.RequestException:
        return False
    finally:
        session.close()


@pytest.mark.parametrize("header_line", MALFORMED_HEADERS.values(),
                         ids=MALFORMED_HEADERS.keys())
def test_parser_rejection_is_json_400_and_closes(header_line):
    status, headers, body, closed = _send_raw(header_line)

    assert status == 400, f"expected 400, got {status}: {body[:200]!r}"
    assert headers.get("content-type", "").startswith("application/json"), (
        f"parser 400 must match the documented JSON BadRequest, got "
        f"Content-Type {headers.get('content-type')!r}: {body[:200]!r}"
    )
    payload = json.loads(body)
    assert isinstance(payload.get("error"), str) and payload["error"]
    assert headers.get("connection", "").lower() == "close"
    assert closed, "device must close the connection after a parser error"
    assert _health_ok(), "device stopped answering after a malformed request"
