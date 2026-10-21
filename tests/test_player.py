"""The Player and publish(): pause/mute with a live position, over the
loopback server with real media."""

import time

import pytest

import tgrtmp

from rtmp_test_server import RtmpTestServer

URL = "rtmp://127.0.0.1:{}/live"


def wait_for(predicate, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return False


def audio_count(server):
    return sum(1 for message in server.media if message[0] == 8)


def video_count(server):
    return sum(1 for message in server.media if message[0] == 9)


@pytest.fixture(scope="module")
def sample10(tmp_path_factory):
    """A 10-second sample: long enough that a mute/unmute cycle happens
    mid-file, before the player reaches the end."""
    import subprocess

    path = str(tmp_path_factory.mktemp("player") / "sample10.ts")
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=160x120:rate=10:duration=10",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=440:duration=10",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-g",
            "30",
            "-pix_fmt",
            "yuv420p",
            "-c:a",
            "aac",
            "-b:a",
            "48k",
            "-f",
            "mpegts",
            path,
        ],
        check=True,
        capture_output=True,
    )
    return path


@pytest.fixture(scope="module")
def sample2(tmp_path_factory):
    """A 2-second sample: short enough that a loop=True player wraps around
    within the test timeout."""
    import subprocess

    path = str(tmp_path_factory.mktemp("player") / "sample2.ts")
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=160x120:rate=10:duration=2",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=440:duration=2",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-g",
            "10",
            "-pix_fmt",
            "yuv420p",
            "-c:a",
            "aac",
            "-b:a",
            "48k",
            "-f",
            "mpegts",
            path,
        ],
        check=True,
        capture_output=True,
    )
    return path


@pytest.fixture()
def factory(media):
    """Hands out (player, server) pairs; tears them down."""
    made = []

    def make(source=None, **kwargs):
        server = RtmpTestServer()
        server.__enter__()

        try:
            source = source if source is not None else media["ts"]
            kwargs.setdefault("log", lambda message: None)
            player = tgrtmp.publish(
                source, URL.format(server.port), "key", freeze_interval=0.1, **kwargs
            )
        except BaseException:
            server.__exit__()
            raise

        made.append((player, server))
        return player, server

    yield make

    for player, server in made:
        player.stop()
        server.__exit__()


def test_position_advances_pause_freezes_wire_stays_alive(factory):
    player, server = factory()
    assert wait_for(lambda: player.position > 0.3), "the position never advanced"

    player.pause()
    time.sleep(1.0)  # Let the chunk in flight drain.
    frozen = player.position
    time.sleep(1.0)
    assert abs(player.position - frozen) < 0.05, "the position ran while paused"

    # A paused player keeps sending the freeze keyframes: the wire must not
    # go silent (the server drops idle sessions).
    sent_at_pause = player.frames_sent
    assert wait_for(lambda: player.frames_sent >= sent_at_pause + 3), (
        "a paused player stopped sending"
    )

    player.play()
    assert wait_for(lambda: player.position > frozen + 0.3), (
        "the position never resumed"
    )


def test_loop_restarts_the_source(factory, sample2):
    player, server = factory(sample2, loop=True)
    assert wait_for(lambda: player.position > 1.2), "the first pass never played"
    assert wait_for(lambda: player.stats["loops"] >= 1 and player.position < 0.8), (
        "the player never looped"
    )
    assert wait_for(lambda: player.position > 1.2), "the second pass never played"
    assert wait_for(lambda: video_count(server) > 40), "looping starved the wire"
    player.stop()


def test_loop_never_spams_when_restart_yields_no_data(factory, sample2):
    """A handle whose seek(0) silently does nothing must not restart forever:
    the player verifies data comes back, then holds the last frame."""

    class LyingSource:
        def __init__(self, path):
            self._file = open(path, "rb")
            self.seekable = lambda: False

        def seek(self, offset):
            pass  # Pretends to seek; reads keep returning b"".

        def read(self, n):
            return self._file.read(n)

    messages = []
    player, server = factory(LyingSource(sample2), loop=True, log=messages.append)
    assert wait_for(lambda: player.position > 1.2), "the first pass never played"

    time.sleep(3)  # The old bug spammed dozens of restarts per second here.

    loops = sum(1 for m in messages if m == "Looping: the source restarted")
    assert loops == 0, f"the player restarted {loops} times without data"
    assert player.stats["loops"] == 0

    sent = player.frames_sent
    assert wait_for(lambda: player.frames_sent >= sent + 3), (
        "the player did not hold the last frame"
    )
    player.stop()


