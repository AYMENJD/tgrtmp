"""The Publisher: keyframe gating, sequence headers, metadata, real-time
pacing, discontinuities, and reconnect state resets; exercised over the
loopback server."""

import time

import pytest

import tgrtmp._core as core

from rtmp_test_server import RtmpTestServer

URL = "rtmp://127.0.0.1:{}/live"

# A synthetic 2-byte ASC: AAC-LC, 48 kHz, stereo.
ASC = b"\x12\x10"
# A minimal Annex-B access unit: SPS, PPS, IDR (payloads need not be
# bitstream-valid for the framing logic under test).
SPS = b"\x67\x42\x00\x1e\xda\x01\x40\x16\xec\x04\x40\x00\x00\x03\x00\x40\x00\x00\x0c\xa0\x00\x00\x57\x80\x01"
PPS = b"\x68\xcb\x83\x00\x00"
IDR = b"\x65\x00" + b"\x00" * 40
NON_IDR = b"\x41\x00" + b"\x00" * 40


def keyframe():
    return (
        b"\x00\x00\x00\x01"
        + SPS
        + b"\x00\x00\x00\x01"
        + PPS
        + b"\x00\x00\x00\x01"
        + IDR
    )


def interframe():
    return b"\x00\x00\x00\x01" + NON_IDR


@pytest.fixture()
def session():
    """A server, a client and an attached publisher."""
    server = RtmpTestServer()
    server.__enter__()
    client = core.connect(URL.format(server.port), "key")
    publisher = core.Publisher()
    publisher.attach(client)
    yield server, client, publisher
    publisher.stop()
    client.close()
    server.__exit__()


