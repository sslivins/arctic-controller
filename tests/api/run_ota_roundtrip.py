"""Manual OTA round-trip test — run directly, not via pytest."""
import re
import requests
import time
import sys
from pathlib import Path

BASE_URL = "http://192.168.1.23"
API_KEY = "94b8e52abfd8744e5c6de4314ef22a70"


def headers():
    return {"X-API-Key": API_KEY}


def get_cmake_version():
    """Read PROJECT_VER from CMakeLists.txt."""
    cmake_path = Path(__file__).resolve().parent.parent.parent / "CMakeLists.txt"
    text = cmake_path.read_text()
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    return m.group(1) if m else None


def main():
    # Load firmware
    bin_path = Path(__file__).resolve().parent.parent.parent / "build" / "arctic_controller.bin"
    firmware = bin_path.read_bytes()
    print(f"Firmware size: {len(firmware)} bytes")
    print(f"Header byte: 0x{firmware[0]:02X}")

    # Record pre-OTA state
    pre = requests.get(f"{BASE_URL}/api/ota/status", headers=headers(), timeout=10).json()
    expected_version = get_cmake_version()
    print(f"Pre-OTA: version={pre['current_version']}, state={pre['state']}, "
          f"pending_verify={pre.get('pending_verify', 'N/A')}")
    print(f"Expected post-OTA version (from CMakeLists.txt): {expected_version}")

    # Upload firmware
    print("\nUploading firmware...")
    h = headers()
    h["Content-Type"] = "application/octet-stream"
    r = requests.post(f"{BASE_URL}/api/ota/upload", headers=h, data=firmware, timeout=120)
    print(f"Upload response: {r.status_code} {r.text}")

    if r.status_code != 200:
        print("UPLOAD FAILED")
        return 1

    # Wait for reboot: first observe the device go offline (so we don't read the
    # pre-reboot instance), then poll until it serves again.
    print("\nWaiting for device to reboot (offline -> online)...")
    seen_offline = False
    for i in range(45):
        try:
            r = requests.get(f"{BASE_URL}/api/ota/status", headers=headers(), timeout=5)
            if r.status_code == 200:
                if not seen_offline:
                    print(f"  attempt {i+1}/45 (still on pre-reboot instance)...")
                    time.sleep(2)
                    continue
                post = r.json()
                print(f"\nDevice is back!")
                print(f"  version: {post['current_version']}")
                print(f"  state: {post['state']}")
                print(f"  pending_verify: {post.get('pending_verify', 'N/A')}")

                # Verify
                ok = True
                if post["current_version"] != expected_version:
                    print(f"  FAIL: version mismatch ({post['current_version']} != {expected_version})")
                    ok = False
                if post["state"] != "idle":
                    print(f"  FAIL: unexpected state ({post['state']})")
                    ok = False

                if ok:
                    print("\n✅ ROUND-TRIP TEST PASSED")
                    return 0
                else:
                    print("\n❌ ROUND-TRIP TEST FAILED")
                    return 1
        except requests.ConnectionError:
            seen_offline = True
        print(f"  attempt {i+1}/45...")
        time.sleep(2)

    print("\n❌ TIMEOUT: Device did not come back")
    return 1


if __name__ == "__main__":
    sys.exit(main())
