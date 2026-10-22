"""Turn a media file into a 24/7 channel: with loop=True the file restarts at
its end and every pass continues the same stream timeline, so viewers see one
uninterrupted stream. A dashboard shows where it is.

    python endless_channel.py movie.mp4 --key YOUR_KEY
"""

import argparse
import time

import tgrtmp


def on_connection(player, connected):
    if connected:
        print("\n=== live ===")
    else:
        print(f"\n=== dropped: {player.error} ===")


parser = argparse.ArgumentParser(
    description="Loop a media file as an endless livestream."
)
parser.add_argument("source", help="media file: MPEG-TS, MP4 or MKV")

parser.add_argument(
    "--url", default="rtmps://dc4-1.rtmp.t.me/s/", help="RTMP application URL"
)
parser.add_argument("--key", required=True, help="stream key")

args = parser.parse_args()

player = tgrtmp.publish(
    args.source,
    args.url,
    args.key,
    loop=True,
    on_connection=on_connection,
    log=print,
)

try:
    while not player.stopped:
        s = player.stats

        print(
            f"\r{s['position']:7.1f}s  loop {s['loops']}  "
            f"{s['bitrate_kbps']:8.1f} kbit/s  {s['frames_sent']} frames",
            end="",
            flush=True,
        )

        time.sleep(1)

except KeyboardInterrupt:
    pass

finally:
    player.stop()

print("\nstopped")
