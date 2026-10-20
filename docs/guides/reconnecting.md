# Reconnecting

Drops are normal. tgrtmp reconnects with exponential backoff (1 s doubling to 30 s) and resumes the source from where it was.

## Know when it happens

```python
def on_connection(player, connected):
    if connected:
        print("live")
    else:
        print(f"dropped: {player.error}")

player = tgrtmp.publish(source, url, key, on_connection=on_connection)
```

`on_connection(player, connected)` fires on every transition: `True` when a session attaches (first time and after every reconnect), `False` when it drops, ends, or you `stop()`. It runs on the worker thread (keep it quick), and a buggy callback is caught and logged, never fatal.

`player.connected` reads the same state without a callback.

## Tune the retries

```python
player = tgrtmp.publish(
    source,
    url,
    key,
    reconnect_delay=1.0,       # first wait
    max_reconnect_delay=30.0,  # cap
    connect_timeout=15.0,
)
```

A broken source (bad container, unsupported codec) is not retried: it raises `SourceError` through `player.error` and stops. See [errors](errors.md).
