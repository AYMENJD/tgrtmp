import collections
import os
import threading
import time
import weakref

from urllib.parse import urlsplit

from ._core import CollectorSink, Publisher, Source, connect
from .client import _connected_message, ZERO_MEDIA, _tail_moov

__all__ = ["Player", "publish"]

CHUNK = 16384  # The size of one demuxer feed; also the pause latency bound.


class _Permanent(RuntimeError):
    pass


class _Stopped(RuntimeError):
    pass


class _Playlist:
    """The ordered media a Player plays through. Internal: control it with
    the Player's own methods (play_next, next, add, remove, ...); every
    method is safe to call while the player runs, including from
    ``on_media_end``."""

    def __init__(self, items=None):
        self._lock = threading.Lock()
        self._items = []
        self._index = -1  # The playing item; -1 before the first one.

        for item in items or ():
            self.add(item)

    def add(self, source):
        """Appends ``source`` and returns its index."""
        with self._lock:
            self._items.append(source)

            return len(self._items) - 1

    def insert(self, index, source):
        """Inserts ``source`` before ``index``, with list semantics."""
        with self._lock:
            index = max(0, min(index, len(self._items)))
            self._items.insert(index, source)

            if index <= self._index:
                self._index += 1

    def insert_next(self, source):
        """Inserts ``source`` to play right after the current media, ahead
        of anything already queued."""
        with self._lock:
            self._items.insert(self._index + 1, source)

    def remove(self, index):
        """Removes the item at ``index``. The playing item cannot be
        removed."""
        with self._lock:
            if not 0 <= index < len(self._items):
                raise IndexError(f"no playlist item at index {index}")

            if index == self._index:
                raise ValueError("the playing item cannot be removed")

            del self._items[index]

            if index < self._index:
                self._index -= 1

    def remove_next(self):
        """Drops the item that would play next; True when one was dropped."""
        with self._lock:
            index = self._index + 1

            if index >= len(self._items):
                return False

            del self._items[index]

        return True

    def move(self, index, to):
        """Moves a pending item to another pending position."""
        with self._lock:
            if not 0 <= index < len(self._items) or not 0 <= to < len(self._items):
                raise IndexError("move needs indexes of existing items")

            if index == self._index or to == self._index:
                raise ValueError("the playing item cannot be moved")

            self._items.insert(to, self._items.pop(index))

    def clear(self):
        """Removes every pending item; the playing media finishes alone."""
        with self._lock:
            del self._items[self._index + 1 :]

    def __len__(self):
        """How many items the list holds, playing one included."""
        with self._lock:
            return len(self._items)

    def __getitem__(self, index):
        with self._lock:
            return self._items[index]

    @property
    def index(self):
        """The playing item's index; None before the first media starts."""
        with self._lock:
            return None if self._index < 0 else self._index

    @property
    def items(self):
        """A snapshot of the sources, playing one included."""
        with self._lock:
            return list(self._items)

    def _advance(self, repeat_all):
        # Worker only: the next source to play, or None at the end. With
        # repeat_all the list cycles back to its first item.
        with self._lock:
            index = self._index + 1

            if index >= len(self._items):
                if not (repeat_all and self._items):
                    return None

                index = 0

            self._index = index

            return self._items[index]

    def _has_next(self, repeat_all):
        with self._lock:
            index = self._index + 1

            if index < len(self._items):
                return True

            return bool(repeat_all and self._items)


def _stop_at_exit(ref):
    # A player someone forgot to stop() releases its connection when the
    # interpreter exits. The native handles are dropped here too: left to
    # interpreter teardown, they outlive their module and nanobind reports
    # them as leaks.
    player = ref()

    if player is None:
        return

    player.stop()

    player._src = None
    player._collector = None
    player._client = None
    player._pub = None
    player._thread = None


