# Quick start

Publish a media file to a Telegram livestream and hold the last frame when it ends.

You need an RTMP URL and stream key for your Telegram livestream, and a file with H.264 video and AAC audio (MPEG-TS, MP4, or MKV).

## Run this

```python
import time
import tgrtmp

player = tgrtmp.publish(
    "movie.mp4",
    "rtmps://dc4-1.rtmp.t.me/s/",
    "STREAM_KEY",
    log=print,
)

while not player.stopped:
    time.sleep(1)
```

## What you just created

`publish()` returns a [`Player`][tgrtmp.Player] and starts a worker thread; the file plays in real time from the moment the call returns.

`log=print` narrates the session: connects, drops, retries, the end-of-stream hold. Drop it for quiet, or point it at your logger.

At EOF the player re-sends the last keyframe once per second, so the stream never looks dead. `player.stop()` ends it.

## Control it

```python
player.pause()          # viewers see a still frame; the connection stays alive
player.play()
player.mute()           # audio frames stop; video keeps flowing
player.position         # seconds into the source (freezes while paused)
player.stop()
```

Everything is safe to call from any thread. More in [Player](../guides/player.md).

## Never transcode

tgrtmp remuxes: the H.264 and AAC bytes in your file are the bytes that reach Telegram. Other codecs are rejected, not converted; feed it what Telegram accepts.

MP4s whose `moov` index sits at the end (not faststart) are handled too: the player reads the index from the tail and streams the media against it. Live (non-seekable) handles are the exception; remux those with `ffmpeg -i input -c copy -movflags +faststart output.mp4`.
