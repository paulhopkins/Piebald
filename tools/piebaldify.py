#!/usr/bin/env python3
"""
piebaldify.py -- lay a Piebald program out so that, zoomed out, it looks like
the coat of a piebald animal: big irregular dark patches on a white ground.

    python tools/piebaldify.py program.pie [-o out.pie] [--width 140]
                               [--height 90] [--seed 7] [--coverage 0.45]

The program's behaviour is unchanged:

  * Its own instructions keep their relative columns (the "code band"), and
    its rows keep their order.  Extra rows are only added when they contain
    nothing but no-ops and dead wires, so they change only the timing.
  * Dark patches outside the code band are made of real instructions on dead
    wires: wires that are never printed, never tested, and never read by a
    live wire.  They compute busily and pointlessly, and the interpreter's
    dead-wire elimination deletes every one of them before running.
  * Inside the code band, and on the guard wire either side of it, the
    patches are made of letters, which are always no-ops.

Programs that slide (use ( or )) can wander onto any wire, so for those every
patch is made of letters.
"""
import argparse
import math
import random
import sys

OPS = set("01234567!~<>|&^/\\{},.@?:()")  # { } are the 2012 spellings of / \
DEAD_OPS = "01234567!~<>|&^/\\"  # no I/O, no branches, no slides
HIDE = "MWNHBEKRDGQAXmwnh"      # dense letters for patches over live wires


def read_rows(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n").split("#", 1)[0]
            if not line:
                continue  # blank and comment-only lines are not rows
            if "\t" in line:
                sys.exit(f"{path}: tabs are not allowed")
            rows.append("".join(c if c in OPS else " " for c in line).rstrip())
    return rows


class Noise:
    """Smooth 2-D value noise, a few octaves."""

    def __init__(self, rng, octaves=4):
        self.tables = [{} for _ in range(octaves)]
        self.rng = rng

    def _lattice(self, t, ix, iy):
        k = (ix, iy)
        if k not in t:
            t[k] = self.rng.random()
        return t[k]

    def at(self, x, y, scale):
        total, amp, norm = 0.0, 1.0, 0.0
        for o, t in enumerate(self.tables):
            fx, fy = x / scale * 2 ** o, y / scale * 2 ** o
            ix, iy = math.floor(fx), math.floor(fy)
            sx, sy = fx - ix, fy - iy
            sx, sy = sx * sx * (3 - 2 * sx), sy * sy * (3 - 2 * sy)
            a = self._lattice(t, ix, iy)
            b = self._lattice(t, ix + 1, iy)
            c = self._lattice(t, ix, iy + 1)
            d = self._lattice(t, ix + 1, iy + 1)
            total += amp * ((a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy)
            norm += amp
            amp *= 0.5
        return total / norm


def coat(width, height, coverage, rng, aspect=2.0):
    """A boolean mask: True where the coat is dark.  `aspect` is pixel height / width."""
    # Characters are about twice as tall as they are wide, so measure
    # distances in "square" units: y counts double.
    W, H = width, height * aspect
    blobs = []
    for _ in range(rng.randint(7, 12)):
        r = rng.uniform(0.07, 0.20) * (W + H) / 2
        blobs.append((rng.uniform(0.0, 1.0) * W, rng.uniform(0.0, 1.0) * H,
                      r * rng.uniform(0.8, 1.6), r * rng.uniform(0.5, 1.0),
                      rng.uniform(0, math.pi), rng.uniform(0.7, 1.0)))
    noise = Noise(rng)
    field = []
    for y in range(height):
        Y = y * aspect
        row = []
        for x in range(width):
            v = 0.0
            for bx, by, rx, ry, th, w in blobs:
                dx, dy = x - bx, Y - by
                u = dx * math.cos(th) + dy * math.sin(th)
                t = -dx * math.sin(th) + dy * math.cos(th)
                v = max(v, w * math.exp(-((u / rx) ** 2 + (t / ry) ** 2)))
            # ragged edges and the odd island
            v += 0.6 * (noise.at(x, Y, 18.0) - 0.5)
            row.append(v)
        field.append(row)
    flat = sorted(v for row in field for v in row)
    cut = flat[int((1 - coverage) * (len(flat) - 1))]
    return [[v > cut for v in row] for row in field]


def piebaldify(rows, width, height, coverage, seed):
    rng = random.Random(seed)
    code_w = max((len(r) for r in rows), default=0)
    slides = any(c in "()" for r in rows for c in r)
    width = max(width, code_w + 4)
    height = max(height, len(rows))

    # Where the code band goes: somewhere in the middle third.
    lo = max(1, (width - code_w) // 3)
    hi = max(lo, (width - code_w) * 2 // 3)
    x0 = rng.randint(lo, hi)
    guard_l, guard_r = x0 - 1, x0 + code_w  # wires that must stay op-free

    # Spread the code rows over the canvas, keeping their order.
    extra = height - len(rows)
    cuts = sorted(rng.randint(0, len(rows)) for _ in range(extra))
    canvas_rows = []
    k = 0
    for i in range(len(rows) + 1):
        while k < len(cuts) and cuts[k] == i:
            canvas_rows.append(None)  # a pattern-only row
            k += 1
        if i < len(rows):
            canvas_rows.append(rows[i])

    mask = coat(width, len(canvas_rows), coverage, rng)
    out = []
    for y, code in enumerate(canvas_rows):
        line = []
        for x in range(width):
            dark = mask[y][x]
            in_band = guard_l <= x <= guard_r
            if in_band and code is not None and 0 <= x - x0 < len(code) and code[x - x0] != " ":
                line.append(code[x - x0])          # a real instruction
            elif not dark:
                line.append(" ")
            elif in_band or slides:
                line.append(rng.choice(HIDE))       # a no-op
            else:
                line.append(rng.choice(DEAD_OPS))   # a dead wire
        text = "".join(line).rstrip()
        out.append(text if text else " ")           # keep the row: it may be a jump target
    return out, x0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("program")
    ap.add_argument("-o", "--output")
    ap.add_argument("--width", type=int, default=140)
    ap.add_argument("--height", type=int, default=0, help="rows (default: about 2x the program)")
    ap.add_argument("--coverage", type=float, default=0.45, help="fraction of the coat that is dark")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    rows = read_rows(args.program)
    height = args.height or max(40, len(rows) * 2)
    out, x0 = piebaldify(rows, args.width, height, args.coverage, args.seed)
    name = args.program.replace("\\", "/").split("/")[-1]
    text = (f"# {name}, piebaldified (seed {args.seed}).  The real code is in wires {x0}-{x0 + max(map(len, rows)) - 1};\n"
            f"# every other instruction is on a dead wire and is optimised away.\n"
            + "\n".join(out) + "\n")
    if args.output:
        with open(args.output, "w", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
