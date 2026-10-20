"""The RTMP chunk stream: golden bytes, hand-built sequences, and random
roundtrips against the pure-Python reference."""

import random


import tgrtmp._core as core
from _pyref import chunk_write

CSID, TYPE, STREAM = 3, 20, 0


def write(csid, type_id, stream, timestamp, payload, chunk_size=4096):
    return bytes(
        core.chunk_write(csid, type_id, stream, timestamp, payload, chunk_size)
    )


def parse(data, chunk_size=128):
    parser = core.ChunkParser()
    parser.set_chunk_size(chunk_size)
    return parser.feed(data)


def test_set_chunk_size_message_golden():
    # csid 2, type 1 (Set Chunk Size), stream 0, timestamp 0, payload 4096.
    data = write(2, 1, 0, 0, b"\x00\x00\x10\x00")
    assert data == b"\x02\x00\x00\x00\x00\x00\x04\x01\x00\x00\x00\x00\x00\x00\x10\x00"


def test_single_chunk_message():
    payload = b"A" * 100
    data = write(CSID, TYPE, STREAM, 0, payload, chunk_size=4096)
    assert len(data) == 12 + 100
    assert data[0] == CSID
    assert data[1:4] == b"\x00\x00\x00"  # timestamp
    assert data[4:7] == (100).to_bytes(3, "big")  # length
    assert data[7] == TYPE
    assert data[8:12] == STREAM.to_bytes(4, "little")
    ((type_id, timestamp, stream, body),) = parse(data)
    assert (type_id, timestamp, stream, body) == (TYPE, 0, STREAM, payload)


def test_timestamp_delta_and_types():
    # fmt0 sets the timestamp; a fmt1 message on the same stream inherits it.
    first = write(CSID, TYPE, STREAM, 100, b"x")
    second = write(CSID, TYPE, STREAM, 50, b"y")  # our writer always emits fmt0
    messages = parse(first + second)
    assert [(m[1], m[3]) for m in messages] == [(100, b"x"), (50, b"y")]


def test_multi_chunk_split():
    payload = bytes(range(256)) * 16  # 4096 bytes
    data = write(CSID, TYPE, STREAM, 7, payload, chunk_size=128)
    # 12-byte header + 128, then 31 continuation chunks of 1 + 128.
    assert len(data) == 12 + 4096 + 31
    ((type_id, timestamp, stream, body),) = parse(data, chunk_size=128)
    assert (type_id, timestamp, stream, body) == (TYPE, 7, STREAM, payload)


def test_extended_timestamp():
    timestamp = 0x1234567  # > 0xFFFFFF
    payload = b"z" * 10
    data = write(CSID, TYPE, STREAM, timestamp, payload)
    assert data[1:4] == b"\xff\xff\xff"
    assert int.from_bytes(data[12:16], "big") == timestamp  # extended field
    ((type_id, got_timestamp, stream, body),) = parse(data)
    assert got_timestamp == timestamp
    assert body == payload
    # And it matches the reference writer byte for byte.
    assert data == chunk_write(CSID, TYPE, STREAM, timestamp, payload)


def test_extended_timestamp_multi_chunk_matches_reference():
    payload = b"q" * 10000
    timestamp = 0x1000000
    data = write(CSID, TYPE, STREAM, timestamp, payload, chunk_size=128)
    assert data == chunk_write(CSID, TYPE, STREAM, timestamp, payload, 128)
    ((type_id, got_timestamp, stream, body),) = parse(data, chunk_size=128)
    assert (got_timestamp, body) == (timestamp, payload)


def test_interleaved_streams():
    # Two streams whose chunks interleave at chunk boundaries, as real senders
    # write them; a 100-byte-chunk parser is told the peers' chunk size.
    first = write(4, 8, 1, 0, b"a" * 300, chunk_size=100)
    second = write(6, 9, 2, 0, b"b" * 300, chunk_size=100)
    f1, f2, f3 = first[:112], first[112:213], first[213:]
    s1, s2, s3 = second[:112], second[112:213], second[213:]
    combined = f1 + s1 + f2 + s2 + f3 + s3
    messages = parse(combined, chunk_size=100)
    assert sorted((m[0], m[3][:1], len(m[3])) for m in messages) == [
        (8, b"a", 300),
        (9, b"b", 300),
    ]

    # And byte-by-byte feeding of the same interleaving.
    parser = core.ChunkParser()
    parser.set_chunk_size(100)
    byte_by_byte = []
    for index in range(len(combined)):
        byte_by_byte += parser.feed(combined[index : index + 1])
    assert sorted((m[0], m[3][:1], len(m[3])) for m in byte_by_byte) == [
        (8, b"a", 300),
        (9, b"b", 300),
    ]


def test_abort_discards_partial():
    partial = write(CSID, TYPE, STREAM, 0, b"a" * 300, chunk_size=100)[:200]
    parser = core.ChunkParser()
    parser.set_chunk_size(100)
    parser.feed(partial)
    parser.abort_stream(CSID)
    # Finishing the (now aborted) stream delivers nothing.
    assert parser.feed(write(CSID, TYPE, STREAM, 0, b"", chunk_size=100)) == []


def test_byte_by_byte_feeding():
    payload = b"m" * 1000
    data = write(CSID, TYPE, STREAM, 42, payload, chunk_size=64)
    parser = core.ChunkParser()
    parser.set_chunk_size(64)
    messages = []
    for index in range(len(data)):
        messages += parser.feed(data[index : index + 1])
    assert messages == [(TYPE, 42, STREAM, payload)]


def test_reference_writer_agrees_random():
    rng = random.Random(1234)
    for _ in range(200):
        csid = rng.choice([2, 3, 4, 5, 6, 100, 400])
        type_id = rng.choice([1, 2, 3, 4, 5, 6, 8, 9, 18, 20])
        stream = rng.randrange(0, 2**32)
        timestamp = rng.choice([0, 5, 0xFFFFFF - 1, 0xFFFFFF, 0xFFFFFF + 7, 2**32 - 1])
        length = rng.randrange(0, 5000)
        chunk_size = rng.choice([1, 16, 128, 127, 4096])
        payload = bytes(rng.randrange(256) for _ in range(length))
        ours = write(csid, type_id, stream, timestamp, payload, chunk_size)
        assert ours == chunk_write(
            csid, type_id, stream, timestamp, payload, chunk_size
        )
        # The parser is told the peer's chunk size, as RTMP negotiates it.
        got = parse(ours, chunk_size=chunk_size)
        assert got == [(type_id, timestamp, stream, payload)]


def test_large_csid_roundtrip():
    for csid in (64, 65, 319, 320, 65599):
        payload = b"k" * 300
        data = write(csid, TYPE, STREAM, 0, payload, chunk_size=128)
        ((type_id, timestamp, stream, body),) = parse(data, chunk_size=128)
        assert body == payload