def test_looping_stays_paced_past_the_third_pass(factory, sample2):
    """Pass N must not replay pass N-1's wire timestamps: a colliding offset
    trips the publisher's monotonic clamp, the pacer re-anchors to now, and
    the file blasts out unpaced (loop spam)."""
    player, server = factory(sample2, loop=True)

    assert wait_for(lambda: player.stats["loops"] >= 3, 60), "never looped thrice"

    # The wire keeps real-time pace after the third restart: a 10 fps source
    # yields ~20 frames per 2 s window, not hundreds.
    sent = video_count(server)
    time.sleep(2.0)
    grew = video_count(server) - sent

    assert 1 <= grew <= 60, f"the pacer is not pacing: {grew} frames in 2 s"

    timestamps = [m[1] for m in server.media if m[0] == 9]
    assert all(b > a for a, b in zip(timestamps, timestamps[1:])), (
        "wire timestamps regressed across loop passes"
    )
    player.stop()


@pytest.fixture()
def moov_at_end_mp4(tmp_path, media):
    """A progressive MP4 whose moov index sits at the tail."""
    import subprocess

    path = str(tmp_path / "moov_end.mp4")
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-i", media["ts"], "-c", "copy", path],
        check=True,
        capture_output=True,
    )

    data = open(path, "rb").read()

    if data.find(b"moov") < len(data) // 2:
        pytest.skip("this ffmpeg writes faststart by default")

    return path


@pytest.mark.parametrize("loop", [True, False])
def test_a_tail_moov_mp4_recovers_and_plays(factory, moov_at_end_mp4, loop):
    """A moov-at-end MP4 streams blind to EOF with zero frames; the player
    must recover by reading the index from the tail and replaying."""
    player, server = factory(moov_at_end_mp4, loop=loop)

    assert wait_for(lambda: player.position > 1.0, 30), "the recovered MP4 never played"
    assert wait_for(lambda: video_count(server) > 5), "no video reached the wire"
    assert player.error is None

    if loop:
        assert wait_for(lambda: player.stats["loops"] >= 1, 60), "never looped"

    player.stop()


def test_a_tail_moov_mp4_without_seek_stays_loud(factory, moov_at_end_mp4):
    """A non-seekable handle cannot read its own tail: the player must stop
    with the zero-media error instead of spinning or holding a dead wire."""

    class Unseekable:
        def __init__(self, path):
            self._file = open(path, "rb")
            self.seekable = lambda: False

        def seek(self, offset):
            raise OSError("not seekable")

        def read(self, n):
            return self._file.read(n)

    messages = []
    player, server = factory(Unseekable(moov_at_end_mp4), log=messages.append)

    assert wait_for(lambda: player.stopped, 30), "the player did not stop"
    assert player.error and "no media" in player.error, f"error: {player.error!r}"
    assert player.stats["loops"] == 0
    assert "Looping: the source restarted" not in messages
    player.stop()


def test_stream_recovers_a_tail_moov_mp4(moov_at_end_mp4):
    server = RtmpTestServer()
    server.__enter__()

    try:
        publisher = tgrtmp.stream(moov_at_end_mp4, URL.format(server.port), "key")
        assert publisher.frames_sent > 0, "the recovered MP4 published nothing"
    finally:
        server.__exit__()


def test_stream_raises_on_an_unrecoverable_zero_frame_source(tmp_path):
    """A non-MP4 source that ends with zero frames has no tail to read: the
    blocking API must raise instead of returning a silent publish."""
    # TS transport packets with no PAT/PMT and no frames: the probe accepts
    # them, the demuxer emits nothing, and the tail holds no moov.
    path = tmp_path / "frameless.ts"
    path.write_bytes((b"\x47" + b"\x00" * 187) * 6)

    server = RtmpTestServer()
    server.__enter__()

    try:
        with pytest.raises(tgrtmp.SourceError, match="no media"):
            tgrtmp.stream(str(path), URL.format(server.port), "key")
    finally:
        server.__exit__()


