"""Home Assistant, the bench controller and the simulated heat pump, end to end.

The Home Assistant integration's own tests use canned controller data, and
tests/api checks the client library against the firmware. Neither shows the
two working together inside Home Assistant. Here hass-macon (main) is paired
with the bench controller, which drives the arctic-simulator over RS-485, so a
command sent from Home Assistant has to travel the whole way: integration,
controller, RS-485 write, simulated heat pump, next poll, push stream, and back
into the entity.

Nothing here sleeps for a fixed time; every wait is for a state change or for
the integration's own diagnostics refreshes.
"""

from __future__ import annotations

import asyncio
import socket
from collections import defaultdict
from collections.abc import Callable, Generator
from urllib.parse import urlsplit
from unittest.mock import PropertyMock, patch

import pytest
from homeassistant.components.logbook.helpers import is_sensor_continuous
from homeassistant.const import (
    ATTR_ENTITY_ID,
    ATTR_TEMPERATURE,
    CONF_HOST,
    CONF_PORT,
    STATE_UNAVAILABLE,
)
from homeassistant.core import HomeAssistant, State, callback
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.event import async_track_state_change_event
from pymacon import MaconClient
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.macon.const import (
    CONF_DEVICE_ID,
    CONF_FINGERPRINT,
    CONF_TOKEN,
    DIAGNOSTICS_INTERVAL,
    DOMAIN,
)

# Controller poll (~800 ms) + RS-485 write + push, with margin.
ROUND_TRIP_TIMEOUT = 15.0
# Diagnostics refreshes to watch for activity-log noise (one a minute).
NOISE_REFRESHES = 3


@pytest.fixture
def entity_registry_enabled_by_default() -> Generator[None]:
    """Enable the entities that are disabled by default, as HA core's tests do."""
    with patch(
        "homeassistant.helpers.entity.Entity.entity_registry_enabled_default",
        return_value=True,
        new_callable=PropertyMock,
    ):
        yield


@pytest.fixture
def pairing(device) -> dict:
    """Pair with the controller through its test instrumentation."""
    base = device.base_url
    response = device.session.post(f"{base}/api/test/ha-token", timeout=10)
    response.raise_for_status()
    token = response.json()["token"]
    response = device.session.get(f"{base}/api/test/ha-identity", timeout=10)
    response.raise_for_status()
    identity = response.json()
    return {
        # An address, so Home Assistant's resolver never gets involved.
        CONF_HOST: socket.gethostbyname(urlsplit(base).hostname),
        CONF_PORT: int(identity["port"]),
        CONF_TOKEN: token,
        CONF_FINGERPRINT: identity["sha256_fingerprint"],
    }


