import os
import time
from urllib.parse import urlsplit

from ._core import CollectorSink, connect
from ._core import Publisher as _Publisher
from ._core import Source as _Source

# A source that ends without producing one frame is broken, and no control
# path should spin or sit silently on it.
ZERO_MEDIA = (
    "The source produced no media. If this is an MP4, remux it with its "
    "index in front first: ffmpeg -i input -c copy -movflags +faststart output.mp4"
)

# The largest moov box a recovery is willing to read.
_MOOV_CAP = 256 * 1024 * 1024


def _tail_moov(handle):
    """Walk the top-level MP4 box headers with seeks; return (moov_bytes,
    moov_offset) when the index sits at the tail, else None. A non-MP4 or a
    non-seekable handle returns None."""
    try:
        handle.seek(0)

        if handle.read(16)[4:8] != b"ftyp":
            return None

        handle.seek(0, 2)
        size = handle.tell()

        pos = 0

        while pos + 8 <= size:
            handle.seek(pos)
            header = handle.read(16)

            if len(header) < 8:
                return None

            box_size = int.from_bytes(header[0:4], "big")
            box_type = header[4:8]
            header_len = 8

            if box_size == 1:
                if len(header) < 16:
                    return None

                box_size = int.from_bytes(header[8:16], "big")
                header_len = 16

            elif box_size == 0:
                box_size = size - pos

            if box_size < header_len or pos + box_size > size:
                return None

            if box_type == b"moov":
                if box_size > _MOOV_CAP:
                    return None

                handle.seek(pos)
                moov = handle.read(box_size)

                if len(moov) != box_size:
                    return None

                return moov, pos

            pos += box_size

    except (OSError, ValueError):
        return None

    return None


# How much of a source the probe reads before concluding: container headers
# and the first frames live in the first megabytes of any supported file.
_PROBE_BYTES = 8 * 1024 * 1024

# Media between the video track appearing and the audio verdict: interleaved
# audio follows the first keyframe within kilobytes.
_PROBE_AUDIO_BYTES = 512 * 1024


def needs_transcode(source) -> bool:
    """Whether ``source`` must be transcoded before it can be published.

    Accepts a path or an open seekable binary file: True means the container is
    unsupported, the video is not H.264, the audio is not AAC, or there is
    no video track; False means it can be published as is. It samples the
    head of the source (a few megabytes), so a pathological file that hides
    its headers deeper than that reads as needing a transcode.
    """
    if isinstance(source, (str, os.PathLike)):
        handle, managed = open(source, "rb"), True
    else:
        handle, managed = source, False

    try:
        handle.seek(0)
    except (OSError, ValueError, AttributeError):
        if managed:
            handle.close()

        raise ValueError(
            "needs_transcode reads a file; pass a path or a seekable file"
        ) from None

    try:
        head = handle.read(8)
        collector = CollectorSink()
        src = _Source(collector)

        if len(head) < 8:
            return True  # Not enough bytes to be any supported container.

        tail = None

        if head[4:8] == b"ftyp":
            tail = _tail_moov(handle)  # Seeks around; None for a front moov.

        handle.seek(0)
        chunks = iter(_read_chunks(handle))

        # The first feed sniffs the container; priming needs that verdict,
        # so it only comes after (the player recovers the same way).
        try:
            src.feed(next(chunks, b""))
        except RuntimeError:
            return True

        offset = 0 if tail is not None else None

        if offset is not None:
            try:
                src.prime(tail[0])
            except RuntimeError:
                return True  # A moov the demuxers refuse is a transcode case.

            handle.seek(0)

        fed = 0

        for chunk in chunks:
            try:
                if offset is None:
                    src.feed(chunk)

                else:
                    src.feed_at(chunk, offset)
                    offset += len(chunk)

            except RuntimeError:
                return True  # The demuxers refuse what publishing would refuse.

            fed += len(chunk)

            if collector.warnings:
                return True  # Codec complaints: the same ones publishing logs.

            if collector.has_video and fed >= _PROBE_AUDIO_BYTES:
                break

            # The frames were only needed for the verdict: dropping them
            # keeps the probe's memory at one chunk instead of the sample.
            collector.clear()

            if fed >= _PROBE_BYTES:
                break

        return not collector.has_video
    finally:
        if managed:
            handle.close()
        else:
            try:
                handle.seek(0)
            except (OSError, ValueError, AttributeError):
                pass


def _connected_message(url, remote, elapsed_ms):
    """The one-line connection notice: the host, its numeric address when
    the two differ, and how long reaching it took."""
    host = urlsplit(url).hostname or url
    address = f" ({remote})" if remote and remote != host else ""

    return f"Connected to {host}{address} in {elapsed_ms:.0f}ms"


class SourceError(RuntimeError):
    """The published source is broken (bad container, unsupported codec):
    reconnecting cannot help, so :func:`stream` propagates it."""


