"""The demuxers against real ffmpeg-generated containers, with ffprobe
ground truth: frame counts, timestamps, keyframes, configuration records,
robustness under hostile chunking, and discontinuity handling."""

import subprocess

import pytest

import tgrtmp._core as core

from conftest import demux


def video_frames(collector):
    return [f for f in collector.frames if f["video"]]


def audio_frames(collector):
    return [f for f in collector.frames if not f["video"]]


def test_ts_demux_matches_ffprobe(media):
    collector = demux(media["ts"])
    truth = media["truth"]
    assert len(video_frames(collector)) == truth["video"]["packets"]
    assert len(audio_frames(collector)) == truth["audio"]["packets"]
    assert collector.jumps == 0


def test_ts_frames_and_config(media):
    collector = demux(media["ts"])
    frames = video_frames(collector)
    # Keyframes every gop (30 frames at rate 30): first frame is one.
    assert frames[0]["keyframe"]
    keyframes = [f for f in frames if f["keyframe"]]
    assert len(keyframes) >= media["seconds"]
    # Keyframes carry the AVCDecoderConfigurationRecord...
    record = bytes(keyframes[0]["config"])
    assert record[:1] == b"\x01"
    # ...which describes the real dimensions.
    described = core.avcc_describe(keyframes[0]["data"])
    assert described["width"] == media["width"]
    assert described["height"] == media["height"]
    # Inter frames carry none.
    assert all(not f["config"] for f in frames if not f["keyframe"])
    # Timestamps advance in lockstep with the 30 fps grid.
    assert frames[1]["dts_ms"] - frames[0]["dts_ms"] in (33, 34)
    # The stream is ~3 s long.
    assert frames[-1]["dts_ms"] > (media["seconds"] - 1) * 1000
    # Audio frames carry the ASC and last ~21-23 ms apart.
    audio = audio_frames(collector)
    assert audio[0]["config"]
    assert core.aac_sample_rate(audio[0]["config"]) == 44100
    assert 0 < audio[1]["pts_ms"] - audio[0]["pts_ms"] <= 23


def test_mp4_faststart(media):
    collector = demux(media["mp4"])
    truth = media["truth"]
    assert len(video_frames(collector)) == truth["video"]["packets"]
    assert len(audio_frames(collector)) == truth["audio"]["packets"]
    frames = video_frames(collector)
    assert frames[0]["keyframe"]
    # MP4 keeps the parameter sets in the avcC record (not in-band).
    record = bytes(frames[0]["config"])
    assert record[:1] == b"\x01"
    # Dimensions through the record's first SPS.
    offset = 6
    length = int.from_bytes(record[offset : offset + 2], "big")
    sps = record[offset + 2 : offset + 2 + length]
    parsed = core.sps_dimensions(sps)
    assert parsed["ok"] and parsed["width"] == media["width"]
    assert parsed["height"] == media["height"]


def test_mp4_fragmented(media):
    collector = demux(media["fmp4"])
    truth = media["truth"]
    assert len(video_frames(collector)) == truth["video"]["packets"]
    assert len(audio_frames(collector)) == truth["audio"]["packets"]
    frames = video_frames(collector)
    assert frames[0]["keyframe"]
    # The avcC record rides the keyframe, with the parameter sets inside.
    record = bytes(frames[0]["config"])
    assert record[:1] == b"\x01"
    assert len(audio_frames(collector)[0]["config"]) >= 2  # The ASC.


def test_matroska(media):
    collector = demux(media["mkv"])
    truth = media["truth"]
    assert len(video_frames(collector)) == truth["video"]["packets"]
    # ffmpeg's AAC-in-MKV uses Xiph lacing: demuxed frames may exceed the
    # packet count is NOT allowed; every packet must come out separately or
    # merged 1:1; count must match exactly.
    assert len(audio_frames(collector)) == truth["audio"]["packets"]
    assert video_frames(collector)[0]["keyframe"]


@pytest.mark.parametrize("chunk_size", [1, 7, 188, 4096])
def test_chunked_feeding_is_identical(media, chunk_size):
    reference = demux(media["ts"], chunk_size=65536)
    hostile = demux(media["ts"], chunk_size=chunk_size)
    assert len(hostile.frames) == len(reference.frames)
    for a, b in zip(reference.frames, hostile.frames):
        assert a["video"] == b["video"] and a["dts_ms"] == b["dts_ms"]
        assert a["data"] == b["data"]


