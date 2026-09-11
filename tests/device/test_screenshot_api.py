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
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Disable TLS verification for self-signed device certificate
_OrigSessionInit = requests.Session.__init__
def _session_init_no_verify(self, *args, **kwargs):
    _OrigSessionInit(self, *args, **kwargs)
    self.verify = False
requests.Session.__init__ = _session_init_no_verify

# Device URL and API key from environment
ARCTIC_URL = os.environ.get("ARCTIC_URL", "http://arctic.local")
API_KEY = os.environ.get("ARCTIC_API_KEY")

# Expected display dimensions (Tab5 portrait)
EXPECTED_WIDTH = 720
EXPECTED_HEIGHT = 1280

# PNG magic bytes
# Retry-enabled session for API calls
_session = requests.Session()
_retry = Retry(total=3, backoff_factor=1, allowed_methods=None,
               status_forcelist=[502, 503, 504])
_session.mount("http://", HTTPAdapter(max_retries=_retry))
_session.mount("https://", HTTPAdapter(max_retries=_retry))

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# JPEG framing. SOI opens every JPEG, EOI closes it; checking both is what
# distinguishes a complete image from one truncated by a failed send.
JPEG_SOI = b"\xff\xd8"
JPEG_EOI = b"\xff\xd9"


def _api_headers(api_key: str = API_KEY) -> dict:
    """Build request headers with optional API key."""
    headers = {}
    if api_key:
        headers["X-API-Key"] = api_key
    return headers


def _get_screenshot(api_key: str = API_KEY, params: dict = None) -> requests.Response:
    """Fetch a screenshot from the production endpoint."""
    return _session.get(
        f"{ARCTIC_URL}/api/screenshot",
        headers=_api_headers(api_key),
        params=params,
        timeout=30.0,
    )


def _auth_config_post(payload: dict):
    """POST /api/auth/config, handling the case where web auth is already on.

    When ``web_auth_enabled`` is already true, the endpoint requires a
    session cookie (``check_web_auth``), not just an API key.  We try
    with the API key first; on 401 we log in with default creds and retry.
    """
    for attempt in range(3):
        try:
            r = requests.post(
                f"{ARCTIC_URL}/api/auth/config",
                json=payload,
                headers=_api_headers(),
                timeout=5,
            )
            if r.status_code == 401:
                # Web auth is on — need a session cookie
                s = requests.Session()
                s.post(
                    f"{ARCTIC_URL}/login",
                    json={"username": "arctic", "password": "arctic"},
                    timeout=5,
                )
                s.post(
                    f"{ARCTIC_URL}/api/auth/config",
                    json=payload,
                    timeout=5,
                )
            return
        except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
            if attempt == 2:
                raise
            time.sleep(2)


def _enable_web_auth():
    """Enable web + API auth so that API key enforcement kicks in.

    Both flags must be true for ``check_api_auth`` to reject requests:
    ``api_auth_enabled`` gates the entire check (short-circuits to *allow*
    when false), and ``web_auth_enabled`` prevents the "allow local web UI"
    fallback.  On a fresh device both default to false, so we must set both.
    """
    _auth_config_post({"web_auth_enabled": True, "api_auth_enabled": True})


def _disable_web_auth():
    """Disable web + API auth (restore normal test state)."""
    _auth_config_post({"web_auth_enabled": False, "api_auth_enabled": False})


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


def _parse_jpeg_dimensions(data: bytes) -> tuple:
    """Return (width, height) from a JPEG's SOFn marker.

    Walks the marker segments rather than assuming a fixed offset, because the
    hardware encoder is free to emit whatever tables and APPn segments it likes
    ahead of the frame header.
    """
    assert data[:2] == JPEG_SOI, "Not a JPEG (missing SOI)"
    i = 2
    while i + 3 < len(data):
        if data[i] != 0xFF:
            raise AssertionError(f"Expected a marker at offset {i}, got {data[i]:#04x}")
        marker = data[i + 1]
        # Standalone markers carry no length field.
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        seg_len = struct.unpack(">H", data[i + 2:i + 4])[0]
        # SOF0/1/2/3, 5-7, 9-11, 13-15 — every frame header except DHT (0xC4),
        # JPG (0xC8) and DAC (0xCC), which share the 0xCn range.
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            height, width = struct.unpack(">HH", data[i + 5:i + 9])
            return width, height
        if marker == 0xDA:  # start of scan — no frame header found
            break
        i += 2 + seg_len
    raise AssertionError("No SOF marker found in JPEG")


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


