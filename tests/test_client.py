"""Integration tests: the real Client against the loopback RTMP server over
real TCP sockets; the full handshake, the publish command flow, media
writes, control message handling, and teardown."""

import time

import pytest

import tgrtmp._core as core
from _pyref import aac_frame, avc_frame

from rtmp_test_server import RtmpTestServer

URL = "rtmp://127.0.0.1:{}/live"


def test_publish_flow_and_media():
    with RtmpTestServer() as server:
        client = core.connect(URL.format(server.port), "stream-key")
        assert not client.is_closed

        client.send_metadata(bytes(core.flv_metadata(320, 240, True, True)))
        client.send_audio(0, bytes(core.aac_sequence_header(b"\x12\x10")))
        client.send_audio(21, bytes(core.aac_frame(b"AUDIO")))
        client.send_video(0, bytes(core.avc_sequence_header(b"AVCC")))
        client.send_video(
            33, bytes(core.avc_frame(b"VIDEO", keyframe=True, composition_time=5))
        )

        deadline = time.monotonic() + 5
        while len(server.media) < 4 and time.monotonic() < deadline:
            time.sleep(0.01)

        assert server.published.is_set()
        assert [name for name, _ in server.commands] == [
            "connect",
            "releaseStream",
            "FCPublish",
            "createStream",
            "publish",
        ]
        # connect carried the parsed URL pieces.
        connect_args = dict(server.commands[0][1][0])
        assert connect_args["app"] == "live"
        assert connect_args["tcUrl"] == URL.format(server.port)
        assert server.metadata == {
            "duration": 0.0,
            "width": 320.0,
            "height": 240.0,
            "videocodecid": 7.0,
            "audiocodecid": 10.0,
            "encoder": "tgrtmp",
        }
        # Media arrived on the created stream, bodies intact.
        types = [m[0] for m in server.media]
        assert types == [8, 8, 9, 9]
        assert all(m[2] == 1 for m in server.media)
        audio, video = server.media[1], server.media[3]
        assert audio[1] == 21 and audio[3] == aac_frame(b"AUDIO")
        assert video[1] == 33 and video[3] == avc_frame(b"VIDEO", True, 5)

        client.close()
        client.close()  # Idempotent.
        assert client.is_closed
        deadline = time.monotonic() + 5
        while not server.teardown and time.monotonic() < deadline:
            time.sleep(0.01)
        assert "FCUnpublish" in server.teardown and "deleteStream" in server.teardown
        assert client.wait_closed(0.1)


def test_connect_refusal():
    with RtmpTestServer(refuse_connect=True) as server:
        with pytest.raises(RuntimeError, match="refused"):
            core.connect(URL.format(server.port), "stream-key")


def test_publish_refusal():
    with RtmpTestServer(refuse_publish=True) as server:
        with pytest.raises(RuntimeError, match="refused the stream"):
            core.connect(URL.format(server.port), "stream-key")


def test_server_closes_during_handshake():
    # A TCP server that accepts and immediately closes.
    import socket
    import threading

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    def serve():
        sock, _ = listener.accept()
        sock.close()

    threading.Thread(target=serve, daemon=True).start()
    with pytest.raises(RuntimeError, match="handshake"):
        core.connect(f"rtmp://127.0.0.1:{port}/live", "key", connect_timeout=5.0)


def test_ping_answered_and_ack_window():
    with RtmpTestServer(ping=True, chunk_size=1234) as server:
        client = core.connect(URL.format(server.port), "stream-key")
        # The server announced a 1234-byte ack window and a ping: after
        # receiving enough media, it must see our acknowledgement (type 3 on
        # csid 2 rides the control stream and is answered by the reader).
        client.send_video(0, bytes(core.avc_sequence_header(b"A" * 5000)))
        client.send_video(33, bytes(core.avc_frame(b"B" * 5000, False, 0)))
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and len(server.media) < 3:
            time.sleep(0.01)
        # The first item is the ping response (user control, type 4).
        assert server.media[0][0] == 4
        assert server.media[0][3][:2] == b"\x00\x07"
        media = [m for m in server.media if m[0] in (8, 9, 18)]
        assert len(media) == 2
        client.close()


def test_server_chunk_size_honored():
    # The server announces its chunk size before the connect; a reply larger
    # than one chunk must still parse on the client side.
    with RtmpTestServer(chunk_size=64) as server:
        client = core.connect(
            URL.format(server.port), "stream-key"
        )  # _result spans chunks.
        assert not client.is_closed
        client.close()


def test_error_string_on_drop():
    with RtmpTestServer(close_after_media=1) as server:
        client = core.connect(URL.format(server.port), "stream-key")
        client.send_audio(0, bytes(core.aac_frame(b"A" * 2000)))
        assert client.wait_closed(5.0)
        assert client.is_closed
        assert "closed" in client.error.lower()
