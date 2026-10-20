# 24/7 channels

One flag turns a file into an endless livestream.

```python
player = tgrtmp.publish("movie.mp4", url, key, loop=True)
```

At EOF the source restarts from the beginning; same connection, no reconnect. Each pass continues the same timeline (the next pass starts one millisecond past the highest timestamp sent), so viewers and the server see one uninterrupted stream.

`player.stats["loops"]` counts completed passes.

```python
while not player.stopped:
    s = player.stats
    print(f"{s['position']:6.1f}s  loop {s['loops']}  {s['bitrate_kbps']:.0f} kbit/s")
    time.sleep(1)
```

`loop="all"` applies the same idea to a [playlist](player.md#playlists): the queue cycles back to its first item when it drains, one uninterrupted stream across all files.

```python
player = tgrtmp.publish("a.mp4", url, key, playlist=["b.mp4", "c.mp4"], loop="all")
```

A player with a playlist rejects `loop=True`: the first file would restart forever and the queue would never move. Cycle the queue with `loop="all"` instead, or keep `loop=True` for a player without one.

## What loop is not

- Not a restart: no teardown, no handshake, no gap in the stream.
- Not for live sources: a live handle cannot rewind; a loop request on one falls back to the normal end-of-stream hold, with a log line saying so.

## Combine freely

```python
player = tgrtmp.publish(
    "movie.mp4",
    url,
    key,
    loop=True,
    on_connection=on_connection,
    log=print,
)
```
