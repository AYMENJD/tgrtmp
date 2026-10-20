from ._core import Client, Source, connect
from ._version import __version__
from .client import Publisher, SourceError, needs_transcode, stream
from .player import Player, publish

__all__ = [
    "Client",
    "Player",
    "Publisher",
    "Source",
    "SourceError",
    "connect",
    "needs_transcode",
    "publish",
    "stream",
    "__version__",
]

__copyright__ = "Copyright (c) 2026 AYMENJD"
__license__ = "MIT License"
