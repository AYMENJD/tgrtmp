"""Relay a live byte stream (any .read(n) handle that only grows) to Telegram.

A live source cannot be paused from the outside (it keeps producing), so
this example opts into a rolling window (--buffer-mb): while you are paused
(or lagging), the window holds your point while the live edge keeps filling
it. The window holds roughly buffer_size / bitrate seconds; a pause that
outlives it resumes from the oldest buffered data. The library itself
defaults to no buffering: pause freezes the picture and resume jumps to the
live edge.

    python live_relay.py http://example.org/live.ts --key YOUR_KEY
"""

import argparse
import time
import urllib.request

import tgrtmp


def on_connection(player, connected):
    if connected:
        print("\n=== live ===")
    else:
        print(f"\n=== dropped: {player.error} ===")


parser = argparse.ArgumentParser(
    description="Relay a live HTTP stream to a Telegram livestream."
)
parser.add_argument("source", help="URL of a growing media stream (MPEG-TS, ...)")

parser.add_argument(
    "--url", default="rtmps://dc4-1.rtmp.t.me/s/", help="RTMP application URL"
)
parser.add_argument("--key", required=True, help="stream key")
parser.add_argument(
    "--buffer-mb",
    type=int,
    default=48,
    help="rolling window size; hold time is size / bitrate",
)

args = parser.parse_args()

handle = urllib.request.urlopen(args.source)

player = tgrtmp.publish(
    handle,
    args.url,
    args.key,
    buffer_size=args.buffer_mb * 1_000_000,
    on_connection=on_connection,
    log=print,
)

try:
    while not player.stopped:
        s = player.stats
        buffered = s["window_bytes"]

        hold = "n/a" if buffered is None else f"{buffered / 1_000_000:.1f} MB"

        print(f"\r{s['position']:7.1f}s  buffered {hold}", end="", flush=True)

        time.sleep(1)

except KeyboardInterrupt:
    pass

finally:
    player.stop()

print("\nstopped")
