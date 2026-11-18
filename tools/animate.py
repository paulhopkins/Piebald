#!/usr/bin/env python3
"""
animate.py -- play the output of examples/life.pie as an animation.

    ./piebald examples/life.pie | python tools/animate.py [seconds-per-frame]

Each frame ends with a separator line starting with '-'.  The frame is drawn
at the top of the screen, replacing the previous one, followed by a pause
(default 0.1 s).  It uses ANSI escape codes, which work in most terminals,
including Windows Terminal and the VS Code terminal.

The timing lives here rather than in the Piebald program because a program
has no way to sleep, and the interpreter and compiled versions run at very
different speeds.
"""
import sys
import time


def main():
    delay = float(sys.argv[1]) if len(sys.argv) > 1 else 0.1
    out = sys.stdout
    out.write("\x1b[2J\x1b[?25l")          # clear the screen, hide the cursor
    frame = []
    try:
        for line in sys.stdin:
            frame.append(line)
            if line.startswith("-"):        # the separator ends a frame
                out.write("\x1b[H" + "".join(frame))
                out.flush()
                frame = []
                time.sleep(delay)
    except KeyboardInterrupt:
        pass
    finally:
        out.write("\x1b[?25h")              # show the cursor again
        out.flush()


if __name__ == "__main__":
    main()
