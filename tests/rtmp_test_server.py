"""A minimal in-process RTMP server for loopback tests: it performs the
simple handshake, parses the client's chunk stream (with the very ChunkParser
of the core, dogfooding it), answers the publish flow like Telegram's server
does, and records every media message for assertions.

Run it in a thread; it serves exactly one connection per instance.
"""

import socket
import threading

import tgrtmp._core as core

from _pyref import amf_decode, amf_encode


def amf_decode_all(data):
    """Decode every AMF0 value of a payload."""
    values = []
    offset = 0
    while offset < len(data):
        value, offset = amf_decode(data, offset)
        values.append(value)
    return values


def command(name, txn, *args):
    """An RTMP command message: name, transaction id, then the arguments;
    sequential top-level AMF0 values, as the wire carries them."""
    return amf_encode(name) + amf_encode(txn) + b"".join(amf_encode(a) for a in args)


class RtmpTestServer:
    def __init__(
        self,
        *,
        refuse_connect=False,
        refuse_publish=False,
        ping=False,
        chunk_size=None,
        close_after_media=None,
    ):
        self.refuse_connect = refuse_connect
        self.refuse_publish = refuse_publish
        self.ping = ping
        self.chunk_size = chunk_size  # Announce a Set Chunk Size when set.
        self.close_after_media = close_after_media
        self.commands = []  # Every command the client sent.
        self.media = []  # (type_id, timestamp, stream_id, payload).
        self.teardown = []  # FCUnpublish/deleteStream commands.
        self.closed_by_client = threading.Event()
        self.published = threading.Event()
        self.metadata = None
        self._sock = None
        self._listener = None
        self._parser = None
        self._thread = None
        self.port = None

    def __enter__(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        self.port = listener.getsockname()[1]
        self._listener = listener
        self._thread = threading.Thread(
            target=self._serve, args=(listener,), daemon=True
        )
        self._thread.start()
        return self

    def __exit__(self, *exc):
        # A serve thread still waiting in accept() only wakes when a
        # connection arrives: dial our own port so a server no client ever
        # reached does not stall the teardown on the join timeout.
        if self.port is not None and self._sock is None:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
            except OSError:
                pass

        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass

        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        self._thread.join(timeout=5)
        if self._parser is not None:
            self._parser.set_handler(None)  # Break the handler cycle.

    def _send(self, data):
        view = memoryview(data)
        while view:
            sent = self._sock.send(view)
            view = view[sent:]

    def _serve(self, listener):
        # Serves connections one after another: a player that reconnects
        # must find the server still there.
        while True:
            try:
                sock, _ = listener.accept()
            except OSError:
                return

            self._sock = sock
            try:
                self._session()
            except (OSError, ValueError):
                pass
            finally:
                self.closed_by_client.set()
                try:
                    sock.close()
                except OSError:
                    pass

            self._sock = None
            self._parser = None

    def _session(self):
        # Handshake: C0+C1 in, S0+S1+S2 out, C2 in.
        c0c1 = self._recv_exact(1537)
        if c0c1[0] != 3:
            raise ValueError("bad version")
        self._send(bytes([3]) + c0c1[1:1537] + c0c1[1:1537])
        self._recv_exact(1536)  # C2.

        parser = core.ChunkParser()
        self._parser = parser
        parser.set_chunk_size(128)
        out_chunk = 128

        def send_message(csid, type_id, stream_id, timestamp, payload):
            nonlocal out_chunk
            self._send(
                core.chunk_write(
                    csid, type_id, stream_id, timestamp, payload, out_chunk
                )
            )

        if self.chunk_size is not None:
            out_chunk = self.chunk_size
            send_message(2, 1, 0, 0, self.chunk_size.to_bytes(4, "big"))
        if self.ping:
            send_message(2, 4, 0, 0, b"\x00\x06" + b"\x00\x00\x00\x01")

        transaction = 0

        def on_message(type_id, timestamp, stream_id, payload):
            nonlocal transaction
            if type_id == 1:  # Set Chunk Size: the client's chunks grow.
                parser.set_chunk_size(
                    int.from_bytes(bytes(payload), "big") & 0x7FFFFFFF
                )
                return
            if type_id == 20:  # AMF0 command.
                values = amf_decode_all(bytes(payload))
                name = values[0]
                txn = values[1]
                self.commands.append((name, values[2:]))
                if name == "connect":
                    if self.refuse_connect:
                        send_message(
                            3,
                            20,
                            0,
                            0,
                            command(
                                "_error",
                                txn,
                                None,
                                {
                                    "level": "error",
                                    "code": "NetConnection.Connect.Rejected",
                                    "description": "nope",
                                },
                            ),
                        )
                        return
                    send_message(
                        3,
                        20,
                        0,
                        0,
                        command(
                            "_result",
                            txn,
                            {"fmsVer": "FMS/3,5,7,7009", "capabilities": 31.0},
                            {
                                "level": "status",
                                "code": "NetConnection.Connect.Success",
                                "description": "Connection succeeded.",
                                "objectEncoding": 0.0,
                            },
                        ),
                    )
                elif name == "releaseStream":
                    send_message(3, 20, 0, 0, command("_result", txn, None, None))
                elif name == "FCPublish":
                    send_message(3, 20, 0, 0, command("_result", txn, None, None))
                elif name == "createStream":
                    send_message(3, 20, 0, 0, command("_result", txn, None, 1.0))
                elif name == "publish":
                    if self.refuse_publish:
                        send_message(
                            3,
                            20,
                            stream_id,
                            0,
                            command(
                                "onStatus",
                                0.0,
                                None,
                                {
                                    "level": "error",
                                    "code": "NetStream.Publish.BadName",
                                    "description": "already in use",
                                },
                            ),
                        )
                    else:
                        send_message(
                            3,
                            20,
                            stream_id,
                            0,
                            command(
                                "onStatus",
                                0.0,
                                None,
                                {
                                    "level": "status",
                                    "code": "NetStream.Publish.Start",
                                    "description": " Publishing",
                                },
                            ),
                        )
                        self.published.set()
                elif name in ("FCUnpublish", "deleteStream"):
                    self.teardown.append(name)
            elif type_id == 18:  # AMF0 data: the onMetaData.
                values = amf_decode_all(bytes(payload))
                if values and values[0] == "@setDataFrame":
                    self.metadata = values[2]
            else:
                self.media.append((type_id, timestamp, stream_id, bytes(payload)))
                if (
                    self.close_after_media is not None
                    and len(self.media) >= self.close_after_media
                ):
                    self._sock.close()

        parser.set_handler(on_message)
        while True:
            data = self._sock.recv(65536)
            if not data:
                return
            parser.feed(data)

    def _recv_exact(self, count):
        data = b""
        while len(data) < count:
            chunk = self._sock.recv(count - len(data))
            if not chunk:
                raise OSError("closed")
            data += chunk
        return data
