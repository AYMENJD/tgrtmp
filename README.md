# tgrtmp

[![Version](https://img.shields.io/pypi/v/tgrtmp?style=flat&logo=pypi)](https://pypi.org/project/tgrtmp)
[![Downloads](https://img.shields.io/pepy/dt/tgrtmp?style=flat&logo=pypi)](https://pypi.org/project/tgrtmp)
[![Docs](https://img.shields.io/badge/Documentation-229ED9?style=flat)](https://tgrtmp.pages.dev)

<a href="https://cupofton.pages.dev/donate?a=UQCeySURtYxvqF2jNXlsFrXuTEqPjJhGx8uoev6tUbD_HELL&n=AYMEN&t=5&c=You+deserve+a+Cup+of+TON+for+tgrtmp%2521" target="_blank" rel="noopener">
    <img src="https://cupofton.pages.dev/assets/badge-1.svg" alt="Buy me a Cup of TON" style="width: 600px; height: auto;">
</a>

`tgrtmp` is a high-performance RTMP(S) publishing client for **Telegram** livestreams.

### Features

`tgrtmp` offers numerous advantages, including:

- **Easy to Use**: One call to go live; a worker thread does the rest
- **High Performance**: Demuxing, pacing, and sending run in a fast native engine
- **Zero Transcoding**: H.264/AAC frames pass through untouched
- **Player Controls**: Pause, play, mute, and a live position, VLC-style, from any thread
- **Playlists**: Queue files, skip, reorder, and chain episodes while live; it all plays on one connection
- **Endless Channels**: Looping restarts a file on the same timeline: one uninterrupted stream, forever
- **DVR for Live Sources**: Any growing `.read(n)` handle, with an opt-in rolling window (`buffer_size`) that lets pause hold its point

### Compatibility

`tgrtmp` is compatible with the following platforms:

- **Linux** (`x64`, `ARM64`) — Debian 8+, Ubuntu 13.10+, Fedora 19+, RHEL 7+
- **Windows** (`x64`) — Windows 7+
- **macOS** (`M-series`) — macOS 11+

### Requirements

- Python 3.10+
- A [Telegram livestream](https://telegram.org/blog/downloads-attachments-streaming#live-streaming-with-other-apps) RTMP URL and stream key
- Media in H.264 + AAC (MPEG-TS, MP4, or MKV); other codecs are rejected, not transcoded

### Installation

You can install tgrtmp with prebuilt binaries using pip:

```bash
pip install --upgrade tgrtmp
```

### Examples

Publish a file and hold the last frame when it ends:

```python
import time
import tgrtmp

player = tgrtmp.publish(
    "movie.mp4",
    "rtmps://dc4-1.rtmp.t.me/s/",
    "STREAM_KEY",
    log=print,
)

# The file plays in real time; at the end the last frame is held
# so the stream stays live until you stop it.
while not player.stopped:
    time.sleep(1)
```

Control it while it runs; every call is thread-safe:

```python
player.pause()       # viewers see a still frame; the connection stays alive
player.play()
player.mute()
player.position      # seconds into the source
player.stats         # one dict: bitrate, frames, loops, state
player.stop()
```

Turn it into a 24/7 channel:

```python
player = tgrtmp.publish("movie.mp4", url, key, loop=True)
```

Queue a playlist and edit it while live:

```python
player = tgrtmp.publish("intro.mp4", url, key, playlist=["a.mp4", "b.mp4"])

player.next()            # skip the intro
player.add("c.mp4")      # append to the queue
```

For more examples, check the [examples](https://github.com/AYMENJD/tgrtmp/tree/main/examples) folder.

### Documentation

The full documentation (quick start, guides, and API reference) lives at [tgrtmp.pages.dev](https://tgrtmp.pages.dev).

# Thanks to

- You for viewing or using this project.

- [MadelineProto](https://github.com/danog/MadelineProto) for documenting the RTMP behavior of Telegram's servers

# License

MIT [License](https://github.com/AYMENJD/tgrtmp/blob/main/LICENSE)
