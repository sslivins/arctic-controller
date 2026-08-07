#!/usr/bin/env python3
"""Continuously capture an ESP device's USB serial output across reconnects."""

from __future__ import annotations

import argparse
import glob
import signal
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

import serial


running = True


def stop_capture(_signum: int, _frame: object) -> None:
    global running
    running = False


def usb_serial_for(port: str) -> str | None:
    result = subprocess.run(
        ["udevadm", "info", "-q", "property", f"--name={port}"],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )
    for line in result.stdout.splitlines():
        if line.startswith("ID_SERIAL_SHORT="):
            return line.partition("=")[2]
    return None


def find_port(expected_serial: str) -> str | None:
    for port in sorted(glob.glob("/dev/ttyACM*")):
        try:
            if usb_serial_for(port) == expected_serial:
                return port
        except (OSError, subprocess.SubprocessError):
            continue
    return None


def marker(message: str) -> bytes:
    timestamp = datetime.now(timezone.utc).isoformat()
    return f"\n--- {timestamp} {message} ---\n".encode()


def capture(expected_serial: str, output: Path, baud: int) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("ab", buffering=0) as log:
        while running:
            port = find_port(expected_serial)
            if port is None:
                time.sleep(1)
                continue

            try:
                log.write(marker(f"connected to {port} at {baud} baud"))
                with serial.Serial(port, baudrate=baud, timeout=1) as device:
                    while running:
                        chunk = device.read(4096)
                        if chunk:
                            log.write(chunk)
            except (OSError, serial.SerialException) as error:
                log.write(marker(f"serial disconnected: {error}"))
                time.sleep(1)

        log.write(marker("capture stopped"))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--usb-serial", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    signal.signal(signal.SIGINT, stop_capture)
    signal.signal(signal.SIGTERM, stop_capture)
    capture(args.usb_serial, args.output, args.baud)


if __name__ == "__main__":
    main()
