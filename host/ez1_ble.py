from __future__ import annotations

import asyncio
import json
from typing import Any

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


SERVICE_UUID = "0000fffe-0000-1000-8000-00805f9b34fb"
WRITE_UUID = "0000ff0a-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000ff0b-0000-1000-8000-00805f9b34fb"
_KEY = b"E7MiPPrs9v6i3DY3"
_IV = b"8914934610490056"
_STOP_COMMAND = "FBFB06AAE1000000000191FEFE"
_LIMIT_30_COMMAND = "FBFB06AAE10000031F01B3FEFE"
_COMMAND_SUCCESS_REPLY = "fbfb06de02000100000000fefe"


def _crypt(data: bytes, decrypt: bool = False) -> bytes:
    operation = Cipher(algorithms.AES(_KEY), modes.CBC(_IV)).decryptor() if decrypt else Cipher(
        algorithms.AES(_KEY), modes.CBC(_IV)
    ).encryptor()
    return operation.update(data) + operation.finalize()


def build_request(device_id: str, name: str, identifier: str, request_id: str) -> bytes:
    message = {
        "id": request_id,
        "deviceId": device_id,
        "type": "property",
        "method": "get",
        "identifier": identifier,
        "params": {},
        "company": "apsystems",
        "companyKey": "AmS4SV9oy3gk",
        "version": "1.0",
        "productKey": "EZ1" if name.startswith("EZ1_") else "",
    }
    plain = json.dumps(message, separators=(",", ":")).encode()
    plain += b"\0" * (-len(plain) % 16)
    return _crypt(plain).hex().encode()


def _build_inverter_request(device_id: str, command: str) -> bytes:
    message = {
        "id": "16",
        "deviceId": device_id,
        "type": "property",
        "identifier": "inverterCmd",
        "method": "set",
        "params": {"cmd": command},
        "company": "apsystems",
        "companyKey": "AmS4SV9oy3gk",
        "version": "1.0",
        "productKey": "EZ1",
    }
    plain = json.dumps(message, separators=(",", ":")).encode()
    plain += b"\0" * (-len(plain) % 16)
    return _crypt(plain).hex().encode()


def build_stop_request(device_id: str) -> bytes:
    return _build_inverter_request(device_id, _STOP_COMMAND)


def build_limit_30_request(device_id: str) -> bytes:
    return _build_inverter_request(device_id, _LIMIT_30_COMMAND)


def fragment_request(payload: bytes, sequence: int = 0) -> tuple[list[bytes], int]:
    fragments = []
    while len(payload) > 40:
        fragments.append(bytes((0x4D, 0x10, sequence, 42)) + len(payload).to_bytes(2, "little") + payload[:40])
        payload = payload[40:]
        sequence = (sequence + 1) & 0xFF
    fragments.append(bytes((0x4D, 0x00, sequence, len(payload))) + payload)
    return fragments, (sequence + 1) & 0xFF


class ResponseAssembler:
    def __init__(self) -> None:
        self._payload = bytearray()
        self._remaining: int | None = None
        self._next_sequence: int | None = None

    def feed(self, fragment: bytes) -> dict[str, Any] | None:
        if len(fragment) < 4 or fragment[0] != 0x4D:
            return None
        sequence = fragment[2]
        if self._next_sequence is not None and sequence != self._next_sequence:
            raise ValueError("BLE response fragment sequence mismatch")
        self._next_sequence = (sequence + 1) & 0xFF
        if fragment[1] == 0x14:
            if len(fragment) < 6:
                raise ValueError("short BLE continuation fragment")
            remaining = int.from_bytes(fragment[4:6], "little")
            if self._remaining is not None and remaining != self._remaining:
                raise ValueError("BLE response remaining length mismatch")
            chunk = fragment[6:]
            if len(chunk) > remaining:
                raise ValueError("BLE response fragment exceeds declared length")
            self._remaining = remaining - len(chunk)
            self._payload.extend(chunk)
            return None
        if fragment[1] != 0x04 or fragment[3] != len(fragment) - 4:
            raise ValueError("invalid BLE final fragment")
        chunk = fragment[4:]
        if self._remaining is not None and len(chunk) != self._remaining:
            raise ValueError("BLE response final length mismatch")
        self._payload.extend(chunk)
        plain = _crypt(bytes(self._payload), decrypt=True).rstrip(b"\0")
        self.__init__()
        result = json.loads(plain)
        if not isinstance(result, dict):
            raise ValueError("BLE response is not an object")
        return result


def validate_reply(reply: dict[str, Any], device_id: str, identifier: str) -> None:
    if reply.get("deviceId") != device_id or reply.get("identifier") != identifier or reply.get("code") != 200:
        raise ValueError(f"unexpected BLE {identifier} response")
    data = reply.get("data")
    if not isinstance(data, dict):
        raise ValueError(f"unexpected BLE {identifier} data")
    if identifier == "deviceInfo":
        if reply.get("method") != "get_reply" or data.get("deviceId") != device_id:
            raise ValueError("BLE deviceInfo identity mismatch")
    elif identifier == "inverterCmd":
        if reply.get("method") != "set_reply":
            raise ValueError("unexpected BLE inverterCmd response method")
        if not isinstance(data.get("cmdResult"), str) or data["cmdResult"].lower() != _COMMAND_SUCCESS_REPLY:
            raise ValueError("unexpected BLE inverterCmd DSP acknowledgement")


async def _communicate(address: str, device_id: str, command: bytes | None = None) -> dict[str, Any]:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as exc:
        raise RuntimeError("bleak is not installed; install requirements.lock") from exc
    device = await BleakScanner.find_device_by_address(address, timeout=15)
    expected_name = f"EZ1_{device_id}"
    if not device or device.name != expected_name:
        raise ValueError(f"BLE identity mismatch: expected {expected_name!r}")
    replies: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
    assembler = ResponseAssembler()

    def notification(_sender, data: bytearray) -> None:
        reply = assembler.feed(bytes(data))
        if reply is not None:
            replies.put_nowait(reply)

    sequence = 0
    async with BleakClient(device, timeout=15) as client:
        await client.start_notify(NOTIFY_UUID, notification)

        async def request(identifier: str, request_id: str, payload: bytes | None = None) -> dict[str, Any]:
            nonlocal sequence
            payload = payload or build_request(device_id, expected_name, identifier, request_id)
            fragments, sequence = fragment_request(payload, sequence)
            for fragment in fragments:
                await client.write_gatt_char(WRITE_UUID, fragment, response=True)
            reply = await asyncio.wait_for(replies.get(), timeout=10)
            validate_reply(reply, device_id, identifier)
            return reply

        device_info = await request("deviceInfo", "1")
        if command is not None:
            reply = await request("inverterCmd", "16", command)
            return {"address": address, "device_info": device_info, "command_reply": reply}
        output_data = await request("outputData", "3")
    return {"address": address, "read_only": True, "device_info": device_info, "output_data": output_data}


def read(address: str, device_id: str) -> dict[str, Any]:
    return asyncio.run(_communicate(address, device_id))


def stop(address: str, device_id: str) -> dict[str, Any]:
    return asyncio.run(_communicate(address, device_id, build_stop_request(device_id)))


def limit_30(address: str, device_id: str) -> dict[str, Any]:
    return asyncio.run(_communicate(address, device_id, build_limit_30_request(device_id)))
