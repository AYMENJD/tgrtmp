# Errors

Two kinds of failure, two behaviors.

## Ask before streaming

[`needs_transcode`][tgrtmp.needs_transcode] tells you whether a file publishes as is or must be re-encoded first (H.264 video, AAC audio):

```python
import tgrtmp

if tgrtmp.needs_transcode("clip.mkv"):
    print("transcode it: ffmpeg -i clip.mkv -c:v libx264 -c:a aac clip.mp4")
```

The check runs the same demuxers that would stream the file, so its answer is the publisher's answer: an unsupported container, non-H.264 video, non-AAC audio, or no video track all read True.

## Drops: retried

Network failures, server resets, TLS timeouts: the session dies, the player backs off (1 s doubling to 30 s) and reconnects. Nothing raises; `log` and [`on_connection`](reconnecting.md) narrate.

## Broken sources: fatal

A bad container, a truncated file, a codec Telegram does not accept; retrying cannot help. The player stops, and `player.error` carries the reason:

```python
player = tgrtmp.publish("broken.mp4", url, key)

while not player.stopped:
    time.sleep(0.5)

if player.error:
    print("failed:", player.error)
```

In the blocking API the same condition raises [`SourceError`][tgrtmp.SourceError].

## Warnings: not errors

`player.warnings` collects things the server will tolerate but you should fix: non-16:9 sources that Telegram will stretch, odd audio sample rates. They never stop the session.

## Misuse: raised at once

Calls that cannot work raise immediately and never touch the stream: `loop=True` on a player with a playlist, queueing into a player that repeats one file, `play_next()` after the player has ended. `next()` is gentler: with nothing queued it just returns False. Fix the call; the session keeps running.

## Client vs player

`Client.error` (low level) explains why one connection closed. `Player.error` is the session-level verdict, set only when the player gave up for good.
