"""Guards: nothing the Telegram server will not accept may pass silently;
it fails loudly, or warns-and-degrades visibly."""

import contextlib
import time

import pytest

import tgrtmp._core as core

from rtmp_test_server import RtmpTestServer
from test_publisher import ASC, URL, interframe, keyframe, wait_for  # noqa: F401

# An ASC with an explicit sampling frequency (index 15): unparseable for the
# wire, rejected with a clear message by the reference too.
BAD_ASC = bytes([0b0001_0111, 0b1000_0000])


@contextlib.contextmanager
def session():
    """A server, a client and an attached publisher."""
    server = RtmpTestServer()
    server.__enter__()
    client = core.connect(URL.format(server.port), "key")
    publisher = core.Publisher()
    publisher.attach(client)
    try:
        yield server, client, publisher
    finally:
        publisher.stop()
        client.close()
        server.__exit__()


def test_unsupported_audio_dropped_with_warning():
    with session() as (server, client, publisher):
        seen = []
        publisher.set_logger(seen.append)
        publisher.feed_audio(0, BAD_ASC, b"A0")
        publisher.feed_audio(21, BAD_ASC, b"A1")
        publisher.feed_video(0, 0, keyframe(), True, b"AVCC")
        time.sleep(0.2)
        # The unsupported audio never reached the wire; the video flows; and
        # exactly one warning fired (not one per frame).
        assert all(m[0] != 8 for m in server.media)
        assert wait_for(lambda: any(m[0] == 9 for m in server.media))
        drops = [message for message in seen if "Dropping audio" in message]
        assert len(drops) == 1, seen
        assert publisher.warnings >= 1
        # Good audio still flows afterwards.
        publisher.feed_audio(64, ASC, b"A2")
        assert wait_for(lambda: any(m[0] == 8 for m in server.media))


def test_logger_callback_may_touch_the_publisher():
    # The warning fires from inside a locked feed path; the callback must be
    # able to call back into the publisher without deadlocking.
    with session() as (server, client, publisher):
        publisher.set_logger(
            lambda message: (publisher.connected, publisher.frames_sent)
        )
        publisher.feed_video(0, 0, keyframe(), True, b"")  # No config -> warning.
        time.sleep(0.2)
        assert publisher.warnings >= 1


def test_keyframe_without_config_warns():
    with session() as (server, client, publisher):
        seen = []
        publisher.set_logger(seen.append)
        publisher.feed_video(0, 0, keyframe(), True, b"")  # Keyframe, no config.
        time.sleep(0.2)
        assert server.media == []  # Gated.
        assert any("SPS/PPS" in message for message in seen), seen


def test_aspect_ratio_warning_fires_once():
    with session() as (server, client, publisher):
        dims = core.avcc_describe(keyframe())
        if dims["width"] * 9 == dims["height"] * 16:
            pytest.skip("the synthetic keyframe happens to be 16:9")
        seen = []
        publisher.set_logger(seen.append)
        publisher.feed_video(0, 0, keyframe(), True, bytes(dims["record"]))
        publisher.feed_video(33, 33, keyframe(), True, bytes(dims["record"]))
        time.sleep(0.2)
        aspect = [message for message in seen if "aspect ratio" in message]
        assert len(aspect) == 1, seen  # Once, not per keyframe.
        assert wait_for(lambda: any(m[3][:2] == b"\x17\x00" for m in server.media))


def test_audio_hold_expiry_warns():
    with RtmpTestServer() as server:
        client = core.connect(URL.format(server.port), "key")
        publisher = core.Publisher(wait_video_ms=100)
        publisher.set_expect_video(True)  # Video expected...
        publisher.attach(client)
        seen = []
        publisher.set_logger(seen.append)
        try:
            # ...but no keyframe with a config ever arrives: inter frames only.
            for index in range(8):
                publisher.feed_video(index * 33, index * 33, interframe(), False, b"")
                publisher.feed_audio(index * 21, ASC, b"A%d" % index)
                time.sleep(0.03)
            assert any("audio-only" in message for message in seen), seen
            # The stream still published (audio-only), visibly.
            assert any(m[0] == 8 for m in server.media)
        finally:
            publisher.stop()
            client.close()
