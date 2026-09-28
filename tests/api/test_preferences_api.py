"""Live API contract checks for /api/preferences friendly-name support."""

import os

import pytest
import requests
import urllib3


urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BASE_URL = os.environ.get("ARCTIC_URL", "http://arctic.local").rstrip("/")
API_KEY = os.environ.get("ARCTIC_API_KEY")


pytestmark = pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")


def _headers():
    return {"X-API-Key": API_KEY}


@pytest.fixture(scope="module", autouse=True)
def _reachable():
    try:
        r = requests.get(f"{BASE_URL}/api/health", timeout=5, verify=False)
        r.raise_for_status()
    except Exception as exc:
        pytest.skip(f"Device not reachable at {BASE_URL}: {exc}")


@pytest.fixture(autouse=True)
def _restore_name():
    requests.patch(
        f"{BASE_URL}/api/preferences",
        headers=_headers(),
        json={"device_name": ""},
        timeout=10,
        verify=False,
    )
    yield
    requests.patch(
        f"{BASE_URL}/api/preferences",
        headers=_headers(),
        json={"device_name": ""},
        timeout=10,
        verify=False,
    )


def _patch_name(value):
    return requests.patch(
        f"{BASE_URL}/api/preferences",
        headers=_headers(),
        json={"device_name": value},
        timeout=10,
        verify=False,
    )


def test_preferences_get_includes_device_name():
    data = requests.get(
        f"{BASE_URL}/api/preferences",
        headers=_headers(),
        timeout=10,
        verify=False,
    ).json()
    assert "device_name" in data
    assert isinstance(data["device_name"], str)


def test_device_name_is_trimmed_and_persisted():
    response = _patch_name("  Heat Pump 1  ")
    assert response.status_code == 200
    assert response.json()["device_name"] == "Heat Pump 1"


@pytest.mark.parametrize(
    ("value", "message"),
    [
        ("A" * 31, "30 characters"),
        ("Heat Pump 😀", "display cannot show"),
        ("Heat\nPump", "control characters"),
    ],
)
def test_invalid_device_names_are_rejected(value, message):
    response = _patch_name(value)
    assert response.status_code == 400
    assert message in response.json()["error"]