def test_loop_falls_back_to_holding(factory, sample2):
    class Unseekable:
        def __init__(self, path):
            self._file = open(path, "rb")
            self.seekable = lambda: False

        def read(self, n):
            return self._file.read(n)

    events = []
    player, server = factory(
        Unseekable(sample2), loop=True, on_connection=lambda p, up: events.append(up)
    )
    assert wait_for(lambda: player.position > 1.2), "the first pass never played"
    # A live source cannot seek(0): the player must say so and hold the
    # frame instead of dying or spinning.
    assert wait_for(lambda: player.stats["loops"] == 0 and player.frames_sent > 0, 20)
    player.stop()
    assert events == [True, False]


def test_stats_snapshot(factory):
    player, server = factory()
    assert wait_for(lambda: video_count(server) > 3), "the session never sent video"

    stats = player.stats
    assert stats["connected"] and not stats["stopped"] and not stats["paused"]
    assert stats["loops"] == 0
    assert stats["frames_sent"] == player.frames_sent > 0
    assert stats["bytes_sent"] == player.bytes_sent > 0
    assert stats["window_bytes"] is None
    # Both tracks anchor within the same relay batch: the wire skew is small.
    assert abs(stats["av_skew_ms"]) < 100, f"audio is {stats['av_skew_ms']}ms off video"
    assert isinstance(stats["bitrate_kbps"], float)

    # Polled bitrate: real frames flowing means a positive estimate.
    player.stats  # Seed the sampler.
    assert wait_for(lambda: player.stats["bitrate_kbps"] > 0.0), (
        "the bitrate estimate never woke up"
    )
    player.stop()


def test_on_connection_fires(factory):
    events = []
    player, server = factory(on_connection=lambda player, up: events.append(up))
    assert wait_for(lambda: events[:1] == [True]), f"never connected: {events}"
    assert player.connected
    player.stop()
    assert events == [True, False], f"unexpected events: {events}"
    assert not player.connected


def test_on_connection_survives_a_bad_callback(factory):
    events = []

    def callback(player, up):
        events.append(up)
        raise ValueError("user code is buggy")

    player, server = factory(on_connection=callback)
    assert wait_for(lambda: len(events) == 1), f"callback never ran: {events}"
    assert wait_for(lambda: video_count(server) > 3), (
        "the session died with the callback"
    )
    player.stop()
    assert events == [True, False]


def test_media_switch_keeps_one_connection(factory, sample2, sample10):
    """play_next() from on_media_end chains the next file onto the same
    connection: no reconnect, and the stream timeline keeps moving."""
    ends = []

    def on_media_end(player):
        ends.append(round(player.position, 1))

        if len(ends) == 1:
            player.play_next(sample10)

    messages = []
    player, server = factory(sample2, on_media_end=on_media_end, log=messages.append)

    assert wait_for(lambda: len(ends) == 1), "on_media_end never fired"

    # The next file plays past the first one's length: only a switch can
    # move the position that far (a hold freezes it at the first's end).
    assert wait_for(lambda: player.position > 4.0, 30), "the next media never played"

    connects = sum(1 for m in messages if m.startswith("Connected to"))
    assert connects == 1, f"the switch reconnected ({connects} handshakes)"

    # The second file ends too: the callback fires again, and the player
    # holds its last frame with the session still up.
    assert wait_for(lambda: len(ends) == 2, 30), "the second end never fired"
    assert player.connected, "the session dropped while holding"

    sent = player.frames_sent
    assert wait_for(lambda: player.frames_sent >= sent + 3), (
        "the ended player stopped sending"
    )
    player.stop()


def test_play_next_after_the_player_ended_raises(factory, sample2):
    player, _ = factory(sample2)
    player.stop()

    with pytest.raises(RuntimeError):
        player.play_next(sample2)


def test_remove_next_drops_the_queued_media(factory, sample2, sample10):
    ends = []
    player, _ = factory(sample2, on_media_end=lambda player: ends.append(1))

    player.play_next(sample10)
    assert player.remove_next() is True
    assert player.remove_next() is False, "remove lied about the queue"

    # The first file ends and stays ended: nothing takes over, the session
    # keeps holding the last frame.
    assert wait_for(lambda: len(ends) == 1), "on_media_end never fired"

    time.sleep(3)
    assert player.position < 3.5, "the removed media played anyway"
    assert player.connected, "the session dropped instead of holding"

    sent = player.frames_sent
    assert wait_for(lambda: player.frames_sent >= sent + 3), (
        "the ended player stopped sending"
    )
    player.stop()


