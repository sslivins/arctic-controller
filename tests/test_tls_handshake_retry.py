"""TLS handshake timeouts are retried on a new connection (tests/tls_handshake_retry.py).

A local listener accepts TCP connections and then says nothing, so every TLS
handshake times out -- what a stalled controller looks like to the client.
"""

import socket
import threading

import pytest
import requests
import tls_handshake_retry
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


@pytest.fixture
def silent_server():
    """Accepts connections and never answers. Yields (url, accepted list)."""
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    listener.settimeout(0.1)
    accepted: list[socket.socket] = []
    stop = threading.Event()

    def serve():
        while not stop.is_set():
            try:
                conn, _ = listener.accept()
            except OSError:
                continue
            accepted.append(conn)

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    yield f"https://127.0.0.1:{listener.getsockname()[1]}/api/test/ha-token", accepted
    stop.set()
    thread.join()
    for conn in accepted:
        conn.close()
    listener.close()


def test_post_without_retries_gets_one_more_attempt(silent_server):
    url, accepted = silent_server
    before = len(tls_handshake_retry.HANDSHAKE_RETRIES)
    with (pytest.warns(UserWarning, match="TLS handshake timed out"),
          pytest.raises(requests.exceptions.ConnectTimeout)):
        requests.post(url, json={}, timeout=0.3, verify=False)
    assert len(accepted) == 2
    assert len(tls_handshake_retry.HANDSHAKE_RETRIES) == before + 1


def test_post_uses_the_sessions_connect_retries(silent_server):
    # Retry(total=2) never retries a POST read timeout; the handshake timeout
    # must be retried as a connect error instead (the nightly failure).
    url, accepted = silent_server
    session = requests.Session()
    session.mount("https://", HTTPAdapter(max_retries=Retry(total=2, backoff_factor=0)))
    with (pytest.warns(UserWarning, match="TLS handshake timed out"),
          pytest.raises(requests.exceptions.ConnectTimeout)):
        session.post(url, json={}, timeout=0.3, verify=False)
    assert len(accepted) == 3


def test_other_timeouts_are_not_retried():
    assert not tls_handshake_retry.is_tls_handshake_timeout(
        requests.exceptions.ReadTimeout("Read timed out. (read timeout=10)"))
