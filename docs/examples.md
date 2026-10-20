# Examples

Runnable scripts live in the [examples](https://github.com/AYMENJD/tgrtmp/tree/main/examples) folder. Each one runs against a real stream.

## Minimal publish

```python
import time
import tgrtmp

player = tgrtmp.publish("movie.mp4", url, key, log=print)

while not player.stopped:
    time.sleep(1)

player.stop()
```

More in [`examples/basic_publish.py`](https://github.com/AYMENJD/tgrtmp/blob/main/examples/basic_publish.py).

## 24/7 channel with a dashboard

`loop=True` restarts the file at EOF on the same timeline; the dashboard reads `stats`.

```python
player = tgrtmp.publish("movie.mp4", url, key, loop=True, log=print)

while not player.stopped:
    s = player.stats
    print(f"{s['position']:6.1f}s  loop {s['loops']}  {s['bitrate_kbps']:.0f} kbit/s")
    time.sleep(1)
```

More in [`examples/endless_channel.py`](https://github.com/AYMENJD/tgrtmp/blob/main/examples/endless_channel.py).

## Relay a live source

Any growing `.read(n)` handle, with the DVR window sized in megabytes:

```python
handle = urllib.request.urlopen("http://example.org/live.ts")

player = tgrtmp.publish(handle, url, key, buffer_size=48_000_000)
```

More in [`examples/live_relay.py`](https://github.com/AYMENJD/tgrtmp/blob/main/examples/live_relay.py).

## Keyboard-driven player

Space pauses, `m` mutes, `i` prints stats, `q` quits; the full control surface from a terminal. See [`examples/interactive_player.py`](https://github.com/AYMENJD/tgrtmp/blob/main/examples/interactive_player.py).

## The low level

`connect` + `Publisher` + `Source`, driven by hand. See [`examples/lowlevel_manual.py`](https://github.com/AYMENJD/tgrtmp/blob/main/examples/lowlevel_manual.py) and [low level](guides/low-level.md).
