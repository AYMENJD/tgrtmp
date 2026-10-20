"""A compact pure-Python reference implementation of the same wire protocol
(AMF0, chunk stream, FLV tags), used as a test oracle and as the benchmark
baseline. Written independently, from the same public specs."""

import struct


NUMBER, BOOLEAN, STRING, OBJECT, NULL, UNDEFINED = 0x00, 0x01, 0x02, 0x03, 0x05, 0x06
ECMA_ARRAY, OBJECT_END, STRICT_ARRAY, DATE, LONG_STRING = 0x08, 0x09, 0x0A, 0x0B, 0x0C


def amf_encode(value, ecma=False):
    if value is None:
        return bytes([NULL])
    if isinstance(value, bool):
        return bytes([BOOLEAN, 1 if value else 0])
    if isinstance(value, (int, float)):
        return bytes([NUMBER]) + struct.pack(">d", float(value))
    if isinstance(value, str):
        raw = value.encode()
        if len(raw) > 0xFFFF:
            return bytes([LONG_STRING]) + struct.pack(">I", len(raw)) + raw
        return bytes([STRING]) + struct.pack(">H", len(raw)) + raw
    if isinstance(value, list):
        out = bytes([STRICT_ARRAY]) + struct.pack(">I", len(value))
        for item in value:
            out += amf_encode(item)
        return out
    if isinstance(value, dict):
        marker = (
            bytes([ECMA_ARRAY]) + struct.pack(">I", len(value))
            if ecma
            else bytes([OBJECT])
        )
        out = marker
        for key, item in value.items():
            key = key.encode()
            out += struct.pack(">H", len(key)) + key + amf_encode(item)
        return out + b"\x00\x00" + bytes([OBJECT_END])
    raise TypeError(value)


def amf_decode(data, offset=0):
    marker = data[offset]
    offset += 1
    if marker == NUMBER:
        return struct.unpack(">d", data[offset : offset + 8])[0], offset + 8
    if marker == BOOLEAN:
        return data[offset] != 0, offset + 1
    if marker == STRING:
        length = struct.unpack(">H", data[offset : offset + 2])[0]
        return data[offset + 2 : offset + 2 + length].decode(), offset + 2 + length
    if marker == LONG_STRING:
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        return data[offset + 4 : offset + 4 + length].decode(), offset + 4 + length
    if marker in (NULL, UNDEFINED):
        return None, offset
    if marker in (ECMA_ARRAY, OBJECT):
        if marker == ECMA_ARRAY:
            offset += 4
        result = {}
        while True:
            length = struct.unpack(">H", data[offset : offset + 2])[0]
            if length == 0 and data[offset + 2] == OBJECT_END:
                return result, offset + 3
            key = data[offset + 2 : offset + 2 + length].decode()
            value, offset = amf_decode(data, offset + 2 + length)
            result[key] = value
    if marker == STRICT_ARRAY:
        count = struct.unpack(">I", data[offset : offset + 4])[0]
        offset += 4
        result = []
        for _ in range(count):
            value, offset = amf_decode(data, offset)
            result.append(value)
        return result, offset
    if marker == DATE:
        return struct.unpack(">d", data[offset : offset + 8])[0], offset + 10
    raise ValueError(f"unsupported marker {marker}")


def chunk_write(csid, type_id, message_stream_id, timestamp, payload, chunk_size=4096):
    extended = timestamp >= 0xFFFFFF
    out = bytearray()
    # Basic header: format bits (top two) + chunk stream id escape forms.
    if csid < 64:
        out.append(csid)
    elif csid < 320:
        out.append(0x00)  # Format 0 + escape marker.
        out.append(csid - 64)
    else:
        out.append(0x01)  # Format 0 + 2-byte escape marker.
        rest = csid - 64
        out.append(rest & 0xFF)
        out.append((rest >> 8) & 0xFF)
    out += struct.pack(">I", 0xFFFFFF if extended else timestamp)[1:]
    out += struct.pack(">I", len(payload))[1:]
    out.append(type_id)
    out += struct.pack("<I", message_stream_id)
    if extended:
        out += struct.pack(">I", timestamp)
    offset = 0
    while offset < len(payload) or (len(payload) == 0 and offset == 0):
        if offset > 0:
            if csid < 64:
                out.append(0xC0 | csid)
            elif csid < 320:
                out.append(0xC0)
                out.append(csid - 64)
            else:
                out.append(0xC1)
                rest = csid - 64
                out.append(rest & 0xFF)
                out.append((rest >> 8) & 0xFF)
            if extended:
                out += struct.pack(">I", timestamp)
        out += payload[offset : offset + chunk_size]
        offset += chunk_size
        if len(payload) == 0:
            break
    return bytes(out)


def avc_sequence_header(avcc):
    return b"\x17\x00\x00\x00\x00" + avcc


def avc_frame(nal_units, keyframe, composition_time):
    return (
        bytes([(1 if keyframe else 2) << 4 | 7, 1])
        + struct.pack(">I", composition_time & 0xFFFFFF)[1:]
        + nal_units
    )


def aac_sequence_header(asc):
    return b"\xaf\x00" + asc


def aac_frame(raw):
    return b"\xaf\x01" + raw
