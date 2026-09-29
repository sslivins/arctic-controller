"""Host-side tests for tests/web/net_watch.py.

The web fixture retries a failed dashboard load only when the device stopped
answering pings during the load. These tests pin that decision so a real
dashboard failure can never be retried away.
"""

import importlib.util
import itertools
from pathlib import Path

import pytest

pytestmark = pytest.mark.hostside

_spec = importlib.util.spec_from_file_location(
    "net_watch", Path(__file__).parent / "web" / "net_watch.py")
net_watch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(net_watch)


def _scripted(results):
    """A pinger that replays ``results`` and then keeps answering."""
    it = iter(results)
    return lambda host: next(it, True)


def _watch_for(results):
    w = net_watch.NetworkWatch("dev", interval_s=0, pinger=_scripted(results))
    for _ in results:
        w.probe()
    return w


def test_all_pings_answered_is_not_an_outage():
    assert not _watch_for([True] * 10).saw_outage


def test_single_lost_ping_is_not_an_outage():
    """One lost packet on a healthy link must not excuse a real failure."""
    assert not _watch_for([True, False, True, True]).saw_outage


def test_run_of_missed_pings_is_an_outage():
    w = _watch_for([True, False, False, False, True])
    assert w.saw_outage
    assert (w.missed, w.sent) == (3, 5)


def test_background_thread_counts_pings():
    import threading
    fifth = threading.Event()
    calls = []

    def pinger(host):
        calls.append(host)
        if len(calls) >= 5:
            fifth.set()
        return len(calls) > 3

    w = net_watch.NetworkWatch("dev", interval_s=0, pinger=pinger)
    with w:
        assert fifth.wait(timeout=5), "watch thread never pinged"
    assert w.sent >= 5
    assert w.missed == 3
    assert w.saw_outage


def test_without_ping_the_watch_is_inert():
    w = net_watch.NetworkWatch(None)
    assert not w.available
    with w:
        pass
    assert w.sent == 0 and not w.saw_outage


def _fake_clock():
    t = itertools.count()
    return lambda: float(next(t)), (lambda s: None)


def test_wait_until_reachable_needs_consecutive_answers():
    clock, sleep = _fake_clock()
    pings = _scripted([False, True, False, True, True, True])
    waited = net_watch.wait_until_reachable("dev", timeout_s=60, pinger=pings,
                                            sleep=sleep, clock=clock)
    assert waited is not None


def test_wait_until_reachable_gives_up():
    clock, sleep = _fake_clock()
    waited = net_watch.wait_until_reachable("dev", timeout_s=10,
                                            pinger=lambda h: False,
                                            sleep=sleep, clock=clock)
    assert waited is None
