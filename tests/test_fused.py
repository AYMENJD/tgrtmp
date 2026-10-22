"""The fused video stream path must be byte-identical to the staged one:
write_avc_frame(annexb) == write_message(avc_frame(to_avcc(annexb)))."""

import random

import tgrtmp._core as core


def staged(annexb, keyframe, composition, timestamp, chunk_size):
    body = bytes(core.avc_frame(bytes(core.to_avcc(annexb)), keyframe, composition))
    return bytes(core.chunk_write(6, 9, 1, timestamp, body, chunk_size))


def fused(annexb, keyframe, composition, timestamp, chunk_size):
    return bytes(
        core.chunk_write_avc(6, 1, timestamp, keyframe, annexb, composition, chunk_size)
    )


def test_equivalence_known_cases():
    sps = b"\x67\x42\x00\x1e" + b"\x00" * 16
    pps = b"\x68\xcb\x83\x00\x00"
    idr = b"\x65" + bytes(range(256)) * 40
    annexb = (
        b"\x00\x00\x00\x01" + sps + b"\x00\x00\x00\x01" + pps + b"\x00\x00\x01" + idr
    )
    for chunk_size in (128, 4096, 64, 7):
        for timestamp in (0, 33, 0xFFFFFF, 0xFFFFFF + 5):
            for composition in (0, 1, 0x123456, (2**24 - 1)):
                assert staged(
                    annexb, True, composition, timestamp, chunk_size
                ) == fused(annexb, True, composition, timestamp, chunk_size), (
                    chunk_size,
                    timestamp,
                    composition,
                )


def test_equivalence_fuzz():
    rng = random.Random(42)
    for _ in range(300):
        units = [
            bytes(rng.randrange(256) for _ in range(rng.randrange(0, 300)))
            for _ in range(rng.randrange(1, 6))
        ]
        # Mix 3- and 4-byte start codes.
        annexb = b""
        for index, unit in enumerate(units):
            annexb += b"\x00\x00\x00\x01" if index % 2 else b"\x00\x00\x01"
            annexb += unit
        keyframe = rng.random() < 0.5
        composition = rng.randrange(2**24)
        timestamp = rng.choice(
            [0, rng.randrange(0xFFFFFF), rng.randrange(0xFFFFFF, 2**32)]
        )
        chunk_size = rng.choice([1, 5, 128, 4096, 65536])
        assert staged(annexb, keyframe, composition, timestamp, chunk_size) == fused(
            annexb, keyframe, composition, timestamp, chunk_size
        )
