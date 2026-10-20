# Live sources

Anything with a `.read(n)` that only grows: a downloader's output, a pipe, an HTTP stream.

```python
import urllib.request
import tgrtmp

handle = urllib.request.urlopen("http://example.org/live.ts")

player = tgrtmp.publish(handle, "rtmps://dc4-1.rtmp.t.me/s/", "KEY")
```

Live sources cannot be paused from the outside; they keep producing. tgrtmp offers two ways to handle that, picked by `buffer_size`.

## Default: no buffering

`buffer_size=0` (the default) drains nothing in advance: pause freezes the picture on the connection, and resume jumps straight to the live edge. Zero overhead: no reader thread, no memory.

## Optional: the DVR window

Give `buffer_size` some bytes and the source is drained into a rolling window, so a pause holds its point while the live edge keeps filling it. Resume where you paused, like a DVR, as long as the pause fits the window. See [the live window](../guides/live-window.md).

```python
player = tgrtmp.publish(handle, url, key, buffer_size=48_000_000)
```

## Which sources behave how

| Source | Position | Pause (default) | Pause (with window) | EOF |
| --- | --- | --- | --- | --- |
| Path or open file | exact | freezes the frame | same | holds last frame |
| Live `.read(n)` handle | your point | freezes, resumes at the live edge | holds its point | holds last frame |

A path is opened and closed for you; an open file or handle is yours to close.
