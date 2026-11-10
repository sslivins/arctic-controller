"""Retry controller requests whose TLS handshake timed out (see tests/conftest.py).

TLS handshake timeouts
----------------------
The controller serves HTTPS from one thread, so a handshake occasionally
waits behind other work (see main/httpd_watch.cpp, which logs what the thread
was doing). urllib3 reports a handshake that times out as a *read* timeout,
and requests never retries a read timeout on POST/PATCH/DELETE -- yet nothing
was sent: the request had not left the client. These hooks treat it as the
connect failure it really is:

* a Retry policy with connect retries left retries it like any connect error;
* a session with no connect retries (the requests default) gets one more
  attempt.

The controller also bounds handshakes (main/api_server.cpp) and closes a
client that falls silent mid-handshake, so a handshake the server *dropped* is
retried the same way.

Each retry that rescues a request is reported as a pytest warning and, in CI,
a GitHub annotation, so stalls stay visible instead of being silently absorbed.

Probes that wait for the controller to become unreachable (WiFi bounce,
server stop) must not be rescued: wrap them in ``no_handshake_retry()``.
"""

from __future__ import annotations

import contextlib
import os
import ssl
import sys
import threading
import warnings

try:
    import requests.adapters
    from urllib3.exceptions import (ConnectTimeoutError, ProtocolError,
                                    ReadTimeoutError, SSLError)
    from urllib3.util import retry as _urllib3_retry
except ImportError:  # pragma: no cover - suites that do not use requests
    requests = None

HANDSHAKE_RETRIES: list[str] = []
_MARK = "_arctic_tls_handshake_timeout"
_local = threading.local()


@contextlib.contextmanager
def no_handshake_retry():
    """Let handshake timeouts in this thread fail as they normally would."""
    prev = getattr(_local, "disabled", False)
    _local.disabled = True
    try:
        yield
    finally:
        _local.disabled = prev


def _enabled() -> bool:
    return not getattr(_local, "disabled", False)


def _chain(exc: BaseException | None):
    """Yield exc and everything it wraps (context, cause, urllib3 reasons)."""
    seen: set[int] = set()
    stack = [exc]
    while stack:
        cur = stack.pop()
        # urllib3's SSLError.reason can be a plain string.
        if not isinstance(cur, BaseException) or id(cur) in seen:
            continue
        seen.add(id(cur))
        yield cur
        stack.extend([cur.__context__, cur.__cause__, getattr(cur, "reason", None)])
        stack.extend(a for a in getattr(cur, "args", ()) if isinstance(a, BaseException))


def is_tls_handshake_timeout(exc: BaseException | None) -> bool:
    """True if exc is (or wraps) a timeout during the TLS handshake."""
    for e in _chain(exc):
        if getattr(e, _MARK, False):
            return True
        if isinstance(e, TimeoutError) and "handshake" in str(e):
            return True
    return False


_HANDSHAKE_FRAMES = {"do_handshake", "wrap_socket", "ssl_wrap_socket"}


def _dropped_in_handshake(exc: BaseException) -> bool:
    """The server closed the connection before the handshake finished."""
    if not isinstance(exc, (ssl.SSLEOFError, ssl.SSLZeroReturnError, ConnectionResetError)):
        return False
    tb = exc.__traceback__
    while tb is not None:
        if tb.tb_frame.f_code.co_name in _HANDSHAKE_FRAMES:
            return True
        tb = tb.tb_next
    return False


def is_tls_handshake_failure(exc: BaseException | None) -> bool:
    """True if exc is (or wraps) a handshake that timed out or that the
    controller dropped. It bounds handshakes (main/api_server.cpp), so a client
    held up on the WiFi gets closed rather than waited for. Either way the
    request was never sent."""
    if is_tls_handshake_timeout(exc):
        return True
    return any(_dropped_in_handshake(e) for e in _chain(exc))


def report_handshake_retry(what: str) -> None:
    msg = (f"TLS handshake timed out or was dropped; the retry on a new "
           f"connection succeeded: {what}")
    HANDSHAKE_RETRIES.append(what)
    warnings.warn(msg)
    if os.environ.get("GITHUB_ACTIONS"):
        # sys.__stdout__ bypasses pytest's capture so the runner sees it.
        sys.__stdout__.write(f"::warning title=Controller TLS handshake timeout::{msg}\n")
        sys.__stdout__.flush()


if requests is not None:
    _orig_increment = _urllib3_retry.Retry.increment
    _orig_send = requests.adapters.HTTPAdapter.send
    # Retries made during the current send(). They are reported only if the
    # request then succeeds: a failure surfaces as an exception anyway, and
    # code waiting for a reboot expects handshakes to fail.
    def _pending() -> list[str]:
        if not hasattr(_local, "pending"):
            _local.pending = []
        return _local.pending

    def _increment(self, method=None, url=None, response=None, error=None,
                   _pool=None, _stacktrace=None):
        if (_enabled() and isinstance(error, (ReadTimeoutError, SSLError, ProtocolError))
                and is_tls_handshake_failure(error)):
            converted = ConnectTimeoutError(f"TLS handshake failed: {error}")
            setattr(converted, _MARK, True)
            # Raises when no connect retries are left.
            new_retry = _orig_increment(self, method, url, response, converted,
                                        _pool, _stacktrace)
            _pending().append(f"{method} {url}")
            return new_retry
        return _orig_increment(self, method, url, response, error, _pool, _stacktrace)

    def _send(self, request, *args, **kwargs):
        pending = _pending()
        pending.clear()
        try:
            try:
                response = _orig_send(self, request, *args, **kwargs)
            except requests.exceptions.RequestException as exc:
                if (not _enabled() or not is_tls_handshake_failure(exc)
                        or _has_connect_retries(self.max_retries)):
                    raise
                # Nothing was sent, so even a non-idempotent request is safe
                # to repeat. Only once: a second stall is a real failure.
                pending.append(f"{request.method} {request.url}")
                response = _orig_send(self, request, *args, **kwargs)
            for what in pending:
                report_handshake_retry(what)
            return response
        finally:
            pending.clear()

    def _has_connect_retries(retry) -> bool:
        """The policy already retries connect failures (see _increment)."""
        budget = retry.connect if retry.connect is not None else retry.total
        return budget is None or (budget is not False and budget > 0)

    _urllib3_retry.Retry.increment = _increment
    requests.adapters.HTTPAdapter.send = _send