def test_playlist_plays_in_order_on_one_connection(factory, sample2):
    messages = []
    player, _ = factory(
        sample2,
        playlist=[sample2, sample2],
        log=messages.append,
        on_media_end=lambda p: None,
    )
    playlist = player.playlist

    # The constructor source plays first, then the items in order.
    assert wait_for(lambda: playlist.index == 0, 30), "the first item never played"
    assert wait_for(lambda: playlist.index == 1, 30), "the second item never played"
    assert len(playlist.items) == 2

    connects = sum(1 for m in messages if m.startswith("Connected to"))
    assert connects == 1, f"the playlist reconnected ({connects} handshakes)"

    # The list is drained: the player holds its last frame with the session
    # still up.
    assert player.has_next() is False

    sent = player.frames_sent
    assert wait_for(lambda: player.frames_sent >= sent + 3), (
        "the ended player stopped sending"
    )
    player.stop()


def test_next_skips_to_the_playlist_item(factory, sample2):
    ends = []
    player, _ = factory(
        sample2, playlist=[sample2], on_media_end=lambda player: ends.append(1)
    )

    assert wait_for(lambda: player.position > 0.5), "the first media never played"
    assert player.next() is True
    assert wait_for(lambda: player.playlist.index == 0, 15), "the skip never switched"

    time.sleep(1.5)
    assert ends == [], "the skip fired on_media_end"

    # The last item ends naturally: now the end fires.
    assert wait_for(lambda: ends == [1], 30), "the natural end never fired"
    player.stop()


def test_next_with_an_empty_playlist_is_a_no_op(factory, sample2):
    player, _ = factory(sample2)
    assert wait_for(lambda: player.position > 0.3), "never played"

    assert player.next() is False, "next switched with nothing queued"
    assert player.has_next() is False

    time.sleep(1.5)
    assert player.connected, "the session dropped on a no-op skip"
    player.stop()


def test_a_source_timeline_reset_keeps_the_position_sane(factory, media, tmp_path):
    """Two concatenated copies of one file restart the timestamps at the
    seam: a new timeline must reset the position, not drag it negative,
    and the wire must keep flowing through the jump."""
    doubled = str(tmp_path / "doubled.ts")

    with open(media["ts"], "rb") as source:
        data = source.read()

    with open(doubled, "wb") as out:
        out.write(data * 2)

    player, _ = factory(doubled)

    # The seam restarts the timeline: after playing into the first copy,
    # the position visibly falls back (wherever the clean frames resume)
    # and never below zero.
    seen_high = False
    low = None
    deadline = time.monotonic() + 20

    while time.monotonic() < deadline:
        position = player.position

        if position > 2.0:
            seen_high = True
            low = position if low is None else min(low, position)

        if seen_high and position < low - 0.3:
            break

        time.sleep(0.05)
    else:
        pytest.fail("the seam never restarted the timeline")

    assert position >= 0.0, "the position went negative across the seam"
    assert player.connected, "the jump killed the session"

    sent = player.frames_sent
    assert wait_for(lambda: player.frames_sent >= sent + 3), "the wire went silent"
    player.stop()


def test_a_silent_wire_reconnects(factory, sample10, monkeypatch):
    """A session that stops sending while the source keeps producing is a
    zombie the server has given up on: the player must reconnect it."""
    real = tgrtmp.player.Publisher

    class Silent:
        def __init__(self, *args, **kwargs):
            self.inner = real(*args, **kwargs)

        def __getattr__(self, name):
            return getattr(self.inner, name)

        @property
        def frames_sent(self):
            return 0

    monkeypatch.setattr(tgrtmp.player, "Publisher", Silent)
    messages = []
    player, _ = factory(sample10, log=messages.append)
    player._stall_seconds = 1.0

    assert wait_for(
        lambda: sum(1 for m in messages if m.startswith("Connected to")) >= 2, 30
    ), "the silent session was never recycled"
    assert not player.stopped, "the player died instead of reconnecting"
    player.stop()