class Player:
    """Publishes a file-like source with player controls.

    Prefer :func:`publish`, which accepts paths and live sources directly.
    The worker thread starts immediately; every control is safe to call from
    any thread.
    """

    def __init__(
        self,
        source,
        url,
        key,
        *,
        verify_tls=True,
        connect_timeout=15.0,
        command_timeout=10.0,
        reconnect_delay=1.0,
        max_reconnect_delay=30.0,
        lookahead_ms=1000,
        wait_video_ms=1000,
        freeze_interval=1.0,
        loop=False,
        window_bytes=0,
        playlist=None,
        on_connection=None,
        on_media_end=None,
        log=None,
    ):
        if loop not in (True, False, "all"):
            raise ValueError("loop is True, False, or 'all'")

        if loop is True and playlist is not None:
            raise ValueError(
                "loop=True repeats the first source forever and the playlist "
                'would never play; use loop="all" to cycle the playlist'
            )

        self._source = source
        self._url = url
        self._key = key

        self._verify_tls = verify_tls
        self._connect_timeout = connect_timeout
        self._command_timeout = command_timeout
        self._reconnect_delay = reconnect_delay
        self._max_reconnect_delay = max_reconnect_delay
        self._freeze_interval = max(0.05, float(freeze_interval))
        self._loop = loop
        self._playlist = (
            playlist if isinstance(playlist, _Playlist) else _Playlist(playlist)
        )

        self._loop_base = 0
        self._loops = 0
        self._on_connection = on_connection
        self._on_media_end = on_media_end
        self._connected = False
        self.log = log or (lambda message: None)

        self._pub = Publisher(lookahead_ms=lookahead_ms, wait_video_ms=wait_video_ms)
        self._pub.set_logger(lambda message: self.log(f"warning: {message}"))

        self._collector = CollectorSink()
        self._client = None
        self._managed_source = False  # publish() sets this for paths it opened.

        self._lock = threading.Lock()
        self._playing = True
        self._muted = False
        self._stopped = False
        self._eof = False
        self._hold_eof = False
        self._error = None

        # Set once any frame reaches the relay: a source that hits EOF
        # without ever producing one is broken, not loopable.
        self._saw_media = False

        # The displayed position: the segment base plus the source time since
        # the segment's first frame.
        self._seg_base = 0.0
        self._seg_dts = None
        self._last_dts = 0.0

        # The last keyframe seen: replayed as a freeze frame while paused.
        self._keyframe = None  # (dts_ms, annexb, config)
        self._freeze_dts = None

        # The rolling window (live sources only): a reader thread drains the
        # non-seekable source into it, so a pause holds its point while the
        # live edge moves on. The queue holds the unconsumed bytes.
        self._window_cap = int(window_bytes)
        self._window = collections.deque()
        self._window_bytes = 0
        self._window_cond = threading.Condition()
        self._window_eof = False
        self._window_error = None
        self._fell_behind = False
        self._reader = None

        if self._window_cap > 0:
            self._reader = threading.Thread(target=self._fill_window, daemon=True)
            self._reader.start()

        # Rolling samples of (monotonic, bytes_sent) for the bitrate estimate.
        self._stat_lock = threading.Lock()
        self._samples = collections.deque()

        self._src = None

        # A moov-at-end MP4 recovery: the demuxer is primed with the tail
        # index and the media is replayed with absolute offsets.
        self._primed = False
        self._feed_pos = 0

        # Set to ask the worker to skip to the next playlist item.
        self._skip = False

        # The first chunk read after a loop restart, verified before the
        # restart commits.
        self._pending = b""

        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

        weakref.finalize(self, _stop_at_exit, weakref.ref(self))

    @property
    def position(self):
        """Seconds into the source (freezes while paused)."""
        with self._lock:
            if self._seg_dts is None:
                return self._seg_base

            return self._seg_base + (self._last_dts - self._seg_dts) / 1000.0

    @property
    def paused(self):
        with self._lock:
            return not self._playing

    @property
    def muted(self):
        with self._lock:
            return self._muted

    @property
    def stopped(self):
        """Whether the player has ended: stop() was called, or the worker
        exited (a failed source, a given-up connection)."""
        with self._lock:
            if self._stopped:
                return True

        return not self._thread.is_alive()

    @property
    def error(self):
        """The fault that ended the session, if any."""
        return self._error

    @property
    def connected(self):
        """Whether a session is live right now."""
        return self._connected

    @property
    def window_bytes(self):
        """Bytes buffered in the rolling window (when one is configured; else None)."""
        if self._reader is None:
            return None

        with self._window_cond:
            return self._window_bytes

    @property
    def publisher(self):
        """The underlying Publisher (advanced)."""
        return self._pub

    @property
    def playlist(self):
        """The playlist backing this player, for display: ``index`` (the
        playing item, None before it starts), ``items``, ``len()`` and
        indexing. Control the queue with the Player's own methods."""
        return self._playlist

    @property
    def loop(self):
        """True repeats the current file, ``"all"`` cycles the queue, False
        plays each media once. Safe to change mid-stream: it takes effect
        at the next end of media."""
        return self._loop

    @loop.setter
    def loop(self, value):
        if value not in (True, False, "all"):
            raise ValueError("loop is True, False, or 'all'")

        if value is True and self._playlist._has_next(False):
            raise ValueError(
                "the queue still has pending items; they would never play. "
                'Clear it first (player.clear()), or use loop="all"'
            )

        self._loop = value

    def pause(self):
        with self._lock:
            self._playing = False

    def play(self):
        with self._lock:
            self._playing = True

    def mute(self):
        with self._lock:
            self._muted = True

    def unmute(self):
        with self._lock:
            self._muted = False

    def play_next(self, source):
        """Plays ``source`` right after the current media, ahead of anything
        already queued.

        Accepts a path, an open binary file, or a live byte stream, exactly
        like :func:`publish`. The stream timeline continues past the
        previous media, so viewers never see a rewind, and the switch rides
        the same connection. A player repeating one file (``loop=True``)
        rejects queueing.
        """
        if self.stopped:
            raise RuntimeError("the player has ended; there is nothing left to feed")

        if self._loop is True:
            raise ValueError(
                "the player repeats one file (loop=True); queued items would never play"
            )

        self._playlist.insert_next(source)

    def add(self, source):
        """Appends ``source`` to the playlist and returns its index; it
        plays after everything already queued."""
        if self._loop is True:
            raise ValueError(
                "the player repeats one file (loop=True); queued items would never play"
            )

        return self._playlist.add(source)

    def remove(self, index):
        """Removes the pending item at ``index``. The playing item cannot
        be removed."""
        self._playlist.remove(index)

    def remove_next(self):
        """Drops the item that would play next; True when one was dropped."""
        return self._playlist.remove_next()

    def move(self, index, to):
        """Moves a pending item to another pending position."""
        self._playlist.move(index, to)

    def clear(self):
        """Removes every pending item; the playing media finishes alone."""
        self._playlist.clear()

    def next(self):
        """Finishes the current media early and starts the next playlist
        item on the same connection. False when nothing is queued, and the
        current media keeps playing.
        """
        if not self.has_next():
            return False

        if self.stopped:
            raise RuntimeError("the player has ended")

        with self._lock:
            self._skip = True

        return True

    def has_next(self):
        """Whether :meth:`next` would switch right now."""
        return self._playlist._has_next(self._loop == "all")

    def stop(self):
        with self._lock:
            if self._stopped:
                return

            self._stopped = True

        with self._window_cond:
            self._window_cond.notify_all()

        self._thread.join(timeout=30)

        self._pub.stop()

        if self._managed_source:
            try:
                self._source.close()
            except OSError:
                pass

    @property
    def frames_sent(self):
        return self._pub.frames_sent

    @property
    def video_frames_sent(self):
        return self._pub.video_frames_sent

    @property
    def bytes_sent(self):
        return self._pub.bytes_sent

    @property
    def frames_dropped(self):
        return self._pub.frames_dropped

    @property
    def warnings(self):
        return self._pub.warnings

    @property
    def av_skew_ms(self):
        """The wire skew of the audio timeline against the video one, in ms."""
        return self._pub.av_skew_ms

    @property
    def bitrate_kbps(self):
        """Stream throughput in kbit/s, averaged over the last ~10 seconds."""
        now = time.monotonic()

        with self._stat_lock:
            samples = self._samples

            if not samples or now - samples[-1][0] >= 0.5:
                samples.append((now, self._pub.bytes_sent))

            while len(samples) > 2 and now - samples[0][0] > 10.0:
                samples.popleft()

            if len(samples) < 2:
                return 0.0

            (t0, b0), (t1, b1) = samples[0], samples[-1]

        dt = t1 - t0

        return (b1 - b0) * 8.0 / dt / 1000.0 if dt > 0 else 0.0

    @property
    def stats(self):
        """One snapshot for dashboards: state, position, throughput, counters."""
        return {
            "position": self.position,
            "paused": self.paused,
            "muted": self.muted,
            "connected": self.connected,
            "stopped": self.stopped,
            "loops": self._loops,
            "bitrate_kbps": self.bitrate_kbps,
            "frames_sent": self.frames_sent,
            "video_frames_sent": self.video_frames_sent,
            "bytes_sent": self.bytes_sent,
            "frames_dropped": self.frames_dropped,
            "window_bytes": self.window_bytes,
            "av_skew_ms": self.av_skew_ms,
            "error": self.error,
        }

    def _relay(self):
        collector = self._collector

        if collector.has_video:
            self._pub.set_expect_video(True)

        for warning in collector.warnings:
            self.log(f"warning: {warning}")

        if collector.jumps:
            self._pub.on_discontinuity()

        # Loop passes continue the same stream timeline: frames carry raw source
        # timestamps plus the pass offset, while the position bookkeeping
        # stays raw (each pass restarts the displayed position).
        offset = self._loop_base
        frames = collector.frames

        if frames:
            self._saw_media = True

        for frame in frames:
            if frame["video"]:
                dts = frame["dts_ms"] + offset

                self._pub.feed_video(
                    dts,
                    frame["pts_ms"] + offset,
                    frame["data"],
                    frame["keyframe"],
                    frame["config"],
                )

                with self._lock:
                    if self._seg_dts is None:
                        self._seg_dts = frame["dts_ms"]

                    self._last_dts = frame["dts_ms"]

                    if frame["keyframe"] and frame["config"]:
                        self._keyframe = (
                            dts,
                            bytes(frame["data"]),
                            bytes(frame["config"]),
                        )

            else:
                pts = frame["pts_ms"] + offset

                with self._lock:
                    if self._seg_dts is None:
                        self._seg_dts = frame["pts_ms"]

                    self._last_dts = max(self._last_dts, frame["pts_ms"])
                    muted = self._muted

                if not muted:
                    self._pub.feed_audio(pts, frame["config"], frame["data"])

        collector.clear()

    def _run(self):
        delay = self._reconnect_delay

        try:
            while not self._stopped:
                try:
                    self._session()

                    return

                except _Stopped:
                    return

                except _Permanent as error:
                    self._error = str(error)
                    self.log(f"error: {error}")

                    return

                except (RuntimeError, ConnectionError, OSError) as error:
                    if self._stopped:
                        return

                    self.log(
                        f"The RTMP connection dropped ({error}); retrying in "
                        f"{delay:.0f} seconds..."
                    )

                    if not self._sleep(delay):
                        return

                    delay = min(delay * 2, self._max_reconnect_delay)

                except Exception as error:  # Never die silently.
                    self._error = f"{type(error).__name__}: {error}"
                    self.log(f"error: {self._error}")

                    return

        finally:
            self._pub.detach()

            if self._client is not None:
                self._client.close()

    def _sleep(self, seconds):
        until = time.monotonic() + seconds

        while time.monotonic() < until:
            if self._stopped:
                return False

            time.sleep(min(0.1, until - time.monotonic()))

        return not self._stopped

    def _fire_connection(self, up):
        # Worker thread only; a user callback must never kill the session.
        callback = self._on_connection
        self._connected = up

        if callback is None:
            return

        try:
            callback(self, up)
        except Exception as error:
            self.log(f"error: the on_connection callback failed: {error}")

    def _fire_media_end(self):
        # Worker thread only; a user callback must never kill the session.
        callback = self._on_media_end

        if callback is None:
            return

        try:
            callback(self)
        except Exception as error:
            self.log(f"error: the on_media_end callback failed: {error}")

    def _session(self):
        url, key = self._url, self._key

        started = time.monotonic()

        client = connect(
            url,
            key,
            verify_tls=self._verify_tls,
            connect_timeout=self._connect_timeout,
            command_timeout=self._command_timeout,
        )

        elapsed_ms = (time.monotonic() - started) * 1000.0

        self._client = client
        self._pub.attach(client)

        self.log(_connected_message(url, client.remote, elapsed_ms))
        self._fire_connection(True)

        # A fresh session starts its own pass: recovery and zero-frame
        # accounting reset with it.
        self._saw_media = False
        self._primed = False
        self._feed_pos = 0

        self._src = Source(self._collector)

        try:
            self._feed_loop()

            if self._stopped:
                raise _Stopped()

            self._src.finish()
            self._relay()

            self._pub.drain(10.0)
            client.wait_closed(1.0)

        except _Stopped:
            raise

        except RuntimeError as error:
            if client.is_closed:
                raise ConnectionError(str(error)) from error

            raise _Permanent(str(error)) from error

        finally:
            self._pub.detach()
            client.close()

            self._client = None
            self._fire_connection(False)

    def _feed_loop(self):
        while not self._stopped:
            with self._lock:
                skip = self._skip
                self._skip = False

            if skip:
                nxt = self._playlist._advance(self._loop == "all")

                if nxt is not None:
                    self._install(nxt)

                continue

            with self._lock:
                playing = self._playing

            if not playing or self._eof:
                if self._eof and not self._hold_eof:
                    self._hold_eof = True

                    self.log("The source ended; holding the last frame")

                if self._eof:
                    nxt = self._playlist._advance(self._loop == "all")

                    if nxt is not None:
                        self._install(nxt)

                        continue

                self._hold_frozen()

                continue

            chunk = self._next_chunk()

            if not chunk:
                if not self._saw_media and self._recover_mp4():
                    continue

                if not self._saw_media:
                    raise _Permanent(ZERO_MEDIA)

                if self._loop is True and self._restart():
                    continue

                self._eof = True

                if self._stopped:
                    continue

                self._fire_media_end()

                nxt = self._playlist._advance(self._loop == "all")

                if nxt is not None:
                    self._install(nxt)

                continue

            if self._primed:
                self._src.feed_at(chunk, self._feed_pos)
                self._feed_pos += len(chunk)

            else:
                self._src.feed(chunk)

            self._relay()

    def _install(self, source):
        """Swaps in a new source and starts its pass; the connection stays
        up and the wire timeline continues past the previous media. Paths
        are opened here and closed when the next media starts."""
        managed = isinstance(source, (str, os.PathLike))

        if managed:
            source = open(source, "rb")

        old, old_managed = self._source, self._managed_source
        self._source = source
        self._managed_source = managed

        self._begin_pass()
        self._src = Source(self._collector)

        if old_managed:
            try:
                old.close()
            except OSError:
                pass

        self.log("Switching to the next media")

    def _begin_pass(self):
        """Starts a pass on the wire timeline: the offset climbs one
        millisecond past the highest timestamp ever sent, so the pass never
        rewinds the stream or collides with the publisher's clamp."""
        with self._lock:
            self._loop_base = self._loop_base + int(self._last_dts) + 1
            self._seg_base = 0.0
            self._seg_dts = None
            self._freeze_dts = None
            self._keyframe = None
            self._eof = False
            self._hold_eof = False

        self._saw_media = False
        self._primed = False
        self._feed_pos = 0
        self._pending = b""

    def _recover_mp4(self):
        """A progressive MP4 whose moov sits at the tail streams blind to EOF:
        read the index from the tail, prime the demuxer with it, and replay
        the media against absolute offsets."""
        found = _tail_moov(self._source)

        if found is None:
            return False

        self._src.prime(found[0])
        self._source.seek(0)

        self._primed = True
        self._feed_pos = 0

        self.log("MP4 with a trailing index: recovered via a tail moov read")

        return True

    def _restart(self):
        try:
            self._source.seek(0)
        except (OSError, ValueError) as error:
            self.log(
                f"warning: the source cannot loop ({error}); holding the last frame"
            )

            return False

        # Verify the restart actually yields data before committing: a handle
        # whose seek is a no-op (or a live source whose reader already ended)
        # would otherwise restart forever and starve the stream.
        chunk = self._next_chunk()

        if not chunk:
            self.log("warning: the source cannot loop; holding the last frame")

            return False

        self._begin_pass()

        with self._lock:
            self._loops += 1

        # The fresh demuxer is not primed, and the new pass must earn its
        # media again: a moov-at-end source re-recovers on its own EOF, and
        # a source that turns empty is caught by the zero-frame guard.
        self._pending = chunk
        self._src = Source(self._collector)

        self.log("Looping: the source restarted")

        return True

    def _hold_frozen(self):
        with self._lock:
            keyframe = self._keyframe
            freeze = self._freeze_dts

        if keyframe is None:
            time.sleep(0.1)

            return

        dts, annexb, config = keyframe

        if freeze is None:
            freeze = dts

        freeze += int(self._freeze_interval * 1000)

        with self._lock:
            self._freeze_dts = freeze

        self._pub.feed_video(freeze, freeze, annexb, True, config)

        time.sleep(self._freeze_interval)

    def _next_chunk(self):
        if self._pending:
            chunk, self._pending = self._pending, b""

            return chunk

        if self._reader is None:
            return self._source.read(CHUNK)

        with self._window_cond:
            while not self._window:
                if self._window_error is not None:
                    raise OSError(f"the source failed: {self._window_error}")

                if self._window_eof or self._stopped:
                    return b""

                self._window_cond.wait(0.5)

            if self._fell_behind:
                self._fell_behind = False

                self.log(
                    f"the pause outlived the rolling window "
                    f"({self._window_cap} bytes); continuing from the "
                    f"buffered past"
                )

            parts = []
            want = CHUNK

            while want > 0 and self._window:
                chunk = self._window[0]
                take = chunk[:want]

                parts.append(take)
                want -= len(take)

                if len(take) == len(chunk):
                    self._window.popleft()
                else:
                    self._window[0] = chunk[len(take) :]

                self._window_bytes -= len(take)

            return b"".join(parts)

    def _fill_window(self):
        try:
            while not self._stopped:
                try:
                    chunk = self._source.read(65536)
                except Exception as error:
                    with self._window_cond:
                        self._window_error = error
                        self._window_cond.notify_all()

                    return

                if not chunk:
                    with self._window_cond:
                        self._window_eof = True
                        self._window_cond.notify_all()

                    return

                with self._window_cond:
                    self._window.append(chunk)
                    self._window_bytes += len(chunk)

                    while (
                        self._window_bytes > self._window_cap and len(self._window) > 1
                    ):
                        self._window_bytes -= len(self._window.popleft())
                        self._fell_behind = True

                    self._window_cond.notify_all()

        finally:
            with self._window_cond:
                self._window_cond.notify_all()


