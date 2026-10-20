"""A playlist channel: several files play in order on one connection, with
the queue editable while live. Type commands while it runs:

    n          skip to the next item
    a FILE     append a file to the queue
    pn FILE    play a file right after the current one
    c          drop every pending item
    i          show what is playing and what is queued
    q          quit

    python playlist.py intro.mp4 movie.mp4 extra.mp4 --key YOUR_KEY
"""

import argparse
import threading
import time

import tgrtmp


def now_playing(player):
    index = player.playlist.index
    items = player.playlist.items

    if index is None:
        head = "(the first file)"
    else:
        head = f"{items[index]}"

    pending = items[index + 1 :] if index is not None else items
    print(f"now playing: {head}; pending: {pending or 'nothing'}")


def main():
    parser = argparse.ArgumentParser(
        description="Play several files in order as one livestream."
    )
    parser.add_argument("sources", nargs="+", help="media files, in play order")

    parser.add_argument(
        "--url", default="rtmps://dc4-1.rtmp.t.me/s/", help="RTMP application URL"
    )
    parser.add_argument("--key", required=True, help="stream key")

    args = parser.parse_args()

    if len(args.sources) < 2:
        parser.error("give at least two files so the queue has something to do")

    # The first file plays first, then the playlist items in order; the
    # whole list rides one connection.
    player = tgrtmp.publish(
        args.sources[0],
        args.url,
        args.key,
        playlist=args.sources[1:],
        log=print,
    )

    def commands():
        while True:
            line = input().split(maxsplit=1)

            if not line:
                continue

            word, rest = line[0], line[1] if len(line) > 1 else ""

            if word == "n":
                if not player.next():
                    print("nothing queued; the current file keeps playing")
            elif word == "a" and rest:
                index = player.add(rest)
                print(f"queued at {index}: {rest}")
            elif word == "pn" and rest:
                player.play_next(rest)
                print(f"plays right after this one: {rest}")
            elif word == "c":
                player.clear()
                print("queue cleared; the current file finishes alone")
            elif word == "i":
                now_playing(player)
            elif word == "q":
                return

    threading.Thread(target=commands, daemon=True).start()

    try:
        while not player.stopped:
            time.sleep(1)

    except KeyboardInterrupt:
        pass

    finally:
        player.stop()

    print("totals:", player.stats)


if __name__ == "__main__":
    main()
