# The blocking API

One call, no threads or objects to manage. For scripts that just publish and exit.

```python
import tgrtmp

publisher = tgrtmp.stream("movie.mp4", "rtmps://dc4-1.rtmp.t.me/s/", "KEY")

print(publisher.frames_sent, publisher.bytes_sent)
```

`stream()` blocks until the source ends, then returns the [`Publisher`][tgrtmp.Publisher] with its totals. It reconnects on drops, exactly like the player.

## Stop conditions

```python
import time

deadline = time.monotonic() + 30

tgrtmp.stream(source, url, key, stop=lambda: time.monotonic() >= deadline)
```

`stop` is checked between chunks and inside backoff sleeps, so shutdown is prompt. Ctrl+C works too.

## Path or handle

A path is opened and closed for you. Iterables of chunks and callables returning chunks work as sources as well: anything that produces bytes until it produces `b""`.

## When to prefer the Player

Anything interactive (controls, stats, callbacks) wants [`publish()`][tgrtmp.publish]. `stream()` is for fire-and-forget.
