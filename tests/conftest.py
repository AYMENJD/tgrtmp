"""Shared fixtures: real H.264/AAC media generated with ffmpeg, with
ffprobe-derived ground truth (packet counts, dimensions, duration)."""

import json
import subprocess

import pytest

WIDTH, HEIGHT, SECONDS, RATE = 320, 240, 3, 30


def _run(*args):
    subprocess.run(args, check=True, capture_output=True)


def _probe(path):
    out = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-count_packets",
            "-show_streams",
            "-of",
            "json",
            path,
        ],
        check=True,
        capture_output=True,
    ).stdout
    streams = json.loads(out)["streams"]
    truth = {}
    for stream in streams:
        kind = stream["codec_type"]
        truth[kind] = {
            "packets": int(stream["nb_read_packets"]),
            "codec": stream["codec_name"],
            "duration": float(stream.get("duration") or 0),
        }
        if kind == "video":
            truth[kind]["width"] = stream["width"]
            truth[kind]["height"] = stream["height"]
    return truth


@pytest.fixture(scope="session")
def media(tmp_path_factory):
    """A 3-second H.264 (320x240@30, keyframes every 30) + AAC (44.1 kHz
    stereo) sample in every container we demux, with ffprobe ground truth."""
    directory = tmp_path_factory.mktemp("media")
    base = directory / "sample"
    mp4 = str(base) + ".mp4"
    _run(
        "ffmpeg",
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"testsrc=size={WIDTH}x{HEIGHT}:rate={RATE}:duration={SECONDS}",
        "-f",
        "lavfi",
        "-i",
        f"sine=frequency=440:duration={SECONDS}",
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-profile:v",
        "baseline",
        "-g",
        str(RATE),
        "-pix_fmt",
        "yuv420p",
        "-c:a",
        "aac",
        "-b:a",
        "64k",
        "-movflags",
        "+faststart",
        mp4,
    )
    files = {"mp4": mp4}
    for name, args in [
        ("ts", ["-c", "copy", "-f", "mpegts"]),
        ("mkv", ["-c", "copy"]),
        (
            "fmp4",
            ["-c", "copy", "-movflags", "+frag_keyframe+empty_moov+default_base_moof"],
        ),
    ]:
        path = (
            str(base)
            + "_"
            + name
            + (".ts" if name == "ts" else ".mp4" if name == "fmp4" else ".mkv")
        )
        _run("ffmpeg", "-y", "-i", mp4, *args, path)
        files[name] = path
    files["truth"] = _probe(mp4)
    files["width"] = WIDTH
    files["height"] = HEIGHT
    files["seconds"] = SECONDS
    files["rate"] = RATE
    return files


def demux(path, chunk_size=65536):
    """Run a file through the demuxer; returns the collector."""
    import tgrtmp._core as core

    collector = core.CollectorSink()
    source = core.Source(collector)
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            source.feed(chunk)
    source.finish()
    return collector
