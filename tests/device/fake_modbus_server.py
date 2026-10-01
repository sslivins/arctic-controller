"""Minimal Modbus TCP server that impersonates a Thermux (map v1).

Serves FC03 (holding) and FC04 (input) reads only, which is all the controller's
external temperature sensors use. Used by the device tests and for manual bench
checks:

    python fake_modbus_server.py --port 5020

Input registers (Thermux map v1):
    0..15      info block (map version 1, channel count 100, poll interval 30 s)
    100..199   channel temperatures, int16 in 0.01 °C, 0x8000 when invalid
    200..299   channel status (0 ok, 1 unassigned, 2 missing, 3 read error, 4 stale)
    300..399   seconds since each channel was last read
    1000..1399 channel ROM codes, 4 registers each
Holding registers (not served by a real Thermux, which answers FC03 with
exception 1; they stand in for a generic Modbus device):
    0..99      the same temperatures as input 100..199
    500..501   a float32 (big-endian word order) temperature
    502..503   the same float with swapped words
"""
import argparse
import asyncio
import struct

CHANNELS = 100
INVALID = 0x8000


class ThermuxMap:
    def __init__(self):
        self.temps = {3: 21.25, 4: 22.69}
        self.status = {c: 1 for c in range(CHANNELS)}
        self.status.update({3: 0, 4: 0, 6: 3})
        self.roms = {3: bytes.fromhex("28FF641E0F1C0203"), 4: bytes.fromhex("28FF9A2B0F1C0412")}
        self.float_c = 45.5

    def input(self, addr):
        if addr < 16:
            info = [1, 0, 0, 0, CHANNELS] + [0] * 10 + [30]
            return info[addr]
        if 100 <= addr < 100 + CHANNELS:
            c = addr - 100
            t = self.temps.get(c)
            return INVALID if t is None or self.status.get(c) != 0 else round(t * 100) & 0xFFFF
        if 200 <= addr < 200 + CHANNELS:
            return self.status.get(addr - 200, 1)
        if 300 <= addr < 300 + CHANNELS:
            return 7 if self.status.get(addr - 300) == 0 else 0xFFFF
        if 1000 <= addr < 1000 + 4 * CHANNELS:
            c, w = divmod(addr - 1000, 4)
            rom = self.roms.get(c, bytes(8))
            return (rom[2 * w] << 8) | rom[2 * w + 1]
        return None

    def holding(self, addr):
        if addr < CHANNELS:
            return self.input(100 + addr)
        be = struct.unpack(">HH", struct.pack(">f", self.float_c))
        if addr in (500, 501):
            return be[addr - 500]
        if addr in (502, 503):
            return be[1 - (addr - 502)]
        return None


def handle(m, unit_ids, pdu):
    fc = pdu[0]
    if fc not in (3, 4) or len(pdu) != 5:
        return bytes([fc | 0x80, 1])
    start, count = struct.unpack(">HH", pdu[1:5])
    if not 1 <= count <= 125:
        return bytes([fc | 0x80, 3])
    get = m.holding if fc == 3 else m.input
    vals = [get(a) for a in range(start, start + count)]
    if any(v is None for v in vals):
        return bytes([fc | 0x80, 2])
    return bytes([fc, 2 * count]) + b"".join(struct.pack(">H", v) for v in vals)


async def serve(host, port, unit_ids):
    m = ThermuxMap()

    async def client(reader, writer):
        try:
            while True:
                hdr = await reader.readexactly(7)
                tid, proto, length, unit = struct.unpack(">HHHB", hdr)
                pdu = await reader.readexactly(length - 1)
                if proto != 0:
                    break
                if unit_ids and unit not in unit_ids:
                    continue  # like a gateway with nothing behind that id: no reply
                resp = handle(m, unit_ids, pdu)
                writer.write(struct.pack(">HHHB", tid, 0, len(resp) + 1, unit) + resp)
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            writer.close()

    server = await asyncio.start_server(client, host, port)
    print(f"fake Thermux Modbus TCP on {host}:{port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5020)
    ap.add_argument("--unit", type=int, action="append", default=None,
                    help="unit id(s) to answer; default answers every id")
    a = ap.parse_args()
    asyncio.run(serve(a.host, a.port, set(a.unit) if a.unit else None))
