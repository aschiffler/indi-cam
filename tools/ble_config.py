"""Read or write the configuration of an indi-cam over Bluetooth, from a PC.

    python tools/ble_config.py scan
    python tools/ble_config.py get  [device-id] [--code CODE]
    python tools/ble_config.py set  [device-id] [--code CODE] key=value ...
    python tools/ble_config.py watch [device-id]          # live status

device-id is the 6 characters from the device name (indi-cam-xxxxxx). It can be left
out if only one device is in range. Keys for "set": ssid ent psk eapUser eapPass eapId
host port tls ca user pass topic interval. Settings that are not given keep their value.

Needs: python -m pip install bleak cryptography
The protocol is described in src/ble.cpp.
"""
import argparse
import asyncio
import json
import os
import sys

from bleak import BleakClient, BleakScanner
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

BASE = "3406ec0%d-b055-4c3d-9eac-0a98b1d12d54"
SERVICE, KEY, RX, TX, STATUS = (BASE % i for i in range(5))
SECRET_KEYS = {"psk", "eapPass", "pass"}


class Session:
    """One request/response exchange (mirrors openSession in webflasher/proto.js)."""

    def __init__(self, device_public: bytes):
        own = ec.generate_private_key(ec.SECP256R1())
        peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), device_public)
        digest = hashes.Hash(hashes.SHA256())
        digest.update(own.exchange(ec.ECDH(), peer))
        self.aes = AESGCM(digest.finalize())
        self.public = own.public_key().public_bytes(
            serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)

    def seal(self, request: dict) -> bytes:
        iv = os.urandom(12)
        return self.public + iv + self.aes.encrypt(iv, json.dumps(request).encode(), None)

    def open(self, response: bytes) -> dict:
        return json.loads(self.aes.decrypt(response[:12], response[12:], None))


def to_frames(message: bytes, size: int):
    chunk = size - 1
    for off in range(0, len(message), chunk):
        flag = (1 if off == 0 else 0) | (2 if off + chunk >= len(message) else 0)
        yield bytes([flag]) + message[off:off + chunk]


async def find(device_id):
    print("Scanning ...", file=sys.stderr)
    found = await BleakScanner.discover(timeout=8, service_uuids=[SERVICE], return_adv=True)
    devices = [(d, adv) for d, adv in found.values()
               if not device_id or (adv.local_name or d.name or "").endswith(device_id)]
    if not devices:
        sys.exit("No indi-cam found" + (f" with ID {device_id}" if device_id else ""))
    return devices


async def request(client: BleakClient, payload: dict) -> dict:
    session = Session(bytes(await client.read_gatt_char(KEY)))
    done = asyncio.get_running_loop().create_future()
    parts = []

    def on_frame(_, frame: bytearray):
        if frame[0] & 1:
            parts.clear()
        parts.append(bytes(frame[1:]))
        if frame[0] & 2 and not done.done():
            done.set_result(b"".join(parts))

    await client.start_notify(TX, on_frame)
    try:
        for frame in to_frames(session.seal(payload), min(client.mtu_size - 3, 180)):
            await client.write_gatt_char(RX, frame, response=True)
        return session.open(await asyncio.wait_for(done, 15))
    finally:
        await client.stop_notify(TX)


def parse_value(key, value):
    if key in ("ent", "tls"):
        return value.lower() in ("1", "true", "yes", "on")
    if key in ("port", "interval"):
        return int(value)
    if key == "ca" and os.path.isfile(value):
        return open(value, encoding="ascii").read()
    return value


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["scan", "get", "set", "watch"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--code", default="indicam-setup", help="setup code (BLE_CODE in platformio.ini)")
    opt = ap.parse_args()
    device_id = opt.args[0] if opt.args and "=" not in opt.args[0] else None
    changes = dict(a.split("=", 1) for a in opt.args if "=" in a)

    devices = await find(device_id)
    if opt.command == "scan":
        for d, adv in devices:
            print(f"{adv.local_name or d.name}  {adv.rssi} dBm")
        return
    if len(devices) > 1:
        sys.exit("Several devices found, give the device ID: " +
                 ", ".join((adv.local_name or d.name or "?") for d, adv in devices))

    async with BleakClient(devices[0][0]) as client:
        print("Status:", bytes(await client.read_gatt_char(STATUS)).decode(), file=sys.stderr)
        if opt.command == "watch":
            await client.start_notify(STATUS, lambda _, v: print(bytes(v).decode(), flush=True))
            while client.is_connected:
                await asyncio.sleep(1)
            return

        reply = await request(client, {"code": opt.code, "cmd": "get"})
        if opt.command == "get" or not reply.get("ok"):
            print(json.dumps(reply, indent=2))
            return

        cfg = {k: v for k, v in reply["cfg"].items() if not k.startswith("has")}
        cfg.update({k: parse_value(k, v) for k, v in changes.items()})
        reply = await request(client, {"code": opt.code, "cmd": "set", "cfg": cfg})
        print(json.dumps(reply, indent=2))


asyncio.run(main())
