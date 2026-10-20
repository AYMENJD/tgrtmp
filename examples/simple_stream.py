"""The scripting API: one blocking call, no threads or objects to manage.
It publishes the whole source, reconnecting on drops, and returns the
Publisher with its totals. Ctrl+C stops early.

    python simple_stream.py movie.mp4 --key YOUR_KEY
    python simple_stream.py movie.mp4 --key YOUR_KEY --for 30
"""

import argparse
import time

import tgrtmp

parser = argparse.ArgumentParser(
    description="Publish a media file with one blocking call."
)
parser.add_argument("source", help="media file: MPEG-TS, MP4 or MKV")

parser.add_argument(
    "--url", default="rtmps://dc4-1.rtmp.t.me/s/", help="RTMP application URL"
)
parser.add_argument("--key", required=True, help="stream key")
parser.add_argument(
    "--for", dest="duration", type=float, help="stop after this many seconds"
)

args = parser.parse_args()

deadline = None if args.duration is None else time.monotonic() + args.duration


def past_deadline():
    return deadline is not None and time.monotonic() >= deadline


publisher = tgrtmp.stream(
    args.source, args.url, args.key, stop=past_deadline, log=print
)

print(f"\nsent {publisher.frames_sent} frames, {publisher.bytes_sent} bytes")
