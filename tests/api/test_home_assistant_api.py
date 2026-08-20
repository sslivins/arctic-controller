"""Functional tests for the versioned Home Assistant REST foundation."""

import os
import hashlib
import socket
import ssl
from urllib.parse import urlsplit

import pytest
import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
_base_parts = urlsplit(BASE_URL)
HA_URL = os.environ.get(
    "ARCTIC_HA_URL",
    f"https://{_base_parts.hostname}:8443",
)
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


def _open_test_pairing_window() -> str:
    response = _session.post(
        f"{BASE_URL}/api/test/ha-pairing-window", timeout=10
    )
    if response.status_code == 404:
        pytest.skip("Device firmware does not expose test instrumentation")
    response.raise_for_status()
    code = response.json()["code"]
    assert len(code) == 6
    assert code.isdigit()
    return code


def test_pairing_requires_a_physical_window():
    response = _session.post(
        f"{HA_URL}/api/v1/pair",
        json={"code": "000000"},
        timeout=10,
    )
    assert response.status_code == 403


def test_pairing_code_claims_once_and_rotates_token():
    previous = _issue_test_token()
    code = _open_test_pairing_window()

    rejected = _session.post(
        f"{HA_URL}/api/v1/pair",
        json={"code": "000000" if code != "000000" else "999999"},
        timeout=10,
    )
    assert rejected.status_code == 401

    claimed = _session.post(
        f"{HA_URL}/api/v1/pair",
        json={"code": code},
        timeout=10,
    )
    claimed.raise_for_status()
    data = claimed.json()
    current = data["token"]
    assert len(current) == 64
    assert data["protocol_version"] == 1
    assert data["device_id"].startswith("arctic-")
    assert len(data["sha256_fingerprint"]) == 64
    assert claimed.headers["Cache-Control"] == "no-store"

    old_response = _session.get(
        f"{HA_URL}/api/v1/state",
        headers=_headers(previous),
        timeout=10,
    )
    new_response = _session.get(
        f"{HA_URL}/api/v1/state",
        headers=_headers(current),
        timeout=10,
    )
    assert old_response.status_code == 401
    assert new_response.status_code == 200

    duplicate = _session.post(
        f"{HA_URL}/api/v1/pair",
        json={"code": code},
        timeout=10,
    )
    assert duplicate.status_code == 403


def test_pairing_rejects_non_exact_code_without_truncation():
    code = _open_test_pairing_window()
    malformed = _session.post(
        f"{HA_URL}/api/v1/pair",
        json={"code": code + "0"},
        timeout=10,
    )
    assert malformed.status_code == 400

    claimed = _session.post(
        f"{HA_URL}/api/v1/pair",
        json={"code": code},
        timeout=10,
    )
    assert claimed.status_code == 200


def test_pairing_window_closes_after_five_bad_codes():
    code = _open_test_pairing_window()
    wrong_code = "000000" if code != "000000" else "999999"
    for attempt in range(5):
        response = _session.post(
            f"{HA_URL}/api/v1/pair",
            json={"code": wrong_code},
            timeout=10,
        )
        assert response.status_code == (429 if attempt == 4 else 401)

    closed = _session.post(
        f"{HA_URL}/api/v1/pair",
        json={"code": code},
        timeout=10,
    )
    assert closed.status_code == 403


def test_integration_routes_never_use_legacy_auth_bypass():
    response = _session.get(f"{HA_URL}/api/v1/capabilities", timeout=10)
    assert response.status_code == 401

    response = _session.get(
        f"{HA_URL}/api/v1/capabilities",
        headers={"Authorization": "Bearer " + ("0" * 64)},
        timeout=10,
    )
    assert response.status_code == 401


def test_capabilities_exclude_advanced_and_raw_controls():
    token = _issue_test_token()
    response = _session.get(
        f"{HA_URL}/api/v1/capabilities",
        headers=_headers(token),
        timeout=10,
    )
    response.raise_for_status()
    data = response.json()

    assert data["protocol_version"] == 1
    assert data["device_id"].startswith("arctic-")
    assert data["transports"] == {"rest": True, "websocket": True}
    assert data["capabilities"]["read_state"] is True
    assert isinstance(data["capabilities"]["control_power"], bool)
    assert isinstance(data["capabilities"]["control_mode"], bool)
    assert isinstance(data["capabilities"]["control_setpoints"], bool)
    assert set(data["capabilities"]["setpoint_controls"]) == {
        "cooling",
        "heating",
        "hot_water",
    }
    assert isinstance(data["capabilities"]["supported_modes"], list)
    assert data["capabilities"]["advanced_parameters"] is False
    assert data["capabilities"]["raw_registers"] is False


def test_capabilities_report_network_identity():
    token = _issue_test_token()
    response = _session.get(
        f"{HA_URL}/api/v1/capabilities",
        headers=_headers(token),
        timeout=10,
    )
    response.raise_for_status()
    data = response.json()

    network = data["network"]
    assert set(network) == {"ip_address", "local_hostname"}
    assert network["local_hostname"].endswith(".local")
    assert network["local_hostname"].startswith("arctic-")
    assert network["ip_address"] is None or (
        isinstance(network["ip_address"], str) and network["ip_address"]
    )
    token = _issue_test_token()
    first = _session.get(
        f"{HA_URL}/api/v1/state",
        headers=_headers(token),
        timeout=10,
    )
    second = _session.get(
        f"{HA_URL}/api/v1/state",
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
        f"{HA_URL}/api/v1/state",
        headers=_headers(previous),
        timeout=10,
    )
    accepted = _session.get(
        f"{HA_URL}/api/v1/state",
        headers=_headers(current),
        timeout=10,
    )
    assert rejected.status_code == 401
    assert accepted.status_code == 200


def test_reported_identity_matches_tls_peer_certificate():
    identity = _session.get(
        f"{BASE_URL}/api/test/ha-identity", timeout=10
    )
    if identity.status_code == 404:
        pytest.skip("Device firmware does not expose test instrumentation")
    identity.raise_for_status()
    expected = identity.json()["sha256_fingerprint"]

    host = urlsplit(HA_URL).hostname
    port = urlsplit(HA_URL).port or 8443
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    with socket.create_connection((host, port), timeout=10) as raw_socket:
        with context.wrap_socket(raw_socket, server_hostname=host) as tls_socket:
            certificate = tls_socket.getpeercert(binary_form=True)

    assert hashlib.sha256(certificate).hexdigest() == expected
