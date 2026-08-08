import json

import pytest

from host.ez1_ble import (
    ResponseAssembler,
    _crypt,
    build_limit_30_request,
    build_request,
    build_stop_request,
    fragment_request,
    validate_reply,
)


def test_captured_device_info_request_and_response_framing():
    payload = build_request("E17010020001", "EZ1_E17010020001", "deviceInfo", "1")
    fragments, _ = fragment_request(payload)
    assert fragments[0].hex() == "4d10002a800139616130613039643438336530643063343262383935646532663165663430333463643834636639"

    reply = json.dumps({"deviceId": "E17010020001", "identifier": "deviceInfo", "code": 200}, separators=(",", ":")).encode()
    reply += b"\0" * (-len(reply) % 16)
    encrypted = _crypt(reply)
    assembler = ResponseAssembler()
    assert assembler.feed(bytes((0x4D, 0x14, 7, 42)) + len(encrypted).to_bytes(2, "little") + encrypted[:40]) is None
    decoded = assembler.feed(bytes((0x4D, 0x04, 8, len(encrypted) - 40)) + encrypted[40:])
    assert decoded == {"deviceId": "E17010020001", "identifier": "deviceInfo", "code": 200}


def test_failsafe_commands_are_fixed():
    def decode(payload: bytes) -> dict:
        return json.loads(_crypt(bytes.fromhex(payload.decode()), decrypt=True).rstrip(b"\0"))

    stopped = decode(build_stop_request("E17010020001"))
    limited = decode(build_limit_30_request("E17010020001"))
    assert stopped["params"] == {"cmd": "FBFB06AAE1000000000191FEFE"}
    assert limited["params"] == {"cmd": "FBFB06AAE10000031F01B3FEFE"}
    assert stopped["identifier"] == limited["identifier"] == "inverterCmd"
    assert stopped["method"] == limited["method"] == "set"


@pytest.mark.parametrize(
    "fragment,message",
    [
        (bytes((0x4D, 0x14, 1, 42, 2, 0)) + b"abc", "exceeds declared"),
        (bytes((0x4D, 0x03, 1, 0)), "invalid BLE final"),
        (bytes((0x4D, 0x14, 1, 42)), "short BLE continuation"),
    ],
)
def test_response_assembler_rejects_malformed_fragments(fragment, message):
    with pytest.raises(ValueError, match=message):
        ResponseAssembler().feed(fragment)


def test_response_assembler_rejects_sequence_and_final_length_mismatch():
    assembler = ResponseAssembler()
    assert assembler.feed(bytes((0x4D, 0x14, 7, 42, 16, 0)) + b"12345678") is None
    with pytest.raises(ValueError, match="sequence mismatch"):
        assembler.feed(bytes((0x4D, 0x04, 9, 8)) + b"12345678")

    assembler = ResponseAssembler()
    assert assembler.feed(bytes((0x4D, 0x14, 7, 42, 16, 0)) + b"12345678") is None
    with pytest.raises(ValueError, match="final length mismatch"):
        assembler.feed(bytes((0x4D, 0x04, 8, 7)) + b"1234567")


def valid_reply(identifier: str) -> dict:
    return {
        "deviceId": "EZ1-TEST",
        "identifier": identifier,
        "code": 200,
        "method": "get_reply" if identifier == "deviceInfo" else "set_reply",
        "data": (
            {"deviceId": "EZ1-TEST"}
            if identifier == "deviceInfo"
            else {"cmdResult": "FBFB06DE02000100000000FEFE"}
        ),
    }


def test_ble_replies_require_exact_identity_method_and_dsp_ack():
    validate_reply(valid_reply("deviceInfo"), "EZ1-TEST", "deviceInfo")
    validate_reply(valid_reply("inverterCmd"), "EZ1-TEST", "inverterCmd")

    cases = []
    for path, value in [
        (("deviceId",), "OTHER"),
        (("identifier",), "outputData"),
        (("code",), 500),
        (("method",), "set"),
        (("data",), []),
        (("data", "cmdResult"), "fbfb06de02000100000001fefe"),
    ]:
        reply = valid_reply("inverterCmd")
        if len(path) == 1:
            reply[path[0]] = value
        else:
            reply[path[0]][path[1]] = value
        cases.append(reply)
    for reply in cases:
        with pytest.raises(ValueError):
            validate_reply(reply, "EZ1-TEST", "inverterCmd")
