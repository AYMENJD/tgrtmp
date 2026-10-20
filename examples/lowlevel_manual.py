"""Under the Player, exposed: the wire-level API driven by hand.

connect() opens the RTMP(S) session, the Publisher paces frames to real time
(feed() blocks on a small lookahead window; feeding as fast as the file
reads is safe), and the Source demuxes the container. This is everything
stream() does, written out, for when you want the loop yourself.

    python lowlevel_manual.py movie.mp4 --key YOUR_KEY
"""

import argparse

import tgrtmp
from tgrtmp._core import Publisher, Source, connect

parser = argparse.ArgumentParser(description="Publish a file over the low-level API.")
parser.add_argument("source", help="media file: MPEG-TS, MP4 or MKV")

parser.add_argument(
    "--url", default="rtmps://dc4-1.rtmp.t.me/s/", help="RTMP application URL"
)
parser.add_argument("--key", required=True, help="stream key")

args = parser.parse_args()

publisher = Publisher(lookahead_ms=1000)
publisher.set_logger(lambda message: print("warning:", message))

client = connect(args.url, args.key)

publisher.attach(client)

source = Source(publisher)

try:
    with open(args.source, "rb") as media:
        while chunk := media.read(65536):
            source.feed(chunk)

    source.finish()

except RuntimeError as error:
    # A broken container is permanent; a closed client is a network drop.
    if client.is_closed:
        raise ConnectionError(str(error)) from error

    raise tgrtmp.SourceError(str(error)) from error

finally:
    publisher.detach()
    client.close()

# Let the pacer flush everything it still holds, then read the totals.
publisher.drain(60.0)

print(f"sent {publisher.frames_sent} frames, {publisher.bytes_sent} bytes")
print(f"dropped {publisher.frames_dropped}, warnings: {publisher.warnings}")

publisher.stop()