@pytest.fixture
async def live_entry(
    hass: HomeAssistant,
    pairing: dict,
    entity_registry_enabled_by_default: None,
) -> MockConfigEntry:
    probe = MaconClient(
        pairing[CONF_HOST],
        pairing[CONF_TOKEN],
        pairing[CONF_FINGERPRINT],
        port=pairing[CONF_PORT],
        session=async_get_clientsession(hass),
    )
    device_id = (await probe.fetch_capabilities()).device_id
    entry = MockConfigEntry(
        domain=DOMAIN,
        title="Bench controller",
        unique_id=device_id,
        data={**pairing, CONF_DEVICE_ID: device_id},
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    yield entry
    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()


def _entities(hass: HomeAssistant) -> list[er.RegistryEntry]:
    return [
        entry
        for entry in er.async_get(hass).entities.values()
        if entry.platform == DOMAIN
    ]


def _entity_id(hass: HomeAssistant, entry: MockConfigEntry, domain: str, key: str) -> str:
    entity_id = er.async_get(hass).async_get_entity_id(
        domain, DOMAIN, f"{entry.unique_id}_{key}"
    )
    assert entity_id is not None, f"{domain} {key} was not created"
    return entity_id


def _in_logbook(hass: HomeAssistant, entry: er.RegistryEntry) -> bool:
    """Whether a state change of this entity gets an activity-log entry."""
    if entry.domain != "sensor":
        return True
    return not is_sensor_continuous(hass, er.async_get(hass), entry.entity_id)


async def _wait_for(
    hass: HomeAssistant,
    entity_id: str,
    predicate: Callable[[State], bool],
    desc: str,
    timeout: float = ROUND_TRIP_TIMEOUT,
) -> State:
    done = asyncio.Event()

    @callback
    def check(_event=None) -> None:
        state = hass.states.get(entity_id)
        if state is not None and predicate(state):
            done.set()

    unsubscribe = async_track_state_change_event(hass, [entity_id], check)
    check()
    try:
        await asyncio.wait_for(done.wait(), timeout)
    except TimeoutError:
        pytest.fail(
            f"Timed out after {timeout:.0f}s waiting for {desc} "
            f"(last: {hass.states.get(entity_id)})"
        )
    finally:
        unsubscribe()
    return hass.states.get(entity_id)


async def test_every_entity_is_available(
    hass: HomeAssistant, live_entry: MockConfigEntry
) -> None:
    unavailable = sorted(
        entry.entity_id
        for entry in _entities(hass)
        if hass.states.get(entry.entity_id).state == STATE_UNAVAILABLE
    )
    assert unavailable == []


async def test_commands_from_home_assistant_reach_the_heat_pump(
    hass: HomeAssistant, live_entry: MockConfigEntry, sim
) -> None:
    climate = _entity_id(hass, live_entry, "climate", "climate")
    mode = _entity_id(hass, live_entry, "select", "mode")

    # The simulated unit switches to cooling; the entities must follow.
    await hass.async_add_executor_job(sim.load_preset, "cooling")
    await _wait_for(hass, mode, lambda s: s.state == "cooling", "mode to read cooling")
    state = await _wait_for(
        hass,
        climate,
        lambda s: s.attributes.get(ATTR_TEMPERATURE) is not None,
        "the cooling setpoint",
    )

    # Setpoint: Home Assistant -> controller -> heat pump -> back.
    current = int(state.attributes[ATTR_TEMPERATURE])
    target = current + 1 if current + 1 <= state.attributes["max_temp"] else current - 1
    await hass.services.async_call(
        "climate",
        "set_temperature",
        {ATTR_ENTITY_ID: climate, ATTR_TEMPERATURE: target},
        blocking=True,
    )
    await _wait_for(
        hass,
        climate,
        lambda s: s.attributes.get(ATTR_TEMPERATURE) == target,
        f"the climate entity to show {target} °C",
    )
    fields = (await hass.async_add_executor_job(sim.state))["fields"]
    assert fields["cooling_setpoint"] == target

    # Mode: the same trip through the mode select.
    options = hass.states.get(mode).attributes["options"]
    wanted = "hot_water" if "hot_water" in options else options[0]
    assert wanted != "cooling", options
    await hass.services.async_call(
        "select",
        "select_option",
        {ATTR_ENTITY_ID: mode, "option": wanted},
        blocking=True,
    )
    await _wait_for(hass, mode, lambda s: s.state == wanted, f"mode to read {wanted}")
    fields = (await hass.async_add_executor_job(sim.state))["fields"]
    assert fields["working_mode"] == wanted


async def test_routine_updates_stay_out_of_the_activity_log(
    hass: HomeAssistant, live_entry: MockConfigEntry
) -> None:
    """A steady heat pump must not fill the activity log.

    Home Assistant's activity log records every state change of an entity that
    isn't a measurement, so a value that ticks on every poll (like the old
    "Last RS485 response" sensor did) floods it. Watch several diagnostics
    refreshes of a live, idle unit and flag any such entity that keeps
    changing.
    """
    watched = [entry.entity_id for entry in _entities(hass) if _in_logbook(hass, entry)]
    history: dict[str, list[str]] = defaultdict(list)

    @callback
    def record(event) -> None:
        old, new = event.data["old_state"], event.data["new_state"]
        if old is not None and new is not None and old.state != new.state:
            history[new.entity_id].append(f"{old.state} -> {new.state}")

    client = live_entry.runtime_data.client
    fetch = client.async_fetch_diagnostics
    refreshes = 0
    enough = asyncio.Event()

    async def counted_fetch():
        nonlocal refreshes
        try:
            return await fetch()
        finally:
            refreshes += 1
            if refreshes >= NOISE_REFRESHES:
                enough.set()

    unsubscribe = async_track_state_change_event(hass, watched, record)
    client.async_fetch_diagnostics = counted_fetch
    timeout = (NOISE_REFRESHES + 1) * DIAGNOSTICS_INTERVAL.total_seconds()
    try:
        await asyncio.wait_for(enough.wait(), timeout)
        await hass.async_block_till_done()
    except TimeoutError:
        pytest.fail(f"Only {refreshes} diagnostics refreshes in {timeout:.0f}s")
    finally:
        client.async_fetch_diagnostics = fetch
        unsubscribe()

    # One change can be real (something settling); more is a pattern.
    noisy = {entity_id: changes for entity_id, changes in history.items() if len(changes) > 1}
    assert noisy == {}, (
        f"These entities changed state repeatedly over {NOISE_REFRESHES} diagnostics "
        "refreshes of an idle heat pump; each change is an activity-log entry. "
        "Make them measurements or move the value into an attribute."
    )