def wait_for(predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return False


def test_gating_metadata_and_sequence_headers(session):
    server, client, publisher = session
    # Inter frames before any keyframe are dropped.
    for index in range(5):
        publisher.feed_video(index * 33, index * 33, interframe(), False, b"")
    time.sleep(0.1)
    assert server.media == []

    # The first keyframe sends the metadata (a data message, seen by the
    # server as server.metadata), the sequence header and itself.
    publisher.feed_video(0, 0, keyframe(), True, b"AVCCCONFIG")
    assert wait_for(lambda: len(server.media) >= 2)
    assert server.metadata is not None
    assert server.metadata["videocodecid"] == 7.0
    # The AVC sequence header precedes the keyframe on the stream.
    assert server.media[0][0] == 9
    assert server.media[0][3] == b"\x17\x00\x00\x00\x00" + b"AVCCCONFIG"
    assert server.media[1][0] == 9
    assert server.media[1][3][:2] == b"\x17\x01"

    # Inter frames flow now.
    publisher.feed_video(33, 40, interframe(), False, b"")
    assert wait_for(lambda: len(server.media) >= 3)
    assert server.media[2][3][:2] == b"\x27\x01"  # (2 << 4) | 7: inter + AVC.


def test_audio_sequence_header_and_frames(session):
    server, client, publisher = session
    publisher.feed_audio(0, ASC, b"A0")
    publisher.feed_audio(21, ASC, b"A1")
    assert wait_for(lambda: len(server.media) >= 3)
    # The metadata went out first (a data message); then the sequence header
    # and both frames, in order, as audio messages.
    assert server.metadata is not None
    assert server.media[0][0] == 8
    assert server.media[0][3] == b"\xaf\x00" + ASC
    assert server.media[1][3] == b"\xaf\x01A0"
    assert server.media[2][3] == b"\xaf\x01A1"
    # Timestamps are anchored to the wall clock (the reference behavior):
    # the deltas match the media clock, the monotonic clamp adds ±1 ms.
    assert server.media[1][1] <= 3  # The first frame lands right at the epoch.
    assert 19 <= server.media[2][1] - server.media[1][1] <= 22
    # No new sequence header while the ASC is unchanged.
    before = len(server.media)
    publisher.feed_audio(42, ASC, b"A2")
    assert wait_for(lambda: len(server.media) > before)
    assert server.media[-1][3] == b"\xaf\x01A2"

    # A changed ASC re-sends the sequence header before the frame.
    publisher.feed_audio(64, b"\x14\x08", b"A3")  # 44.1 kHz mono
    assert wait_for(lambda: len(server.media) > before + 2)
    assert server.media[-2][3] == b"\xaf\x00" + b"\x14\x08"
    assert server.media[-1][3] == b"\xaf\x01A3"


def test_a_discontinuity_storm_still_feeds_the_stream(session):
    """A restarting source flags a discontinuity on packet after packet;
    only the first may re-anchor: a generation bump per jump would clear
    the queues under the sender's feet and silence the stream."""
    server, client, publisher = session

    publisher.feed_video(0, 0, keyframe(), True, b"C")
    assert wait_for(lambda: len(server.media) >= 1)

    before = len(server.media)

    for index in range(30):
        publisher.feed_video(
            1000 + index * 33, 1000 + index * 33, interframe(), False, b""
        )
        publisher.on_discontinuity()

    assert wait_for(lambda: len(server.media) > before + 5, timeout=5), (
        "a discontinuity storm silenced the stream"
    )


def test_pacing_real_time(session):
    server, client, publisher = session
    # 15 video frames at 33 ms and matching audio; sends must be paced.
    start = time.monotonic()
    for index in range(15):
        if index == 0:
            publisher.feed_video(0, 0, keyframe(), True, b"C")
        else:
            publisher.feed_video(index * 33, index * 33, interframe(), False, b"")
        publisher.feed_audio(index * 21, ASC, b"A%d" % index)
        # A bursty feeder (all enqueued instantly) is the point of this test.
    # 15 video + 15 audio messages + 2 sequence headers.
    assert wait_for(lambda: len(server.media) >= 32, timeout=5)
    elapsed = time.monotonic() - start
    # 15 frames x 33 ms ≈ 495 ms of stream: the wall clock must have advanced
    # with it (the pacer), not blasted through instantly.
    assert elapsed >= 0.35, f"sends were not paced (elapsed {elapsed:.3f}s)"
    # And did not stall either (loose upper bound).
    assert elapsed <= 2.0


def test_throttle_blocks_feeder():
    # A tiny look-ahead must throttle a bursty feeder: enqueue 5 seconds of
    # stream with lookahead 100 ms; feed() cannot return faster than real time.
    server = RtmpTestServer()
    server.__enter__()
    client = core.connect(URL.format(server.port), "key")
    publisher = core.Publisher(lookahead_ms=200)
    publisher.attach(client)
    try:
        start = time.monotonic()
        for index in range(60):  # 2 s of 30 fps video.
            if index == 0:
                publisher.feed_video(0, 0, keyframe(), True, b"C")
            else:
                publisher.feed_video(index * 33, index * 33, interframe(), False, b"")
        elapsed = time.monotonic() - start
        assert elapsed >= 1.0, "the look-ahead did not throttle the feeder"
    finally:
        publisher.stop()
        client.close()
        server.__exit__()


def test_discontinuity_keeps_decoder_state(session):
    server, client, publisher = session
    publisher.feed_video(0, 0, keyframe(), True, b"C1")
    publisher.feed_video(33, 40, interframe(), False, b"")
    assert wait_for(lambda: len(server.media) >= 3)
    count = len(server.media)

    # The source jumps: the timelines re-anchor but the decoder state (gates,
    # sequence headers, metadata) stays armed; the reference never resets
    # between files, and a re-sent dimensionless metadata can make the server
    # drop video permanently.
    publisher.on_discontinuity()
    publisher.feed_video(1000000, 1000000, interframe(), False, b"")
    assert wait_for(lambda: len(server.media) >= count + 1)
    assert server.media[count][3][:2] == b"\x27\x01"  # An inter frame flowed.
    # And no fresh sequence header or metadata was sent (config unchanged).
    assert all(m[3][:2] != b"\x17\x00" for m in server.media[count:])
    assert server.teardown == []


def test_reconnect_resets_timeline():
    server = RtmpTestServer()
    server.__enter__()
    publisher = core.Publisher()
    try:
        client = core.connect(URL.format(server.port), "key")
        publisher.attach(client)
        publisher.feed_video(0, 0, keyframe(), True, b"C1")
        assert wait_for(lambda: len(server.media) >= 2)

        # Drop and reconnect: the reference publisher restarts the timeline.
        client.close()
        publisher.detach()
        server2 = RtmpTestServer()
        server2.__enter__()
        try:
            client2 = core.connect(URL.format(server2.port), "key")
            publisher.attach(client2)
            publisher.feed_video(0, 0, keyframe(), True, b"C2")
            assert wait_for(lambda: len(server2.media) >= 2)
            # The sequence header was re-sent on the fresh timeline.
            assert server2.media[0][3] == b"\x17\x00\x00\x00\x00" + b"C2"
            assert server2.media[1][3][:2] == b"\x17\x01"
            assert server2.metadata is not None  # Metadata sent again.
            client2.close()
        finally:
            server2.__exit__()
    finally:
        publisher.stop()
        server.__exit__()


def test_counters(session):
    server, client, publisher = session
    publisher.feed_video(0, 0, keyframe(), True, b"C")
    publisher.feed_audio(0, ASC, b"A")
    assert wait_for(lambda: publisher.frames_sent >= 3)
    assert publisher.bytes_sent > 0
    assert publisher.frames_dropped == 0


def test_publisher_without_client_drops(session):
    server, client, publisher = session
    publisher.detach()
    publisher.feed_video(0, 0, keyframe(), True, b"C")
    publisher.feed_audio(0, ASC, b"A")
    assert wait_for(lambda: publisher.frames_dropped >= 2, timeout=2)


def test_mid_gop_join_holds_audio_for_keyframe():
    """The regression test for the audio-only-livestream bug: a live source
    joined mid-GOP announces video from its container (expect_video), the
    first video frames are inter frames, and audio must HOLD until the first
    keyframe with its configuration; a dimensionless onMetaData sent before
    that makes Telegram's server commit to audio-only and discard video."""
    with RtmpTestServer() as server:
        client = core.connect(URL.format(server.port), "key")
        publisher = core.Publisher(wait_video_ms=3000)
        publisher.set_expect_video(True)  # What Source learns from the PMT.
        publisher.attach(client)
        try:
            # Mid-GOP: inter frames only, and audio flowing.
            for index in range(5):
                publisher.feed_video(index * 33, index * 33, interframe(), False, b"")
                publisher.feed_audio(index * 21, ASC, b"A%d" % index)
            time.sleep(0.3)
            # Audio held: nothing on the stream yet, no metadata committed.
            assert server.media == []
            assert server.metadata is None
            assert publisher.frames_dropped >= 10

            # The first keyframe (with its configuration record) arrives.
            publisher.feed_video(165, 165, keyframe(), True, b"AVCC")
            assert wait_for(lambda: len(server.media) >= 2, timeout=5)
            # The metadata carried dimensions, never the dimensionless one.
            assert server.metadata is not None
            assert server.metadata["videocodecid"] == 7.0
            # Sequence header, then the keyframe; the audio fed during the
            # hold was dropped (as the reference drops it), fresh audio flows.
            assert server.media[0][3] == b"\x17\x00\x00\x00\x00" + b"AVCC"
            assert server.media[1][3][:2] == b"\x17\x01"
            assert publisher.video_frames_sent >= 2
            publisher.feed_audio(200, ASC, b"A9")
            assert wait_for(lambda: any(m[0] == 8 for m in server.media), timeout=5)
        finally:
            publisher.stop()
            client.close()


def test_audio_only_source_sends_dimensionless_metadata():
    """Without a video track, audio flows after the (short) wait."""
    with RtmpTestServer() as server:
        client = core.connect(URL.format(server.port), "key")
        publisher = core.Publisher(wait_video_ms=100)
        publisher.attach(client)  # expect_video stays false.
        try:
            publisher.feed_audio(0, ASC, b"A0")
            assert wait_for(lambda: len(server.media) >= 2, timeout=5)
            assert server.metadata is not None
            assert "width" not in server.metadata
            assert server.media[0][3] == b"\xaf\x00" + ASC
        finally:
            publisher.stop()
            client.close()


def test_dvr_timeline_never_leaks_onto_the_wire(session):
    """The regression test for the 21-hour-video bug: a live DVR source
    carries media timestamps of tens of hours; every stream message (the
    sequence header included) must carry the wall-derived timestamp, or
    the server sees video hours ahead of audio and drops it."""
    server, client, publisher = session
    huge = 76_518_913  # ~21 hours, a real DVR timeline value.
    publisher.feed_video(huge, huge + 40, keyframe(), True, b"AVCC")
    publisher.feed_video(huge + 33, huge + 73, interframe(), False, b"")
    publisher.feed_audio(huge + 5, ASC, b"A0")
    assert wait_for(lambda: len(server.media) >= 4, timeout=5)
    video_ts = [m[1] for m in server.media if m[0] == 9]
    audio_ts = [m[1] for m in server.media if m[0] == 8]
    # Every timestamp is wall-derived: seconds from the epoch, not hours.
    for ts in video_ts + audio_ts:
        assert ts < 10_000, f"raw media timestamp leaked: {ts}"
    # And audio/video are within a second of each other.
    assert abs(min(video_ts) - min(audio_ts)) < 1000
