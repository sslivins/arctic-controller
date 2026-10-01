#!/usr/bin/env python3
"""Tiny BACnet/IP UDP server for performance-sensor device tests.

Implements just enough confirmed ReadProperty and ReadPropertyMultiple for the
controller: a Device object with Object_List plus a handful of Analog Inputs.
No third-party dependencies; all encoding is deliberately explicit so failures
are easy to compare with the firmware codec.
"""

from __future__ import annotations

import argparse
import select
import socket
import struct
import time
from dataclasses import dataclass


OBJ_AI = 0
OBJ_AV = 2
OBJ_DEVICE = 8
PROP_DESCRIPTION = 28
PROP_MODEL_NAME = 70
PROP_OBJECT_IDENTIFIER = 75
PROP_OBJECT_LIST = 76
PROP_OBJECT_NAME = 77
PROP_PRESENT_VALUE = 85
PROP_RELIABILITY = 103
PROP_STATUS_FLAGS = 111
PROP_UNITS = 117
SVC_RP = 12
SVC_RPM = 14
UNITS_C = 62
UNITS_F = 64


@dataclass
class Analog:
    instance: int
    name: str
    celsius: float
    units: int = UNITS_C
    reliability: int = 0
    rom: str = "28FF000000000000"

    @property
    def present_value(self) -> float:
        if self.units == UNITS_F:
            return self.celsius * 9.0 / 5.0 + 32.0
        return self.celsius


class CodecError(Exception):
    pass


def oid(obj_type: int, instance: int) -> int:
    return (obj_type << 22) | instance


def app_unsigned(v: int) -> bytes:
    if v <= 0xFF:
        b = bytes([v])
    elif v <= 0xFFFF:
        b = v.to_bytes(2, "big")
    else:
        b = v.to_bytes(4, "big")
    return bytes([(2 << 4) | len(b)]) + b


def app_enum(v: int) -> bytes:
    b = bytes([v]) if v <= 0xFF else v.to_bytes(2, "big")
    return bytes([(9 << 4) | len(b)]) + b


def app_real(v: float) -> bytes:
    return bytes([(4 << 4) | 4]) + struct.pack(">f", float(v))


def app_string(s: str) -> bytes:
    raw = s.encode("utf-8")[:63]
    data = b"\x00" + raw
    return bytes([(7 << 4) | 5, len(data)]) + data


def app_bitstring(flags: int = 0) -> bytes:
    return bytes([(8 << 4) | 2, 4, flags & 0xF0])


def app_oid(obj_type: int, instance: int) -> bytes:
    return bytes([(12 << 4) | 4]) + oid(obj_type, instance).to_bytes(4, "big")


def ctx(tag: int, value: int) -> bytes:
    if value <= 0xFF:
        b = bytes([value])
    elif value <= 0xFFFF:
        b = value.to_bytes(2, "big")
    elif value <= 0xFFFFFF:
        b = value.to_bytes(3, "big")
    else:
        b = value.to_bytes(4, "big")
    return bytes([(tag << 4) | 0x08 | len(b)]) + b


def ctx_oid(tag: int, obj_type: int, instance: int) -> bytes:
    return bytes([(tag << 4) | 0x0C]) + oid(obj_type, instance).to_bytes(4, "big")


def opening(tag: int) -> bytes:
    return bytes([(tag << 4) | 0x0E])


def closing(tag: int) -> bytes:
    return bytes([(tag << 4) | 0x0F])


def read_ctx(data: bytes, pos: int, expected: int) -> tuple[int, int]:
    if pos >= len(data):
        raise CodecError("truncated context tag")
    tag = data[pos] >> 4
    lvt = data[pos] & 0x07
    is_ctx = data[pos] & 0x08
    pos += 1
    if tag != expected or not is_ctx or lvt in (6, 7) or lvt == 0 or lvt > 4:
        raise CodecError("bad context tag")
    if pos + lvt > len(data):
        raise CodecError("truncated context value")
    return int.from_bytes(data[pos : pos + lvt], "big"), pos + lvt


