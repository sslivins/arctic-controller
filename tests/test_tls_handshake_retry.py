"""TLS handshake timeouts are retried on a new connection (tests/tls_handshake_retry.py).

A local HTTPS server leaves the first few connections hanging mid-handshake --
what a stalled controller looks like to the client -- and serves later ones.
"""

import datetime
import socket
import ssl
import threading
import warnings

import pytest
import requests
import tls_handshake_retry
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

pytestmark = pytest.mark.hostside


@pytest.fixture(autouse=True)
def _quiet(monkeypatch):
    # These retries are deliberate: keep them out of CI annotations and the
    # end-of-run summary.
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    saved = list(tls_handshake_retry.HANDSHAKE_RETRIES)
    yield
    tls_handshake_retry.HANDSHAKE_RETRIES[:] = saved


def _server_context(tmp_path) -> ssl.SSLContext:
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "127.0.0.1")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(1)
            .not_valid_before(now).not_valid_after(now + datetime.timedelta(days=1))
            .sign(key, hashes.SHA256()))
    (tmp_path / "c.pem").write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (tmp_path / "k.pem").write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(tmp_path / "c.pem", tmp_path / "k.pem")
    return ctx


def _serve_one(ctx: ssl.SSLContext, conn: socket.socket) -> None:
    try:
        with ctx.wrap_socket(conn, server_side=True) as tls:
            data = b""
            while b"\r\n\r\n" not in data:
                data += tls.recv(4096)
            head, _, body = data.partition(b"\r\n\r\n")
            length = 0
            for line in head.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            while len(body) < length:
                body += tls.recv(4096)
            tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                        b"Connection: close\r\n\r\nok")
    except OSError:
        pass


@pytest.fixture
def stalling_server(tmp_path):
    """make(stall) starts a server whose first `stall` connections hang.
    Returns (url, accepted connections)."""
    ctx = _server_context(tmp_path)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    listener.settimeout(0.1)
    accepted: list[socket.socket] = []
    stop = threading.Event()
    threads: list[threading.Thread] = []

    def make(stall: int):
        def serve():
            while not stop.is_set():
                try:
                    conn, _ = listener.accept()
                except OSError:
                    continue
                accepted.append(conn)
                if len(accepted) > stall:
                    threading.Thread(target=_serve_one, args=(ctx, conn), daemon=True).start()

        threads.append(threading.Thread(target=serve, daemon=True))
        threads[0].start()
        return f"https://127.0.0.1:{listener.getsockname()[1]}/api/test/ha-token", accepted

    yield make
    stop.set()
    for t in threads:
        t.join()
    for conn in accepted:
        conn.close()
    listener.close()


def test_post_without_retries_gets_one_more_attempt(stalling_server):
    url, accepted = stalling_server(stall=1)
    with pytest.warns(UserWarning, match="TLS handshake timed out"):
        response = requests.post(url, json={}, timeout=0.5, verify=False)
    assert response.text == "ok"
    assert len(accepted) == 2
    assert tls_handshake_retry.HANDSHAKE_RETRIES[-1] == f"POST {url}"


def test_post_uses_the_sessions_connect_retries(stalling_server):
    # Retry(total=2) never retries a POST read timeout; the handshake timeout
    # must be retried as a connect error instead (the nightly failure).
    url, accepted = stalling_server(stall=2)
    session = requests.Session()
    session.mount("https://", HTTPAdapter(max_retries=Retry(total=2, backoff_factor=0)))
    with pytest.warns(UserWarning, match="TLS handshake timed out") as record:
        response = session.post(url, json={}, timeout=0.5, verify=False)
    assert response.text == "ok"
    assert len(accepted) == 3
    assert len([w for w in record if "TLS handshake" in str(w.message)]) == 2


def test_failed_retry_raises_without_reporting(stalling_server):
    # A device that is rebooting fails the retry too; callers waiting for the
    # reboot expect that, so it must not be reported as a rescued stall.
    url, accepted = stalling_server(stall=99)
    before = len(tls_handshake_retry.HANDSHAKE_RETRIES)
    with warnings.catch_warnings():
        warnings.filterwarnings("error", message="TLS handshake")
        with pytest.raises(requests.exceptions.ConnectTimeout):
            requests.post(url, json={}, timeout=0.3, verify=False)
    assert len(accepted) == 2
    assert len(tls_handshake_retry.HANDSHAKE_RETRIES) == before


def test_other_timeouts_are_not_retried():
    assert not tls_handshake_retry.is_tls_handshake_timeout(
        requests.exceptions.ReadTimeout("Read timed out. (read timeout=10)"))


def test_opt_out_lets_the_stall_fail(stalling_server):
    # A probe waiting for the device to drop off WiFi must see the failure;
    # a rescued retry hid the drop (PR #408 CI).
    url, accepted = stalling_server(stall=1)
    with tls_handshake_retry.no_handshake_retry():
        with pytest.raises(requests.exceptions.RequestException):
            requests.get(url, timeout=0.3, verify=False)
    assert len(accepted) == 1
    assert requests.get(url, timeout=2, verify=False).text == "ok"