@pytest.mark.skipif(not API_KEY, reason="ARCTIC_API_KEY not set")
class TestScreenshotJPEG:
    """Tests for GET /api/screenshot?format=jpeg (hardware JPEG encoder)."""

    def test_jpeg_content_type(self):
        """format=jpeg returns image/jpeg, not image/png."""
        r = _get_screenshot(params={"format": "jpeg"})
        assert r.status_code == 200
        assert "image/jpeg" in r.headers.get("Content-Type", "")

    def test_jpeg_is_complete(self):
        """Body is framed by SOI and EOI.

        EOI matters as much as SOI: a truncated response still starts with
        SOI, so only the terminator proves the whole image arrived.
        """
        r = _get_screenshot(params={"format": "jpeg"})
        assert r.status_code == 200
        assert r.content[:2] == JPEG_SOI, "Response is not a JPEG"
        assert r.content[-2:] == JPEG_EOI, "JPEG is truncated (no EOI)"

    def test_jpeg_dimensions(self):
        """SOF reports the full 720×1280 display."""
        r = _get_screenshot(params={"format": "jpeg"})
        assert r.status_code == 200
        width, height = _parse_jpeg_dimensions(r.content)
        assert width == EXPECTED_WIDTH, f"Expected width {EXPECTED_WIDTH}, got {width}"
        assert height == EXPECTED_HEIGHT, f"Expected height {EXPECTED_HEIGHT}, got {height}"

    def test_jpeg_is_far_smaller_than_png(self):
        """The point of the JPEG path is the payload reduction.

        The PNG is ~2.77 MB uncompressed. If the JPEG is not dramatically
        smaller the feature has no reason to exist, so assert the benefit
        rather than merely asserting validity.
        """
        r = _get_screenshot(params={"format": "jpeg"})
        assert r.status_code == 200
        size = len(r.content)
        assert size > 10_000, f"JPEG suspiciously small ({size} bytes) — likely blank or corrupt"
        assert size < 1_000_000, f"JPEG not meaningfully smaller than the PNG ({size} bytes)"

    def test_jpg_alias(self):
        """format=jpg is accepted as a synonym for jpeg."""
        r = _get_screenshot(params={"format": "jpg"})
        assert r.status_code == 200
        assert "image/jpeg" in r.headers.get("Content-Type", "")

    def test_jpeg_content_disposition(self):
        """Filename reflects the actual format, not screenshot.png."""
        r = _get_screenshot(params={"format": "jpeg"})
        assert r.status_code == 200
        assert "screenshot.jpg" in r.headers.get("Content-Disposition", "")

    def test_default_format_is_still_png(self):
        """Omitting format must not change existing clients' behaviour."""
        r = _get_screenshot()
        assert r.status_code == 200
        assert "image/png" in r.headers.get("Content-Type", "")
        assert r.content[:8] == PNG_SIGNATURE

    def test_explicit_png_format(self):
        """format=png selects the PNG path explicitly."""
        r = _get_screenshot(params={"format": "png"})
        assert r.status_code == 200
        assert r.content[:8] == PNG_SIGNATURE

    def test_quality_changes_payload_size(self):
        """quality is wired through to the encoder, not silently ignored.

        Asserting only that a low-quality request succeeds would pass even if
        the parameter were dropped, so compare the sizes at the two extremes.
        """
        low = _get_screenshot(params={"format": "jpeg", "quality": 10})
        high = _get_screenshot(params={"format": "jpeg", "quality": 95})
        assert low.status_code == 200
        assert high.status_code == 200
        assert len(low.content) < len(high.content), (
            f"quality had no effect: q10={len(low.content)} bytes, "
            f"q95={len(high.content)} bytes"
        )

    @pytest.mark.parametrize("bad_format", ["gif", "bmp", "", "jpeg2000"])
    def test_invalid_format_rejected(self, bad_format):
        """An unsupported format is a client error, not a silent PNG."""
        r = _get_screenshot(params={"format": bad_format})
        assert r.status_code == 400, (
            f"format={bad_format!r} should be rejected, got {r.status_code}"
        )

    @pytest.mark.parametrize("bad_quality", ["0", "101", "-1", "abc"])
    def test_invalid_quality_rejected(self, bad_quality):
        """Out-of-range or non-numeric quality is a client error."""
        r = _get_screenshot(params={"format": "jpeg", "quality": bad_quality})
        assert r.status_code == 400, (
            f"quality={bad_quality!r} should be rejected, got {r.status_code}"
        )

    def test_consecutive_jpeg_screenshots(self):
        """Back-to-back encodes must both succeed.

        The encoder engine is created and destroyed per request; if it were
        ever leaked the peripheral would be stranded and the second request
        would fail. That makes this the regression test for engine teardown.
        """
        for attempt in range(3):
            r = _get_screenshot(params={"format": "jpeg"})
            assert r.status_code == 200, f"attempt {attempt + 1} failed: {r.status_code}"
            assert r.content[:2] == JPEG_SOI
            assert r.content[-2:] == JPEG_EOI