def parse_request(frame: bytes) -> tuple[int, int, bytes]:
    if len(frame) < 10 or frame[:2] != b"\x81\x0a":
        raise CodecError("not BACnet/IP original unicast")
    total = int.from_bytes(frame[2:4], "big")
    if total != len(frame) or frame[4] != 1:
        raise CodecError("bad BVLC/NPDU")
    if frame[6] & 0xF0 != 0x00:
        raise CodecError("not confirmed request")
    return frame[8], frame[9], frame[10:]


def bvlc(apdu: bytes) -> bytes:
    body = b"\x01\x00" + apdu
    total = len(body) + 4
    return b"\x81\x0a" + total.to_bytes(2, "big") + body


class FakeBacnetServer:
    def __init__(self, host: str, port: int, device_instance: int = 1234,
                 huge_object_count: int = 0, rpm_unsupported: bool = False,
                 rom_change: bool = False) -> None:
        self.host = host
        self.port = port
        self.device_instance = device_instance
        self.huge_object_count = huge_object_count
        self.rpm_unsupported = rpm_unsupported
        self.device_name = "Thermux Test"
        self.model_name = "Thermux"
        self.objects: list[tuple[int, int]] = [(OBJ_DEVICE, device_instance)]
        self.analogs = {
            3: Analog(3, "Supply tank", 21.4, UNITS_C, 0, "28FF6491631603A2"),
            4: Analog(4, "Return tank", 68.0, UNITS_F, 0, "28FF6491631603A3"),
            5: Analog(5, "Faulted sensor", 19.0, UNITS_C, 1, "28FF6491631603A4"),
        }
        if rom_change:
            self.analogs[3].rom = "28FF64916316FFFF"
        self.objects += [(OBJ_AI, i) for i in self.analogs]
        self.drop = False

    @property
    def object_count(self) -> int:
        return self.huge_object_count or len(self.objects)

    def object_at_index(self, array_index: int) -> tuple[int, int]:
        if array_index < 1 or array_index > self.object_count:
            raise KeyError(("object-list", array_index))
        if not self.huge_object_count:
            return self.objects[array_index - 1]
        if array_index == 1:
            return OBJ_DEVICE, self.device_instance
        return OBJ_AI, array_index - 1

    def generated_analog(self, instance: int) -> Analog:
        if instance in self.analogs:
            return self.analogs[instance]
        if self.huge_object_count and 1 <= instance < self.huge_object_count:
            return Analog(instance, f"Generated sensor {instance}", 20.0 + (instance % 10) / 10.0,
                          UNITS_C, 0, f"28FF{instance & 0xFFFFFFFFFFFF:012X}"[-16:])
        raise KeyError((OBJ_AI, instance))

    def property_value(self, obj_type: int, instance: int, prop: int, array_index: int | None) -> bytes:
        if obj_type == OBJ_DEVICE and instance in (self.device_instance, 4194303):
            if prop == PROP_OBJECT_IDENTIFIER:
                return app_oid(OBJ_DEVICE, self.device_instance)
            if prop == PROP_OBJECT_NAME:
                return app_string(self.device_name)
            if prop == PROP_MODEL_NAME:
                return app_string(self.model_name)
            if prop == PROP_OBJECT_LIST:
                if array_index == 0:
                    return app_unsigned(self.object_count)
                if array_index is not None:
                    obj = self.object_at_index(array_index)
                    return app_oid(*obj)
                return b"".join(app_oid(*obj) for obj in self.objects)
        if obj_type == OBJ_AI:
            a = self.generated_analog(instance)
            if prop == PROP_OBJECT_IDENTIFIER:
                return app_oid(OBJ_AI, instance)
            if prop == PROP_OBJECT_NAME:
                return app_string(a.name)
            if prop == PROP_PRESENT_VALUE:
                return app_real(a.present_value)
            if prop == PROP_UNITS:
                return app_enum(a.units)
            if prop == PROP_RELIABILITY:
                return app_enum(a.reliability)
            if prop == PROP_STATUS_FLAGS:
                return app_bitstring(0x40 if a.reliability else 0)
            if prop == PROP_DESCRIPTION:
                return app_string(a.rom)
        raise KeyError((obj_type, instance, prop))

    def handle_rp(self, invoke: int, data: bytes) -> bytes:
        obj, pos = read_ctx(data, 0, 0)
        prop, pos = read_ctx(data, pos, 1)
        array_index = None
        if pos < len(data):
            array_index, pos = read_ctx(data, pos, 2)
        obj_type, instance = obj >> 22, obj & 0x3FFFFF
        value = self.property_value(obj_type, instance, prop, array_index)
        apdu = bytes([0x30, invoke, SVC_RP]) + ctx_oid(0, obj_type, instance) + ctx(1, prop)
        if array_index is not None:
            apdu += ctx(2, array_index)
        apdu += opening(3) + value + closing(3)
        return bvlc(apdu)

    def handle_rpm(self, invoke: int, data: bytes) -> bytes:
        if self.rpm_unsupported:
            return bvlc(bytes([0x60, invoke, 9]))  # Reject: unrecognized-service
        obj, pos = read_ctx(data, 0, 0)
        if data[pos] != 0x1E:
            raise CodecError("missing property-list opening tag")
        pos += 1
        props: list[int] = []
        while pos < len(data) and data[pos] != 0x1F:
            prop, pos = read_ctx(data, pos, 0)
            props.append(prop)
        obj_type, instance = obj >> 22, obj & 0x3FFFFF
        result = bytes([0x30, invoke, SVC_RPM]) + ctx_oid(0, obj_type, instance) + opening(1)
        for prop in props:
            result += ctx(2, prop) + opening(4) + self.property_value(obj_type, instance, prop, None) + closing(4)
        result += closing(1)
        return bvlc(result)

    def serve_forever(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind((self.host, self.port))
        except OSError as exc:
            raise SystemExit(f"failed to bind UDP {self.host}:{self.port}: {exc}") from exc
        self.port = sock.getsockname()[1]
        sock.setblocking(False)
        print(f"fake BACnet server listening on {self.host}:{self.port}", flush=True)
        try:
            while True:
                readable, _, _ = select.select([sock], [], [], 0.2)
                if not readable:
                    continue
                data, addr = sock.recvfrom(2048)
                if self.drop:
                    continue
                try:
                    invoke, svc, payload = parse_request(data)
                    if svc == SVC_RP:
                        reply = self.handle_rp(invoke, payload)
                    elif svc == SVC_RPM:
                        reply = self.handle_rpm(invoke, payload)
                    else:
                        continue
                    sock.sendto(reply, addr)
                except Exception as exc:  # keep the fake alive for malformed probes
                    print(f"ignored BACnet packet from {addr}: {exc}", flush=True)
        finally:
            sock.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--device-instance", type=int, default=1234)
    parser.add_argument("--wrong-device-instance", action="store_true",
                        help="Use a different device instance than the tests normally expect")
    parser.add_argument("--huge-object-count", type=int, default=0,
                        help="Claim this many Object_List entries and generate AIs lazily")
    parser.add_argument("--rpm-unsupported", action="store_true",
                        help="Reject RPM so clients must fall back to individual ReadProperty")
    parser.add_argument("--rom-change", action="store_true",
                        help="Serve a different ROM ID for AI 3")
    args = parser.parse_args()
    instance = args.device_instance + 1 if args.wrong_device_instance else args.device_instance
    FakeBacnetServer(args.host, args.port, instance, args.huge_object_count,
                     args.rpm_unsupported, args.rom_change).serve_forever()


if __name__ == "__main__":
    main()
