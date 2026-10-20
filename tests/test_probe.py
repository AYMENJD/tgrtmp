"""needs_transcode(): the verdict must match what publishing would do,
because it IS the publishing demuxers answering."""

import subprocess

import pytest

import tgrtmp


@pytest.fixture(scope="module")
def moov_at_end(tmp_path_factory, media):
    """A progressive MP4 whose moov index sits at the tail."""
    path = str(tmp_path_factory.mktemp("probe") / "moov_end.mp4")
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-i", media["ts"], "-c", "copy", path],
        check=True,
        capture_output=True,
    )

    data = open(path, "rb").read()

    if data.find(b"moov") < len(data) // 2:
        pytest.skip("this ffmpeg writes faststart by default")

    return path


def _encode(path, *args):
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", *args, path], check=True, capture_output=True
    )
    return path


@pytest.mark.parametrize("kind", ["mp4", "ts", "mkv", "fmp4"])
def test_supported_media_passes(media, kind):
    assert tgrtmp.needs_transcode(media[kind]) is False


def test_a_tail_moov_mp4_passes(moov_at_end):
    assert tgrtmp.needs_transcode(moov_at_end) is False


def test_an_open_file_passes(media):
    with open(media["mp4"], "rb") as handle:
        assert tgrtmp.needs_transcode(handle) is False
        assert handle.tell() == 0, "the handle was not rewound"


def test_no_video_track_needs_a_transcode(tmp_path, media):
    path = _encode(
        str(tmp_path / "audio_only.mp4"), "-i", media["mp4"], "-vn", "-c:a", "copy"
    )
    assert tgrtmp.needs_transcode(path) is True


def test_a_non_h264_video_track_needs_a_transcode(tmp_path, media):
    path = _encode(
        str(tmp_path / "mpeg4.mp4"),
        "-i",
        media["mp4"],
        "-c:v",
        "mpeg4",
        "-c:a",
        "copy",
    )
    assert tgrtmp.needs_transcode(path) is True


def test_a_non_h264_ts_warns_into_a_transcode(tmp_path, media):
    path = _encode(
        str(tmp_path / "mpeg2.ts"),
        "-i",
        media["ts"],
        "-c:v",
        "mpeg2video",
        "-c:a",
        "copy",
        "-f",
        "mpegts",
    )
    assert tgrtmp.needs_transcode(path) is True


def test_an_unsupported_container_needs_a_transcode(tmp_path, media):
    path = _encode(
        str(tmp_path / "clip.avi"),
        "-i",
        media["mp4"],
        "-c:v",
        "mpeg4",
        "-c:a",
        "copy",
    )
    assert tgrtmp.needs_transcode(path) is True


def test_garbage_and_tiny_files_need_a_transcode(tmp_path):
    raw = tmp_path / "raw.bin"
    raw.write_bytes(b"\xde\xad\xbe\xef" * 64)

    assert tgrtmp.needs_transcode(str(raw)) is True

    tiny = tmp_path / "tiny.mp4"
    tiny.write_bytes(b"\x00\x00\x00")

    assert tgrtmp.needs_transcode(str(tiny)) is True

    empty = tmp_path / "empty.mp4"
    empty.touch()

    assert tgrtmp.needs_transcode(str(empty)) is True


def test_a_missing_path_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        tgrtmp.needs_transcode(str(tmp_path / "nope.mp4"))


def test_a_live_handle_is_rejected():
    class Live:
        def read(self, n):
            return b""

    with pytest.raises(ValueError):
        tgrtmp.needs_transcode(Live())
