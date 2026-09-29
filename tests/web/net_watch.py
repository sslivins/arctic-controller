"""Tell a network outage apart from a dashboard bug during web tests.

A dashboard load that times out usually means the page is broken. But the bench
controller sits on real Wi-Fi, and a short outage (for example the ~12 s one in
nightly run 36566212582, where the device logged "gateway unreachable" while it
stayed up) makes the load time out just the same.

``NetworkWatch`` pings the device once a second while a page loads. If the
load fails AND the device stopped answering pings during that window, the
failure was the network, not the firmware, and the caller may wait for the
network to come back and retry once. If every ping was answered, the device
was reachable the whole time and the failure is real.

ICMP is used rather than HTTP so the watch adds no TLS or web-server load to
the device while it is being tested. This module must not import Playwright:
the host-side test suite loads it directly.
"""

from __future__ import annotations

import platform
import shutil
import subprocess
import threading
import time
from typing import Callable, Optional

Pinger = Callable[[str], bool]

# One lost ping can happen on a healthy link. Require more than that before
# blaming the network, so a real dashboard bug is not retried away by chance.
MIN_MISSED_FOR_OUTAGE = 2


def system_ping(host: str, timeout_s: int = 1) -> bool:
    """Send one ICMP echo with the system ``ping``; True if it was answered."""
    if platform.system() == "Windows":
        cmd = ["ping", "-n", "1", "-w", str(timeout_s * 1000), host]
    else:
        cmd = ["ping", "-c", "1", "-W", str(timeout_s), host]
    try:
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=timeout_s + 2)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0


class NetworkWatch:
    """Context manager that pings ``host`` in the background while active."""

    def __init__(self, host: Optional[str], interval_s: float = 1.0,
                 pinger: Optional[Pinger] = None):
        self.host = host
        self.interval_s = interval_s
        self._ping = pinger or system_ping
        self.available = bool(host) and (pinger is not None
                                         or shutil.which("ping") is not None)
        self.sent = 0
        self.missed = 0
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    @property
    def saw_outage(self) -> bool:
        return self.missed >= MIN_MISSED_FOR_OUTAGE

    def probe(self) -> bool:
        """Send one ping and count the result."""
        ok = self._ping(self.host)
        self.sent += 1
        if not ok:
            self.missed += 1
        return ok

    def _run(self) -> None:
        while not self._stop.is_set():
            self.probe()
            self._stop.wait(self.interval_s)

    def __enter__(self) -> "NetworkWatch":
        if self.available:
            self._thread = threading.Thread(target=self._run, daemon=True,
                                            name="net-watch")
            self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)


def wait_until_reachable(host: str, timeout_s: float = 60.0,
                         pinger: Optional[Pinger] = None,
                         needed: int = 3,
                         sleep: Callable[[float], None] = time.sleep,
                         clock: Callable[[], float] = time.monotonic) -> Optional[float]:
    """Wait for ``needed`` consecutive answered pings.

    Returns the seconds waited, or None if the device did not come back within
    ``timeout_s``.
    """
    ping = pinger or system_ping
    start = clock()
    streak = 0
    while clock() - start < timeout_s:
        if ping(host):
            streak += 1
            if streak >= needed:
                return clock() - start
        else:
            streak = 0
        sleep(1.0)
    return None