def test_loop_all_cycles_the_playlist(factory, sample2):
    player, _ = factory(sample2, playlist=[sample2, sample2], loop="all")
    playlist = player.playlist

    assert wait_for(lambda: playlist.index == 0, 45), "the first item never played"
    assert wait_for(lambda: playlist.index == 1, 45), "the second item never played"
    assert wait_for(lambda: playlist.index == 0, 45), "the playlist never wrapped"
    player.stop()


def test_loop_true_is_rejected_with_a_playlist(factory, sample2, sample10):
    with pytest.raises(ValueError):
        factory(sample2, playlist=[sample10], loop=True)


def test_queue_adds_are_rejected_while_looping_one_file(factory, sample2, sample10):
    player, _ = factory(sample2, loop=True)
    assert wait_for(lambda: player.position > 0.3), "never played"

    with pytest.raises(ValueError):
        player.add(sample10)

    with pytest.raises(ValueError):
        player.play_next(sample10)

    player.stop()


def test_loop_is_writable_mid_stream(factory, sample2, sample10):
    player, _ = factory(sample2, playlist=[sample10])

    with pytest.raises(ValueError):
        player.loop = True  # The queue still has pending items.

    player.clear()
    player.loop = True

    with pytest.raises(ValueError):
        player.loop = "sometimes"

    player.stop()


def test_loop_can_be_turned_off_mid_stream(factory, sample2):
    player, _ = factory(sample2, loop=True)
    assert wait_for(lambda: player.position > 0.4), "never played"

    player.loop = False

    # The 2s file ends; a loop would have wrapped the position by now.
    time.sleep(3)
    assert player.stats["loops"] == 0, "the file looped after loop = False"
    assert player.position > 1.7, "the position wrapped: it looped"
    assert player.connected, "the session dropped instead of holding"
    player.stop()


def test_playlist_rejects_removing_the_playing_item(factory, sample2, sample10):
    player, _ = factory(sample2, playlist=[sample10, sample2])
    playlist = player.playlist

    assert wait_for(lambda: playlist.index == 0, 30), "the first item never played"

    with pytest.raises(ValueError):
        player.remove(0)

    with pytest.raises(IndexError):
        player.remove(99)

    player.stop()


def test_add_move_and_clear_control_the_queue(factory, sample2, sample10):
    player, _ = factory(sample2)
    assert wait_for(lambda: player.position > 0.3), "never played"

    assert player.add(sample10) == 0
    assert player.add(sample2) == 1

    player.move(1, 0)
    assert player.playlist.items == [sample2, sample10], "the move did not reorder"

    assert player.remove_next() is True
    assert player.playlist.items == [sample10], "remove_next dropped the wrong item"

    player.clear()
    assert player.playlist.items == [], "clear left items behind"

    player.stop()


def test_mute_drops_audio_only(factory, sample10):
    player, server = factory(source=sample10)
    assert wait_for(lambda: audio_count(server) > 5)
    player.mute()
    video_at_mute = video_count(server)

    # The queued audio (~1 s of look-ahead) drains first, then the wire goes
    # audio-silent while the video keeps flowing.
    deadline = time.monotonic() + 15
    quiet = audio_count(server)
    while time.monotonic() < deadline:
        time.sleep(0.5)
        current = audio_count(server)
        if current == quiet:
            break
        quiet = current
    assert video_count(server) > video_at_mute + 5, "video stalled"
    time.sleep(0.5)
    assert audio_count(server) == quiet, "audio ran while muted"
    player.unmute()
    assert wait_for(lambda: audio_count(server) > quiet + 5)


def test_stats_properties_flow_through(factory):
    player, server = factory()
    assert wait_for(lambda: player.frames_sent > 5)
    assert player.video_frames_sent > 0
    assert player.bytes_sent > 0
    assert player.frames_dropped == 0
    assert player.warnings == player.publisher.warnings  # The 4:3 media warns.
    assert not player.paused and not player.muted and not player.stopped


def test_publish_accepts_an_open_file(factory, media):
    handle = open(media["ts"], "rb")
    player, server = factory(source=handle)
    try:
        assert wait_for(lambda: player.frames_sent > 5)
    finally:
        player.stop()  # The Player does not close what it did not open.
        assert not handle.closed
        handle.close()


