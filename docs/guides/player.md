# Player

[`tgrtmp.publish()`][tgrtmp.publish] hands back a [`Player`][tgrtmp.Player]: a worker thread that demuxes, paces, and reconnects, and a handful of thread-safe controls.

```python
player = tgrtmp.publish("movie.mp4", url, key)

player.pause()
player.play()
player.mute()
player.unmute()
player.stop()
```

Every control is safe to call from any thread, at any time, before or after the stream is live.

## Pause is a still frame, not a stall

Pausing re-sends the last keyframe once per `freeze_interval` seconds (default 1). Viewers see a frozen picture; the server sees a live session. The position freezes with it:

```python
player.pause()
print(player.position)   # stops moving
print(player.paused)     # True
```

On live sources, where the source keeps producing while you are paused, resume behavior depends on `buffer_size`: by default you jump back to the live edge; with a [rolling window](live-window.md) you resume from where you paused.

`freeze_interval` is a `publish()` option; lower it if your server drops idle sessions aggressively.

## State you can read

```python
player.position    # seconds into the source
player.connected   # is a session live right now
player.paused
player.muted
player.stopped     # the player has ended (stop() or a fatal error)
player.error       # the fault that ended the session, if any
```

## One dict for dashboards

```python
player.stats
# {'position': 6.1, 'paused': False, 'muted': False, 'connected': True,
#  'stopped': False, 'loops': 0, 'bitrate_kbps': 116.1, 'frames_sent': 428,
#  'video_frames_sent': 51, 'bytes_sent': 90211, 'frames_dropped': 0,
#  'window_bytes': None, 'av_skew_ms': 0, 'error': None}
```

`bitrate_kbps` is also a property on its own: a ~10 second rolling average of the stream throughput.

## Playlists

Every queue control is a Player method; there is no second object to manage. The queue accepts paths, open files, or live byte streams, and it plays on the same connection: no reconnect, no gap, and the timeline keeps moving forward, so viewers never see a rewind.

```python
player = tgrtmp.publish("intro.mp4", url, key, playlist=["a.mp4", "b.mp4"])

player.add("c.mp4")          # append; plays after everything queued
player.play_next("d.mp4")    # jump the queue: right after the current media
player.move(2, 0)            # reorder pending items
player.remove(0)             # drop a pending item (the playing one is safe)
player.remove_next()         # drop whatever would play next
player.clear()               # drop everything pending
player.next()                # skip: finish the current media early
player.has_next()            # would next() switch right now
```

`next()` is a no-op (returns `False`) when nothing is queued. A skip does not fire `on_media_end`; a media that reaches its end on its own does.

`player.playlist` is a read view for display: `playlist.index` is the playing item (None before it starts), `playlist.items` snapshots the list.

`loop="all"` cycles the whole queue, which turns a list of files into an endless channel:

```python
player = tgrtmp.publish("movie.mp4", url, key, playlist=["movie2.mp4"], loop="all")
```

`loop=True` is rejected when a playlist is given: the first file would restart forever and the queue would never move. Cycle the queue with `loop="all"` instead, or keep `loop=True` for a player without one.

The mode is writable mid-stream: `player.loop = False` stops looping at the next end of media, `player.loop = "all"` starts cycling the queue. Setting `loop = True` is rejected while the queue still has pending items; clear it first.

When the queue drains without a loop, the player holds its last frame, exactly like a single file that ends; `on_media_end` fires once per media, which is where you can chain more:

```python
def on_media_end(player):
    player.add(next_episode())

player = tgrtmp.publish("episode1.mp4", url, key, on_media_end=on_media_end)
```

A runnable version of all of this lives in [`examples/playlist.py`](https://github.com/AYMENJD/tgrtmp/blob/main/examples/playlist.py): a terminal channel where `n` skips, `a FILE` queues, `pn FILE` jumps the queue, `c` clears, and `i` prints what is playing.