def _seekable(source):
    check = getattr(source, "seekable", None)

    try:
        return bool(check()) if callable(check) else False
    except (OSError, ValueError):
        return False


def publish(source, url, key, *, buffer_size=0, log=None, **options) -> Player:
    """Publish ``source`` to ``url`` with ``key`` and hand back a Player.

    The source can be a path, an open binary file, or a live byte stream
    (any ``.read(n)`` handle that only grows, such as a downloader's output).

    A live source cannot be paused from the outside; it keeps producing.
    By default (``buffer_size=0``) nothing is buffered: pause freezes the
    picture, and resume jumps to the live edge. Give ``buffer_size`` some
    bytes and the source is drained into a rolling window instead, so a
    pause holds its point while the live edge keeps filling it, like a DVR.

    Tuning ``buffer_size``: it is how far behind the live edge you can
    afford to be. The held time is roughly ``buffer_size / bitrate``, e.g.
    48 MB holds about five minutes of a 1.3 Mbps stream or one minute of
    6 Mbps. A pause that outlives the window resumes from the oldest
    buffered data and says so in the log.

    With ``loop=True`` a finite source restarts from the beginning at EOF
    instead of holding its last frame: a movie becomes an endless channel.
    Each pass continues the same stream timeline, so viewers see one
    uninterrupted stream; ``player.stats["loops"]`` counts the passes.

    ``player.stats`` is one dict with everything a dashboard needs
    (position, paused, muted, connected, bitrate_kbps, frames_sent, ...),
    and ``player.bitrate_kbps`` is the stream throughput on its own.

    The extra ``options`` go to the Player: ``playlist`` takes an iterable
    of sources to play after the first one, on the same connection (queue
    more later with ``player.play_next`` and ``player.add``);
    ``on_media_end`` is called as ``on_media_end(player)`` when the source
    reaches its end; ``player.next()`` skips to the next playlist item;
    ``loop`` is True (repeat the current file; rejected when a playlist is
    given), ``"all"`` (cycle the playlist), or False; change it mid-stream
    with ``player.loop = ...``.
    """
    log = log or (lambda message: None)

    if isinstance(source, (str, os.PathLike)):
        player = Player(open(source, "rb"), url, key, log=log, **options)
        player._managed_source = True

        return player

    if _seekable(source):
        return Player(source, url, key, log=log, **options)

    return Player(source, url, key, window_bytes=int(buffer_size), log=log, **options)
