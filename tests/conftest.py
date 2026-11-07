"""Hooks shared by every test suite under tests/."""

import tls_handshake_retry  # patches requests/urllib3 on import


def pytest_terminal_summary(terminalreporter):
    if tls_handshake_retry.HANDSHAKE_RETRIES:
        terminalreporter.write_line(
            f"{len(tls_handshake_retry.HANDSHAKE_RETRIES)} request(s) retried after a TLS handshake "
            "timeout (see warnings); the controller log names what stalled it.",
            yellow=True)
