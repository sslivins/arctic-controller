"""Fixtures for the Home Assistant end-to-end suite.

hass-macon (from its main branch) runs inside Home Assistant's test harness
and talks to the bench controller, which drives the arctic-simulator over
RS-485 exactly as in tests/rs485. The bench fixtures (simulator lease, demo
mode off for the session, idle preset before every test) are the RS-485
suite's own, loaded from tests/rs485/conftest.py.

This suite needs Python 3.13 with hass-macon's test dependencies, so it runs
from its own virtualenv in the nightly device-tests run. Anywhere else (the
host-side PR gate collects all of tests/) it is left out of collection.

Env (as for tests/rs485):
  ARCTIC_URL       controller base URL
  ARCTIC_API_KEY   controller API key
  ARCTIC_SIM_URL   simulator base URL
  PYTHONPATH       must include a hass-macon checkout (for custom_components)
"""

import importlib.util
from pathlib import Path

try:
    import pytest_homeassistant_custom_component  # noqa: F401
except ImportError:
    collect_ignore_glob = ["test_*.py"]
else:
    import pytest
    import pytest_socket

    # Home Assistant's harness blocks every connection that isn't to 127.0.0.1
    # before each test. This suite exists to talk to the bench, so turn that off.
    pytest_socket.socket_allow_hosts = lambda *args, **kwargs: None
    pytest_socket.disable_socket = lambda *args, **kwargs: None

    _spec = importlib.util.spec_from_file_location(
        "rs485_bench", Path(__file__).resolve().parents[1] / "rs485" / "conftest.py"
    )
    rs485_bench = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(rs485_bench)

    # Re-exported so pytest registers them for this directory.
    sim = rs485_bench.sim
    device = rs485_bench.device
    fresh_bus_state = rs485_bench.fresh_bus_state

    @pytest.fixture(autouse=True)
    def auto_enable_custom_integrations(enable_custom_integrations):
        yield
