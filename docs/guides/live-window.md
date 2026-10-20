# The live window

An opt-in rolling buffer that lets pause hold its point on live sources.

## When it is off

By default (`buffer_size=0`) nothing is buffered: there is no reader thread and no memory set aside. Pause still works; the picture freezes on the connection, but the live source keeps producing, so resume jumps to the live edge. Seekable sources (paths, open files) never need the window: pause freezes the frame directly, and `player.window_bytes` is `None`.

## Turning it on

```python
player = tgrtmp.publish(live_handle, url, key, buffer_size=48_000_000)
```

A live source cannot be paused from the outside; it keeps producing. With a window, tgrtmp splits the job:

- a reader thread drains the source into a window of `buffer_size` bytes,
- the player plays from the window at real-time pace.

```text
live edge ──▶ [========== window ==========] ──▶ (dropped oldest)
                        ▲
                  your playhead (paused)
```

While you play at the live edge, the window is nearly empty. Pause, and the live edge keeps filling it; your point stays put. `player.window_bytes` shows the fill.

## Sizing it

`buffer_size` is how far behind the live edge you can afford to be. The held time is roughly `buffer_size / bitrate`:

| `buffer_size` | 1.3 Mbps | 6 Mbps |
| --- | --- | --- |
| 48 MB | ~5 min | ~1 min |
| 192 MB | ~20 min | ~4 min |

## When a pause outlives the window

The window drops its oldest bytes first. Resume after too long and playback continues from the oldest buffered data; the player logs it, nothing breaks, the stream never went silent.
