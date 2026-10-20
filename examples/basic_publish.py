"""The smallest publish: one file, one call; it plays to the end and holds
the last frame so the stream stays live until you stop it.

    python basic_publish.py movie.mp4 --key YOUR_KEY
"""

import argparse
import time

import tgrtmp

parser = argparse.ArgumentParser(
    description="Publish a media file to a Telegram livestream."
)
parser.add_argument("source", help="media file: MPEG-TS, MP4 or MKV")

parser.add_argument(
    "--url", default="rtmps://dc4-1.rtmp.t.me/s/", help="RTMP application URL"
)
parser.add_argument("--key", required=True, help="stream key")

args = parser.parse_args()

player = tgrtmp.publish(args.source, args.url, args.key, log=print)

try:
    while not player.stopped:
        time.sleep(1)

except KeyboardInterrupt:
    pass

finally:
    player.stop()

print("totals:", player.stats)
