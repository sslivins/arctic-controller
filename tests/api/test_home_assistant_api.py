"""Functional tests for the versioned Home Assistant REST foundation."""

import os

import pytest
import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
_session = requests.Session()
_session.headers["Connection"] = "close"
_session.mount(
    "http://",
    HTTPAdapter(max_retries=Retry(total=3, backoff_factor=1)),
)
_session.mount(
    "https://",
    HTTPAdapter(max_retries=Retry(total=3, backoff_factor=1)),
)
_session.verify = False


def _issue_test_token() -> str:
    response = _session.post(f"{BASE_URL}/api/test/ha-token", timeout=10)
    if response.status_code == 404:
        pytest.skip("Device firmware does not expose test instrumentation")
    response.raise_for_status()
    token = response.json()["token"]
    assert len(token) == 64
    return token


def _headers(token: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


def test_integration_routes_never_use_legacy_auth_bypass():
    response = _session.get(f"{BASE_URL}/api/v1/capabilities", timeout=10)
    assert response.status_code == 401

    response = _session.get(
        f"{BASE_URL}/api/v1/capabilities",
        headers={"Authorization": "Bearer " + ("0" * 64)},
        timeout=10,
    )
    assert response.status_code == 401


def test_capabilities_exclude_advanced_and_raw_controls():
    token = _issue_test_token()
    response = _session.get(
        f"{BASE_URL}/api/v1/capabilities",
        headers=_headers(token),
        timeout=10,
    )
    response.raise_for_status()
    data = response.json()

    assert data["protocol_version"] == 1
    assert data["device_id"].startswith("arctic-")
    assert data["transports"] == {"rest": True, "websocket": False}
    assert data["capabilities"]["read_state"] is True
    assert data["capabilities"]["control_power"] is False
    assert data["capabilities"]["advanced_parameters"] is False
    assert data["capabilities"]["raw_registers"] is False


def test_state_identity_and_revision_are_ordered():
    token = _issue_test_token()
    first = _session.get(
        f"{BASE_URL}/api/v1/state",
        headers=_headers(token),
        timeout=10,
    )
    second = _session.get(
        f"{BASE_URL}/api/v1/state",
        headers=_headers(token),
        timeout=10,
    )
    first.raise_for_status()
    second.raise_for_status()
    first_data = first.json()
    second_data = second.json()

    assert first_data["protocol_version"] == 1
    assert first_data["device_id"] == second_data["device_id"]
    assert first_data["boot_id"] == second_data["boot_id"]
    assert second_data["revision"] >= first_data["revision"] >= 1
    assert "reversing_valve_request" in first_data["state"]["components"]


def test_rotating_token_immediately_invalidates_previous_token():
    previous = _issue_test_token()
    current = _issue_test_token()
    assert previous != current

    rejected = _session.get(
        f"{BASE_URL}/api/v1/state",
        headers=_headers(previous),
        timeout=10,
    )
    accepted = _session.get(
        f"{BASE_URL}/api/v1/state",
        headers=_headers(current),
        timeout=10,
    )
    assert rejected.status_code == 401
    assert accepted.status_code == 200
