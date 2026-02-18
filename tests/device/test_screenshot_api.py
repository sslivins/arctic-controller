"""
Test: Screenshot API (/api/screenshot)

Verifies the production screenshot endpoint returns a valid PNG image
of the expected dimensions (720×1280 RGB).

This tests the production endpoint, not the test-only /api/test/screenshot.
The production endpoint uses the standard API auth (API key or session cookie).
When web auth is disabled, unauthenticated access is allowed.
"""

import io
import os
import struct
import time

import pytest
import requests

# Device URL and API key from environment
ARCTIC_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# Expected display dimensions (Tab5 portrait)
EXPECTED_WIDTH = 720
EXPECTED_HEIGHT = 1280

# PNG magic bytes
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _api_headers(api_key: str = API_KEY) -> dict:
    """Build request headers with optional API key."""
    headers = {}
    if api_key:
        headers["X-API-Key"] = api_key
    return headers


def _get_screenshot(api_key: str = API_KEY) -> requests.Response:
    """Fetch a screenshot from the production endpoint."""
    return requests.get(
        f"{ARCTIC_URL}/api/screenshot",
        headers=_api_headers(api_key),
        timeout=30.0,
    )


def _enable_web_auth():
    """Enable web auth so that API key enforcement kicks in."""
    requests.post(
        f"{ARCTIC_URL}/api/auth/config",
        json={"web_auth_enabled": True},
        headers=_api_headers(),
        timeout=5,
    )


def _disable_web_auth():
    """Disable web auth (restore normal test state)."""
    requests.post(
        f"{ARCTIC_URL}/api/auth/config",
        json={"web_auth_enabled": False},
        headers=_api_headers(),
        timeout=5,
    )


def _parse_png_ihdr(data: bytes) -> dict:
    """Parse the IHDR chunk from PNG data and return width, height, bit depth, color type."""
    assert data[:8] == PNG_SIGNATURE, "Not a valid PNG file"
    # IHDR is always the first chunk after the 8-byte signature
    # Chunk format: 4-byte length, 4-byte type, data, 4-byte CRC
    chunk_len = struct.unpack(">I", data[8:12])[0]
    chunk_type = data[12:16]
    assert chunk_type == b"IHDR", f"Expected IHDR chunk, got {chunk_type!r}"
    assert chunk_len == 13, f"IHDR chunk length should be 13, got {chunk_len}"
    # IHDR data: width(4), height(4), bit_depth(1), color_type(1), ...
    width, height = struct.unpack(">II", data[16:24])
    bit_depth = data[24]
    color_type = data[25]
    return {
        "width": width,
        "height": height,
        "bit_depth": bit_depth,
        "color_type": color_type,
    }


# ── Tests ────────────────────────────────────────────────────────────────


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
class TestScreenshotAPI:
    """Tests for GET /api/screenshot."""

    def test_returns_png_content_type(self):
        """Response has image/png content type."""
        r = _get_screenshot()
        assert r.status_code == 200
        assert "image/png" in r.headers.get("Content-Type", "")

    def test_returns_valid_png(self):
        """Response body starts with the PNG signature."""
        r = _get_screenshot()
        assert r.status_code == 200
        assert r.content[:8] == PNG_SIGNATURE, "Response is not a valid PNG"

    def test_png_dimensions(self):
        """PNG IHDR reports 720×1280."""
        r = _get_screenshot()
        assert r.status_code == 200
        ihdr = _parse_png_ihdr(r.content)
        assert ihdr["width"] == EXPECTED_WIDTH, f"Expected width {EXPECTED_WIDTH}, got {ihdr['width']}"
        assert ihdr["height"] == EXPECTED_HEIGHT, f"Expected height {EXPECTED_HEIGHT}, got {ihdr['height']}"

    def test_png_color_type_rgb(self):
        """PNG uses RGB color (color_type=2, bit_depth=8)."""
        r = _get_screenshot()
        assert r.status_code == 200
        ihdr = _parse_png_ihdr(r.content)
        assert ihdr["bit_depth"] == 8, f"Expected 8-bit depth, got {ihdr['bit_depth']}"
        assert ihdr["color_type"] == 2, f"Expected color_type 2 (RGB), got {ihdr['color_type']}"

    def test_reasonable_file_size(self):
        """Uncompressed 720×1280 RGB PNG should be ~2.7 MB."""
        r = _get_screenshot()
        assert r.status_code == 200
        size = len(r.content)
        # Uncompressed PNG: pixel data + overhead. Should be > 2 MB and < 4 MB.
        assert size > 2_000_000, f"PNG too small ({size} bytes) — likely corrupt"
        assert size < 4_000_000, f"PNG unexpectedly large ({size} bytes)"

    def test_content_disposition_header(self):
        """Response includes Content-Disposition with filename."""
        r = _get_screenshot()
        assert r.status_code == 200
        cd = r.headers.get("Content-Disposition", "")
        assert "screenshot.png" in cd

    def test_requires_auth_when_web_auth_enabled(self):
        """Request without API key returns 401 when web auth is on."""
        _enable_web_auth()
        try:
            r = _get_screenshot(api_key=None)
            assert r.status_code == 401
        finally:
            _disable_web_auth()

    def test_invalid_api_key_when_web_auth_enabled(self):
        """Request with wrong API key returns 401 when web auth is on."""
        _enable_web_auth()
        try:
            r = _get_screenshot(api_key="wrong-key-12345")
            assert r.status_code == 401
        finally:
            _disable_web_auth()

    def test_consecutive_screenshots_differ(self):
        """Two rapid screenshots should both be valid (no crash/leak).

        We don't assert pixel differences since the screen may be static,
        but both requests must succeed and return valid PNGs.
        """
        r1 = _get_screenshot()
        assert r1.status_code == 200
        assert r1.content[:8] == PNG_SIGNATURE

        r2 = _get_screenshot()
        assert r2.status_code == 200
        assert r2.content[:8] == PNG_SIGNATURE

        # Both should have the same dimensions
        ihdr1 = _parse_png_ihdr(r1.content)
        ihdr2 = _parse_png_ihdr(r2.content)
        assert ihdr1["width"] == ihdr2["width"]
        assert ihdr1["height"] == ihdr2["height"]
