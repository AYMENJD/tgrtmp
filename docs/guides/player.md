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
