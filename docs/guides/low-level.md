# Low level

Under the player: the low-level API, driven by hand. [`connect()`][tgrtmp.connect] opens the RTMP(S) session, [`Publisher`][tgrtmp.Publisher] paces frames to real time, [`Source`][tgrtmp.Source] demuxes the container.

This is everything `stream()` does, written out, for when you want the loop yourself.

```python
from tgrtmp import SourceError, connect
from tgrtmp._core import Publisher, Source

publisher = Publisher(lookahead_ms=1000)
publisher.set_logger(lambda message: print("warning:", message))

client = connect("rtmps://dc4-1.rtmp.t.me/s/", "KEY")
publisher.attach(client)

source = Source(publisher)

try:
    with open("movie.mp4", "rb") as media:
        while chunk := media.read(65536):
            source.feed(chunk)
    source.finish()
except RuntimeError as error:
    if client.is_closed:
        raise ConnectionError(str(error)) from error
    raise SourceError(str(error)) from error
finally:
    publisher.detach()
    client.close()

publisher.drain(60.0)
publisher.stop()
```

## How the pieces fit

- `feed()` blocks on a small lookahead window (`lookahead_ms`): feeding as fast as the file reads is safe; the pacer sets the pace.
- `on_discontinuity()` re-anchors the pacer when source timestamps jump.
- `attach()` / `detach()` swap connections under a live publisher; statistics survive.

## The native module

`tgrtmp._core` also exposes the protocol primitives the test suite uses — `chunk_write`, `amf0_encode`, `adts_parse`, and friends. The generated stubs (`tgrtmp/_core.pyi`) document all of it; type checkers pick them up automatically.