class Publisher(_Publisher):
    """A :class:`Publisher` that keeps its attached client alive."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._client = None

    def attach(self, client):
        self._client = client

        super().attach(client)

    def detach(self):
        self._client = None

        super().detach()

    def stop(self):
        self._client = None

        super().stop()


def _read_chunks(source, size=65536):
    # A file-like object, an iterable of bytes, or a zero-argument callable
    # returning bytes until it returns b"" or None.
    read = getattr(source, "read", None)

    if read is not None:
        while True:
            chunk = read(size)

            if not chunk:
                return

            yield chunk

        return

    try:
        iterator = iter(source)
    except TypeError:
        while True:
            chunk = source()

            if not chunk:
                return

            yield chunk

        return

    yield from iterator


def stream(
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
    stop=None,
    log=None,
):
    """Publish a live source to an RTMP(S) livestream, reconnecting on drops.

    :param source: A file-like object (a path, an open file, or a live byte
        stream handle), an iterable of byte chunks, or a callable returning
        chunks until ``b""``.
    :param url: The RTMP application, e.g. ``rtmps://dc4-1.rtmp.t.me/s/``.
    :param stop: An optional zero-argument callable; when it returns True the
        publishing stops.
    :param log: An optional ``callable(message: str)`` for connection notices.
    :return: The :class:`Publisher`, for its statistics.

    The container of the source is auto-detected (MPEG-TS, MP4
    faststart/fragmented, MKV/WebM); its H.264 video and AAC audio are
    published untouched, paced to real time. Sources in other codecs are
    rejected: Telegram's RTMP server accepts H.264 and AAC only.
    """
    publisher = Publisher(lookahead_ms=lookahead_ms)

    # A path is opened here and closed when the publish ends.
    opened = None

    if isinstance(source, (str, os.PathLike)):
        opened = open(source, "rb")
        source = opened

    if log is not None:
        # Warnings about what the server will not accept are surfaced through
        # the same log: nothing fails silently.
        publisher.set_logger(lambda message: log(f"warning: {message}"))

    delay = reconnect_delay
    log = log or (lambda message: None)

    try:
        while stop is None or not stop():
            src = None
            client = None

            try:
                started = time.monotonic()

                client = connect(
                    url,
                    key,
                    verify_tls=verify_tls,
                    connect_timeout=connect_timeout,
                    command_timeout=command_timeout,
                )

                elapsed_ms = (time.monotonic() - started) * 1000.0

                publisher.attach(client)

                log(_connected_message(url, client.remote, elapsed_ms))
                delay = reconnect_delay

                src = _Source(publisher)

                # A broken source is permanent: its errors propagate instead
                # of triggering a reconnect.
                try:
                    for chunk in _read_chunks(source):
                        if stop is not None and stop():
                            return publisher

                        src.feed(chunk)

                    src.finish()
                except RuntimeError as error:
                    if client.is_closed:
                        raise ConnectionError(str(error)) from error

                    raise SourceError(str(error)) from error

                # EOF without a single frame is a broken source (an MP4 whose
                # moov index is not faststart reads exactly like this): say so
                # instead of returning a silent zero-frame publish.
                if publisher.frames_sent == 0:
                    # A progressive MP4 whose moov sits at the tail streams
                    # blind to EOF: read the index, prime the demuxer, and
                    # replay the media against absolute offsets.
                    found = _tail_moov(source)

                    if found is not None:
                        try:
                            src.prime(found[0])
                            source.seek(0)

                            pos = 0

                            for chunk in _read_chunks(source):
                                if stop is not None and stop():
                                    return publisher

                                src.feed_at(chunk, pos)
                                pos += len(chunk)

                            src.finish()
                        except RuntimeError as error:
                            if client.is_closed:
                                raise ConnectionError(str(error)) from error

                            raise SourceError(str(error)) from error

                if publisher.frames_sent == 0:
                    raise SourceError(ZERO_MEDIA)

                # A finite source: let the pacer flush everything it holds.
                publisher.drain(10.0)
                client.wait_closed(1.0)

                if stop is None or not stop():
                    log("The source ended")

                return publisher

            except SourceError:
                raise

            except (RuntimeError, ConnectionError, OSError) as error:
                if stop is not None and stop():
                    return publisher

                log(
                    f"The RTMP connection dropped ({error}); retrying in "
                    f"{delay:.0f} seconds..."
                )

                if stop is None:
                    time.sleep(delay)
                else:
                    for _ in range(int(delay * 10)):
                        if stop():
                            return publisher

                        time.sleep(0.1)

                delay = min(delay * 2, max_reconnect_delay)

            finally:
                publisher.detach()

                if client is not None:
                    client.close()

    finally:
        publisher.stop()

        if opened is not None:
            opened.close()

    return publisher
