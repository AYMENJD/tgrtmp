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

## What loop is not

- Not a restart: no teardown, no handshake, no gap in the stream.
- Not for live sources: a live handle cannot rewind; a loop request on one falls back to the normal end-of-stream hold, with a log line saying so.

## Combine freely

```python
player = tgrtmp.publish(
    "playlist_item.mp4",
    url,
    key,
    loop=True,
    on_connection=on_connection,
    log=print,
)
```
