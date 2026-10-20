"""A tiny VLC in the terminal: publish a file and drive it with keys, while
viewers see exactly what you do: pausing shows a still frame, muting drops
the audio only.

    space  pause / resume
    m      mute / unmute
    i      print the full stats dict
    q      stop and exit

    python interactive_player.py movie.mp4 --key YOUR_KEY
"""

import argparse
import select
import sys
import time

import tgrtmp

parser = argparse.ArgumentParser(
    description="Publish a file and control it from the keyboard."
)
parser.add_argument("source", help="media file: MPEG-TS, MP4 or MKV")

parser.add_argument(
    "--url", default="rtmps://dc4-1.rtmp.t.me/s/", help="RTMP application URL"
)
parser.add_argument("--key", required=True, help="stream key")

args = parser.parse_args()

player = tgrtmp.publish(args.source, args.url, args.key, log=print)

KEYS = "space pause/play, m mute, i stats, q quit"


def dashboard():
    s = player.stats
    state = "paused" if s["paused"] else "playing"

    if s["muted"]:
        state += ", muted"

    print(
        f"\r[{state}] pos {s['position']:6.1f}s  {s['bitrate_kbps']:7.1f} kbit/s",
        end="",
        flush=True,
    )


def toggle(predicate, on, off):
    (off if predicate() else on)()


try:
    tty = tty_open = None

    try:
        import termios

        import tty as tty_mod

        tty_mod.setcbreak(sys.stdin.fileno())
        tty = tty_mod
        tty_open = termios
    except (ImportError, termios.error, OSError):
        print(f"(not a terminal; playing headless; keys are: {KEYS})")

    while not player.stopped:
        if tty is not None:
            ready, _, _ = select.select([sys.stdin], [], [], 1.0)

            if ready:
                key = sys.stdin.read(1)

                if key == " ":
                    toggle(player.paused, player.play, player.pause)

                elif key == "m":
                    toggle(player.muted, player.unmute, player.mute)

                elif key == "i":
                    print()
                    print(player.stats)

                elif key == "q":
                    break

        else:
            time.sleep(1.0)

        dashboard()

except KeyboardInterrupt:
    pass

finally:
    if tty_open is not None:
        tty_open.tcsetattr(
            sys.stdin.fileno(),
            tty_open.TCSADRAIN,
            tty_open.tcgetattr(sys.stdin.fileno()),
        )

    player.stop()

print("\nstopped")
