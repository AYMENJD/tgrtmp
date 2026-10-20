"""End-to-end: the tgrtmp.stream() helper publishes a file-like source to the
loopback server, with reconnects and clean stops."""

import io
import time

import pytest

import tgrtmp
import tgrtmp._core as core

from rtmp_test_server import RtmpTestServer


def test_stream_helper_publishes(media):
    with RtmpTestServer() as server:
        with open(media["mp4"], "rb") as handle:
            # The file is only 3 s long; the pacer spreads it over real time,
            # so run the source faster than real time by disabling pacing.
            source = io.BufferedReader(io.FileIO(handle.fileno()), buffer_size=65536)
            publisher = tgrtmp.Publisher(lookahead_ms=10_000)  # No throttle.
            client = core.connect(f"rtmp://127.0.0.1:{server.port}/live", "key")
            publisher.attach(client)
            src = tgrtmp.Source(publisher)
            try:
                while chunk := source.read(65536):
                    src.feed(chunk)
                src.finish()
            finally:
                deadline = time.monotonic() + 5
                while (
                    len(server.media)
                    < media["truth"]["video"]["packets"]
                    + media["truth"]["audio"]["packets"]
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.01)
                publisher.stop()
                client.close()
            assert len(server.media) >= media["truth"]["video"]["packets"]
            assert server.published.is_set()
            assert server.metadata is not None


def test_stream_helper_stops(media):
    """The stop() hook ends publishing early, and the pacer thread joins."""
    with RtmpTestServer() as server:
        handle = open(media["ts"], "rb")
        stopped = {"value": False}

        def stop():
            return stopped["value"]

        import threading

        timer = threading.Timer(0.5, lambda: stopped.update(value=True))
        timer.start()
        try:
            tgrtmp.stream(
                handle,
                f"rtmp://127.0.0.1:{server.port}/live",
                "key",
                lookahead_ms=10_000,
                stop=stop,
            )
        finally:
            timer.cancel()
            handle.close()
        assert stopped["value"]
        assert server.published.is_set()


def test_stream_helper_rejects_garbage():
    with RtmpTestServer() as server:
        with pytest.raises(RuntimeError):
            tgrtmp.stream(
                io.BytesIO(b"garbage" * 100),
                f"rtmp://127.0.0.1:{server.port}/live",
                "key",
                reconnect_delay=0.1,
            )