def test_stop_joins_the_worker(factory):
    player, server = factory()
    assert wait_for(lambda: player.frames_sent > 5)
    player.stop()
    assert not player._thread.is_alive()
    assert player.stopped


def test_worker_faults_are_never_silent(factory):
    """An unsupported container ends the player loudly, in player.error."""
    import io

    player, server = factory(source=io.BytesIO(b"this is not any container"))
    assert wait_for(lambda: player.error is not None)
    assert "Unsupported stream" in player.error


def test_a_failing_live_source_retries(factory):
    """A source whose reads fail is treated as a flaky live stream: the
    player retries (and reports through the log), it does not die."""

    class Broken:
        def read(self, n):
            raise OSError("socket gone")

    player, server = factory(source=Broken())
    time.sleep(3)
    assert player.error is None  # Still trying, not dead.
    assert not player.stopped


class LoopingFile:
    """An endless live stream that re-emits a file in a loop (fake HLS)."""

    def __init__(self, path, chunk=4096, delay=0.2):
        with open(path, "rb") as handle:
            self.data = handle.read()
        self.at = 0
        self.chunk = chunk
        self.delay = delay

    def read(self, n):
        time.sleep(self.delay)
        out = bytearray()
        need = min(n, self.chunk)
        while len(out) < need:
            take = self.data[self.at : self.at + need - len(out)]
            if not take:
                self.at = 0
                continue
            out += take
            self.at += len(take)
        return bytes(out)


@pytest.fixture(scope="module")
def long_ts(tmp_path_factory):
    """A 60-second sample: long enough that looping it does not reset the
    media timestamps during the test."""
    import subprocess

    path = str(tmp_path_factory.mktemp("live") / "long.ts")
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=160x120:rate=10:duration=60",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=440:duration=60",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-g",
            "30",
            "-pix_fmt",
            "yuv420p",
            "-c:a",
            "aac",
            "-b:a",
            "48k",
            "-f",
            "mpegts",
            path,
        ],
        check=True,
        capture_output=True,
    )
    return path


def test_publish_wraps_a_live_source_without_buffering(factory, long_ts):
    """Default: a non-seekable live source gets no window; pause freezes
    the picture, resume catches the live edge, nothing is buffered."""
    player, server = factory(source=LoopingFile(long_ts))
    try:
        assert player.window_bytes is None, "a live source was buffered by default"
        assert wait_for(lambda: player.position > 1.0), "never started playing"

        player.pause()
        time.sleep(1.0)

        sent_at_pause = player.frames_sent
        assert wait_for(lambda: player.frames_sent >= sent_at_pause + 3), (
            "a paused live player stopped sending"
        )

        player.play()
        assert wait_for(lambda: player.position > 1.0), "never resumed"
        assert player.error is None
    finally:
        pass  # The fixture stops the player.


def test_a_windowed_live_source_holds_its_point(factory, long_ts):
    """buffer_size opts a live source into a rolling window: a pause holds
    its point while the live edge keeps filling the window."""
    player, server = factory(source=LoopingFile(long_ts), buffer_size=5_000_000)
    try:
        assert player.window_bytes is not None, "the live source was not windowed"
        assert wait_for(lambda: player.position > 1.0), "never started playing"

        player.pause()
        time.sleep(1.0)
        frozen = player.position
        time.sleep(1.0)
        assert abs(player.position - frozen) < 0.05, "the position ran while paused"

        player.play()
        assert wait_for(lambda: player.position > frozen + 1.0), "never resumed"
        assert player.error is None
    finally:
        pass  # The fixture stops the player (and the window reader with it).


def test_a_long_pause_falls_back_to_the_buffered_past(factory, long_ts):
    """When a pause outlives the window capacity, the player continues from
    the buffered past and says so in the log."""
    messages = []
    player, server = factory(
        source=LoopingFile(long_ts), buffer_size=50_000, log=messages.append
    )
    try:
        assert wait_for(lambda: player.position > 1.0)
        player.pause()
        time.sleep(4)  # The 50 KB window fills and drops within ~0.3 s.
        player.play()
        assert wait_for(lambda: player.position > 2.0), "never resumed"
        assert any("outlived the rolling window" in m for m in messages), (
            "the fallback was silent"
        )
        assert player.error is None
    finally:
        pass
