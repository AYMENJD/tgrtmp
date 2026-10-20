"""Golden-vector tests for the AMF0 codec, cross-checked against the
independent pure-Python reference in _pyref.py."""

import pytest

import tgrtmp._core as core
from _pyref import amf_decode


def encode(value, ecma_arrays=False):
    return bytes(core.amf0_encode(value, ecma_arrays))


def test_number():
    assert encode(1.0) == b"\x00\x3f\xf0\x00\x00\x00\x00\x00\x00"
    assert encode(0.0) == b"\x00" + b"\x00" * 8
    assert encode(-1.5) == b"\x00" + b"\xbf\xf8\x00\x00\x00\x00\x00\x00"
    # ints encode as numbers
    assert encode(2) == encode(2.0)


def test_boolean():
    assert encode(True) == b"\x01\x01"
    assert encode(False) == b"\x01\x00"


def test_string():
    assert encode("connect") == b"\x02\x00\x07connect"
    assert encode("") == b"\x02\x00\x00"


def test_long_string():
    value = "x" * 0x10000
    encoded = encode(value)
    assert encoded[0] == 0x0C
    assert int.from_bytes(encoded[1:5], "big") == 0x10000
    assert amf_decode(encoded)[0] == value


def test_null_and_undefined():
    assert encode(None) == b"\x05"
    assert amf_decode(b"\x06")[0] is None


def test_object():
    encoded = encode({"app": "s", "type": "nonprivate"})
    expected = (
        b"\x03"
        + b"\x00\x03app"
        + encode("s")
        + b"\x00\x04type"
        + encode("nonprivate")
        + b"\x00\x00\x09"
    )
    assert encoded == expected


def test_ecma_array():
    encoded = encode({"videocodecid": 7.0}, ecma_arrays=True)
    assert encoded[0] == 0x08
    assert int.from_bytes(encoded[1:5], "big") == 1
    assert amf_decode(encoded)[0] == {"videocodecid": 7.0}


def test_strict_array():
    encoded = encode([1.0, "a", None])
    assert encoded[0] == 0x0A
    assert int.from_bytes(encoded[1:5], "big") == 3
    assert amf_decode(encoded)[0] == [1.0, "a", None]


def test_date():
    import struct

    # DATE is decode-only for us: 8 bytes of double + 2 reserved.
    payload = b"\x0b" + struct.pack(">d", 1234.5) + b"\x00\x00"
    assert amf_decode(payload) == (1234.5, 11)


def test_nested_roundtrip():
    value = {
        "code": "NetConnection.Connect.Success",
        "level": "status",
        "description": "Connection succeeded.",
        "objectEncoding": 0.0,
        "data": [1.0, 2.5, True, None, {"x": "y"}],
    }
    for ecma in (False, True):
        decoded, _ = amf_decode(encode(value, ecma_arrays=ecma))
        assert decoded == value


def test_connect_command_vector():
    # The bytes of the reference connect command, decoded and re-encoded.
    body = b"".join(
        [
            encode("connect"),
            encode(1.0),
            encode(
                {
                    "app": "s",
                    "type": "nonprivate",
                    "flashVer": "FMLE/3.0",
                    "tcUrl": "rtmp://h/s",
                }
            ),
        ]
    )
    values = core.amf0_decode(body)
    assert values[0] == "connect"
    assert values[1] == 1.0
    assert values[2] == {
        "app": "s",
        "type": "nonprivate",
        "flashVer": "FMLE/3.0",
        "tcUrl": "rtmp://h/s",
    }


@pytest.mark.parametrize(
    "data",
    [
        b"",  # empty decodes to []
        b"\x02\x00\x10short",  # truncated string
        b"\x00",  # truncated number
        b"\x09",  # bare object end
        b"\x03\x00\x03app\x02\x00\x01s",  # object without terminator
        b"\x63",  # unsupported marker
    ],
)
def test_malformed(data):
    if data == b"":
        assert core.amf0_decode(data) == []
        return
    with pytest.raises(RuntimeError):
        core.amf0_decode(data)
