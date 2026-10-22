# Monitoring

Everything the session does is readable while it runs.

## `stats`, one call

```python
player.stats
# {'position': 6.1, 'paused': False, 'muted': False, 'connected': True,
#  'stopped': False, 'loops': 0, 'bitrate_kbps': 116.1, 'frames_sent': 428,
#  'video_frames_sent': 51, 'bytes_sent': 90211, 'frames_dropped': 0,
#  'window_bytes': None, 'av_skew_ms': 0, 'backlog_ms': 12, 'send_blocked_ms': 3,
#  'error': None}
```

`av_skew_ms` is the stream skew of the audio timeline against the video one; near 0 and constant is healthy.

## Is the connection keeping up?

Two numbers answer it:

```python
player.backlog_ms       # how far queued media is past its send time
player.send_blocked_ms  # cumulative time stuck inside socket sends
```

`backlog_ms` near zero is healthy (frames leave as they come due); it grows when the connection cannot drain the stream as fast as the source produces it, and `send_blocked_ms` growing quickly is the same verdict measured from inside the socket. A sustained backlog also says so in your `log` (`warning: the connection is struggling to carry the stream`).

## Throughput

```python
player.bitrate_kbps   # ~10 s rolling average of the stream
```

It self-samples: poll it once a second and it is current.

## Warnings

`player.warnings` lists everything the server will not accept: wrong dimensions, non-16:9 aspect ratio, unusual sample rates. Nothing fails silently; each warning is also pushed to your `log` callback prefixed with `warning:`.

```python
player = tgrtmp.publish(source, url, key, log=print)
```

The log narrates the session: connects, drops and retries, pause-outlived-window, end-of-stream holds, loop restarts.

## Periodic dashboard

```python
while not player.stopped:
    print(player.stats)
    time.sleep(1)
```

Or drive it from `on_connection` plus your own timer; the player never blocks on readers.
