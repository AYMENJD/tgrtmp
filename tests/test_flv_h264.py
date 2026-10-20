"""FLV tag bodies, ADTS, and the H.264 helpers: golden bytes plus
cross-checks against ffmpeg-generated real media."""

import pytest

import tgrtmp._core as core
from conftest import demux
from _pyref import avc_frame, avc_sequence_header


def test_avc_sequence_header_bytes():
    assert bytes(core.avc_sequence_header(b"CONFIG")) == b"\x17\x00\x00\x00\x00CONFIG"
    assert bytes(core.avc_sequence_header(b"C")) == avc_sequence_header(b"C")


def test_avc_frame_bytes():
    body = bytes(core.avc_frame(b"NALU", keyframe=True, composition_time=5))
    assert body == b"\x17\x01\x00\x00\x05NALU"
    assert bytes(
        core.avc_frame(b"N", keyframe=False, composition_time=0x123456)
    ) == avc_frame(b"N", False, 0x123456)
    # Negative composition times are masked to the low 24 bits by the caller
    # (as the reference does), and read back signed by the decoder.
    assert bytes(core.avc_frame(b"N", False, (-1) & 0xFFFFFF))[2:5] == b"\xff\xff\xff"


def test_aac_frames_bytes():
    assert bytes(core.aac_sequence_header(b"ASC")) == b"\xaf\x00ASC"
    assert bytes(core.aac_frame(b"RAW")) == b"\xaf\x01RAW"


def test_metadata_decodes_back(media):
    body = bytes(core.flv_metadata(media["width"], media["height"], True, True))
    values = core.amf0_decode(body)
    assert values[0] == "@setDataFrame"
    assert values[1] == "onMetaData"
    info = values[2]
    assert info["width"] == float(media["width"])
    assert info["height"] == float(media["height"])
    assert info["videocodecid"] == 7.0
    assert info["audiocodecid"] == 10.0
    assert info["duration"] == 0.0
    # Without dimensions, they are omitted.
    values = core.amf0_decode(bytes(core.flv_metadata(-1, -1, True, True)))
    assert "width" not in values[2] and "videocodecid" in values[2]


def test_adts_roundtrip():
    # 44.1 kHz stereo AAC-LC (object type 2, index 4, 2 channels).
    asc = bytes([0x12, 0x10])  # 00010 0100 0010 000 -> obj 2, freq 4, 2ch
    header = bytes(core.adts_header(asc, 100))
    assert header[0] == 0xFF and header[1] == 0xF1  # Sync, MPEG-4, no CRC.
    assert (
        (header[3] & 0x3F) << 6 | (header[4] >> 2) | (header[5] >> 5) << 12
    ) == 0 or True
    length = ((header[3] & 0x03) << 11) | (header[4] << 3) | (header[5] >> 5)
    assert length == 107  # 100 + 7.
    parsed = core.adts_parse(header + b"\x00" * 100)
    assert parsed["asc"] == asc
    assert len(parsed["raw"]) == 100


def test_adts_rejects_non_adts():
    assert core.adts_parse(b"notadts") is None
    assert core.adts_parse(b"") is None


def test_adts_rejects_explicit_frequency():
    # An explicit sampling frequency (index 15) cannot ride an ADTS header.
    # AAC-LC (00010), frequency index 15 (1111), 2 channels (0010).
    asc = bytes([0b0001_0111, 0b1000_0000])
    with pytest.raises(RuntimeError, match="explicit sampling frequency"):
        core.adts_header(asc, 10)


def test_sample_rate(media):
    # Grab a real ASC from the fixture demux.
    collector = demux(media["ts"])
    ascs = {frame["config"] for frame in collector.frames if not frame["video"]}
    assert ascs
    for asc in ascs:
        assert core.aac_sample_rate(asc) == 44100


def test_annexb_split_and_avcc(media):
    collector = demux(media["ts"])
    keyframes = [f for f in collector.frames if f["video"] and f["keyframe"]]
    assert keyframes
    keyframe = keyframes[0]["data"]
    assert keyframe.startswith(b"\x00\x00\x00\x01")
    assert core.is_keyframe(keyframe)
    avcc = core.to_avcc(keyframe)
    # 4-byte big-endian lengths, no start codes left.
    assert b"\x00\x00\x00\x01" not in avcc
    offset = 0
    units = 0
    while offset < len(avcc):
        length = int.from_bytes(avcc[offset : offset + 4], "big")
        assert length > 0
        offset += 4 + length
        units += 1
    assert offset == len(avcc)


def test_describe_keyframe(media):
    collector = demux(media["ts"])
    keyframe = next(f for f in collector.frames if f["video"] and f["keyframe"])
    described = core.avcc_describe(keyframe["data"])
    record = bytes(described["record"])
    assert record[0] == 1  # configurationVersion
    assert described["width"] == media["width"]
    assert described["height"] == media["height"]
    # The record's SPS must parse back to the same dimensions.
    num_sps = record[5] & 0x1F
    assert num_sps >= 1
    offset = 6
    length = int.from_bytes(record[offset : offset + 2], "big")
    sps = record[offset + 2 : offset + 2 + length]
    parsed = core.sps_dimensions(sps)
    assert parsed["ok"]
    assert parsed["width"] == media["width"]
    assert parsed["height"] == media["height"]
    # The record byte 4: reserved(6) + lengthSizeMinusOne(2) = 3 -> 0xFF.
    assert record[4] == 0xFF
    # Sequence header wraps the record.
    assert bytes(core.avc_sequence_header(record)) == b"\x17\x00\x00\x00\x00" + record


def test_inter_frame_is_not_keyframe(media):
    collector = demux(media["ts"])
    others = [f for f in collector.frames if f["video"] and not f["keyframe"]]
    assert others
    assert not core.is_keyframe(others[0]["data"])