@pytest.mark.parametrize("name", ["mp4", "fmp4", "mkv"])
def test_chunked_feeding_is_identical_per_container(media, name):
    reference = demux(media[name], chunk_size=65536)
    hostile = demux(media[name], chunk_size=11)
    assert len(hostile.frames) == len(reference.frames)
    for a, b in zip(reference.frames, hostile.frames):
        assert a["data"] == b["data"] and a["dts_ms"] == b["dts_ms"]


def test_discontinuity_detected(media, tmp_path):
    # Two concatenated TS files whose timelines restart: a jump is detected.
    shifted = str(tmp_path / "shifted.ts")
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-i",
            media["mp4"],
            "-c",
            "copy",
            "-output_ts_offset",
            "3600",
            shifted,
        ],
        check=True,
        capture_output=True,
    )
    collector = core.CollectorSink()
    source = core.Source(collector)
    with open(media["ts"], "rb") as first, open(shifted, "rb") as second:
        while chunk := first.read(65536):
            source.feed(chunk)
        while chunk := second.read(65536):
            source.feed(chunk)
        source.finish()
    # The second file restarts its timeline 3600 s back: a jump must be seen.
    assert collector.jumps >= 1
    # And the later video frames still flow.
    assert len(video_frames(collector)) > media["truth"]["video"]["packets"]


def test_unsupported_container_rejected():
    source = core.Source(core.CollectorSink())
    with pytest.raises(RuntimeError):
        source.feed(b"definitely not a container" * 4)


def test_ts_entered_mid_packet(media):
    """A byte offset mid-packet (a seek, a spliced capture) must still probe
    as MPEG-TS: the probe scans for a sync byte with a confirmed +188 one."""
    collector = core.CollectorSink()
    source = core.Source(collector)
    with open(media["ts"], "rb") as handle:
        data = handle.read()
    source.feed(data[100:])  # 100 bytes into the first packet.
    source.finish()
    assert collector.frames, "the mid-packet entry demuxed nothing"


def test_spurious_sync_pair_does_not_derail_the_parse(media):
    """Payload bytes that mimic a packet boundary (0x47 at X and X+188) must
    not lock the parser onto a shifted grid where no PSI ever appears: the
    scan needs three consecutive sync bytes."""
    collector = core.CollectorSink()
    source = core.Source(collector)
    with open(media["ts"], "rb") as handle:
        data = handle.read()
    prefix = bytearray(b"\xff" * 300)
    prefix[5] = 0x47  # A spurious boundary, verified by a spurious
    prefix[5 + 188] = 0x47  # successor, but not by a third one.
    source.feed(bytes(prefix) + data)
    source.finish()
    assert collector.frames, "the parser locked onto the spurious sync pair"


def test_hevc_only_stream_fails_loudly(tmp_path):
    # Telegram's server accepts H.264 only: an HEVC-only TS must fail loudly,
    # not publish silence.
    hevc = str(tmp_path / "h.ts")
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=160x120:rate=10:duration=1",
            "-c:v",
            "libx265",
            "-preset",
            "ultrafast",
            "-f",
            "mpegts",
            hevc,
        ],
        check=True,
        capture_output=True,
    )
    with pytest.raises(RuntimeError, match="no H.264 video and no AAC audio"):
        demux(hevc)


def test_mp3_audio_only_ts_fails_loudly(tmp_path):
    # AAC-only server: an MP3-only TS has nothing publishable either.
    mp3 = str(tmp_path / "m.ts")
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "sine=duration=1",
            "-c:a",
            "mp3",
            "-f",
            "mpegts",
            mp3,
        ],
        check=True,
        capture_output=True,
    )
    with pytest.raises(RuntimeError, match="no H.264 video and no AAC audio"):
        demux(mp3)


def test_hevc_with_aac_continues_audio_only_with_warning(tmp_path):
    # HEVC video + AAC audio: the audio still publishes, with a warning.
    mixed = str(tmp_path / "mix.ts")
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=160x120:rate=10:duration=1",
            "-f",
            "lavfi",
            "-i",
            "sine=duration=1",
            "-c:v",
            "libx265",
            "-preset",
            "ultrafast",
            "-c:a",
            "aac",
            "-f",
            "mpegts",
            mixed,
        ],
        check=True,
        capture_output=True,
    )
    collector = core.CollectorSink()
    source = core.Source(collector)
    with open(mixed, "rb") as handle:
        while chunk := handle.read(65536):
            source.feed(chunk)
        source.finish()
    assert video_frames(collector) == []
    assert len(audio_frames(collector)) > 0
    assert any("not H.264" in warning for warning in collector.warnings), (
        collector.warnings
    )
