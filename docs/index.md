---
hide:
  - navigation
  - toc
---

<div class="hero" markdown>

# tgrtmp

<p class="lede" markdown>Publish media files and live sources to [Telegram livestreams](https://telegram.org/blog/downloads-attachments-streaming#live-streaming-with-other-apps); built for high performance, with the remote control in Python.</p>

```bash
pip install tgrtmp
```

[Quick start](getting-started/quickstart.md){ .md-button .md-button--primary }
[Guides](guides/player.md){ .md-button }

</div>

<div class="home-first" markdown>

## Your first stream

You need an RTMP URL and stream key for your Telegram livestream. Media must be H.264 + AAC; it is published untouched, never transcoded.

```python
import time
import tgrtmp

player = tgrtmp.publish("movie.mp4", "rtmps://dc4-1.rtmp.t.me/s/", "KEY", log=print)

while not player.stopped:
    time.sleep(1)
```

The file plays in real time. At the end the last frame is held so the stream stays live until you `stop()`.

From here: [player controls](guides/player.md), [24/7 channels](guides/looping.md), [live sources](getting-started/live-sources.md).

</div>

<div class="home-map" markdown>

<div markdown>

## Start here

Install, publish a file, go live from a live source.

[Install](getting-started/installation.md)
[Quick start](getting-started/quickstart.md)
[Live sources](getting-started/live-sources.md)

</div>

<div markdown>

## Run the show

Pause, mute, loop, reconnect, watch the numbers.

[Player](guides/player.md)
[The live window](guides/live-window.md)
[24/7 channels](guides/looping.md)
[Monitoring](guides/monitoring.md)

</div>

<div markdown>

## Look something up

The blocking API, the low level, errors.

[The blocking API](guides/stream.md)
[Low level](guides/low-level.md)
[Errors](guides/errors.md)
[API reference](reference/index.md)

</div>

</div>
