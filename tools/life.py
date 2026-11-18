#!/usr/bin/env python3
"""
life.py -- generate examples/life.pie, Conway's Game of Life in Piebald.

    python tools/life.py [-o examples/life.pie] [--check]

The board is W columns by 8 rows.  Each board column is one byte -- bit r is
row r -- so a single wire carries a whole column, and every instruction works
on eight cells at once.  The board wraps top-to-bottom (a cylinder) and is
dead beyond its left and right edges.

Each board column owns a group of K wires.  One column's share of a
generation is written below as a small dataflow program (`column_program`):

    u, d  = s rotated up and down               (shifts and |)
    a', b' = u + d          as two bit-planes    (half adder: ^ &)
    a,  b  = u + s + d                           (full adder, sent to neighbours)
    N  = a_left + a_right + a'  +  2 (b_left + b_right + b')
    new = (N | s) == 3                           (Life, in one comparison)

A tiny compiler (`Compiler`) maps that program onto the K wires of a group.
Values only ever meet neighbours, so they are moved by swapping adjacent wires
(`/\\` on one row: both copies happen simultaneously), and a list scheduler
packs independent operations into the same rows.  Because every group runs
the same instructions, the row pattern is simply repeated W times across the
page, and values cross between groups through the first and last wires.

--check runs the generated program with ./piebald and compares every frame
with a straightforward Python implementation of Life.
"""
import argparse
import os
import subprocess
import sys

# ---------------------------------------------------------------------------
# The starting board.  '#' is alive.  Eight rows; width = longest line.

PATTERN = [
    "                                                ",
    "  #  #               #                          ",
    "      #               #            ###          ",
    "  #   #             ###                         ",
    "   ####                                  ##     ",
    "                                         ##     ",
    "                                                ",
    "                                                ",
]
GENERATIONS = 210


# ---------------------------------------------------------------------------
# Reference implementation

def life_step(board):
    H, W = len(board), len(board[0])
    out = []
    for r in range(H):
        row = []
        for c in range(W):
            n = 0
            for dr in (-1, 0, 1):
                for dc in (-1, 0, 1):
                    if dr == dc == 0:
                        continue
                    cc = c + dc
                    if 0 <= cc < W and board[(r + dr) % H][cc]:
                        n += 1
            row.append(n == 3 or (n == 2 and board[r][c]))
        out.append(row)
    return out


# ---------------------------------------------------------------------------
# One column's computation, as SSA operations.

def column_program():
    P = []
    op = lambda *a: P.append(a)

    # --- print the current column, one board row (bit) per output line
    op("const", "one", 1)
    op("copy", "p0", "s")
    for r in range(8):
        p, pn = f"p{r}", f"p{r + 1}"
        op("and", f"q{r}", p, "one")            # 0 or 1
        op("shl", f"w{r}", f"q{r}")              # 0 or 2
        op("or", f"t{r}", f"q{r}", f"w{r}")      # 0 or 3
        op("set", f"c{r}", f"t{r}", 5)           # ' ' or '#'
        op("print", f"c{r}", "board")
        if r < 7:
            op("shr", pn, p)
    op("const", "dash", 45)
    op("print", "dash", "sep")

    # --- vertical neighbours: rotate the column up and down
    op("shl", "x1", "s")
    op("shr", "x2", "s")
    for i in range(6):
        op("shr", f"x2_{i}", "x2" if i == 0 else f"x2_{i - 1}")
    op("or", "u", "x1", "x2_5")                  # u = s rotated left
    op("shr", "y1", "s")
    op("shl", "y2", "s")
    for i in range(6):
        op("shl", f"y2_{i}", "y2" if i == 0 else f"y2_{i - 1}")
    op("or", "d", "y1", "y2_5")                  # d = s rotated right

    # --- vertical sums
    op("xor", "a1", "u", "d")                    # u + d       = a1 + 2 b1
    op("and", "b1", "u", "d")
    op("xor", "a", "a1", "s")                    # u + s + d   = a + 2 b
    op("and", "m", "a1", "s")
    op("or", "b", "b1", "m")

    # --- horizontal: the neighbours' column sums
    op("import_l", "al", "a")
    op("import_r", "ar", "a")
    op("import_l", "bl", "b")
    op("import_r", "br", "b")
    op("xor", "ha", "al", "ar")
    op("and", "ga", "al", "ar")
    op("xor", "A", "ha", "a1")                   # ones bit of N
    op("and", "k", "a1", "ha")
    op("or", "cy", "ga", "k")                    # carry into the twos
    op("xor", "o", "bl", "br")                   # twos: bl + br + b1 + cy == 1 ?
    op("and", "e", "bl", "br")
    op("xor", "o2", "b1", "cy")
    op("and", "e2", "b1", "cy")
    op("xor", "x", "o", "o2")
    op("or", "y", "e", "e2")
    op("not", "ny", "y")
    op("or", "As", "A", "s")
    op("and", "z", "As", "x")
    op("and", "new", "z", "ny")
    op("final", "new")
    return P


# ---------------------------------------------------------------------------
# Compiler from SSA to lane actions, plus an ASAP row scheduler.

class Action:
    def __init__(self, chars, reads, writes, io=None):
        self.chars, self.reads, self.writes, self.io = chars, set(reads), set(writes), io


class Compiler:
    def __init__(self, K, home):
        self.K, self.home = K, home
        self.lanes = [None] * K          # value name in each lane, None = junk
        self.lanes[home] = "s"
        self.actions = []
        self.tmp = 0

    # -- primitive actions -------------------------------------------------
    def emit(self, *a, **kw):
        self.actions.append(Action(*a, **kw))

    def swap(self, i):
        self.emit({i: "/", i + 1: "\\"}, {i, i + 1}, {i, i + 1})
        self.lanes[i], self.lanes[i + 1] = self.lanes[i + 1], self.lanes[i]

    def pos(self, v):
        return self.lanes.index(v)

    def move(self, v, target):
        while self.pos(v) < target:
            self.swap(self.pos(v))
        while self.pos(v) > target:
            self.swap(self.pos(v) - 1)

    def hole_near(self, p, avoid=()):
        holes = [i for i, v in enumerate(self.lanes) if v is None and i not in avoid]
        if not holes:
            raise RuntimeError("out of wires")
        return min(holes, key=lambda i: abs(i - p))

    def free_lane(self, avoid=()):
        return self.hole_near(self.home, avoid)

    def dup(self, v):
        """Copy v into a neighbouring junk lane; return the copy's name."""
        p = self.pos(v)
        h = self.hole_near(p)
        # bring the hole next to v
        while h > p + 1:
            self.swap(h - 1); h -= 1
        while h < p - 1:
            self.swap(h); h += 1
        name = f"_t{self.tmp}"; self.tmp += 1
        p = self.pos(v)
        if h == p + 1:
            self.emit({h: "\\"}, {p}, {h})
        else:
            self.emit({h: "/"}, {p}, {h})
        self.lanes[h] = name
        return name

    # -- SSA operations ----------------------------------------------------
    def compile(self, prog):
        last = {}
        for i, o in enumerate(prog):
            for x in o[2:]:
                if isinstance(x, str):
                    last[x] = i
            if o[0] in ("print", "final"):
                last[o[1]] = i
        last["s"] = max(last.get("s", 0), len(prog))

        def dead_after(v, i):
            return last.get(v, -1) <= i

        unary_char = {"shl": "<", "shr": ">", "not": "~"}
        for i, o in enumerate(prog):
            kind = o[0]
            if kind == "const":
                _, dst, val = o
                h = self.free_lane()
                self.emit({h: "!"}, set(), {h})
                for b in range(8):
                    if val >> b & 1:
                        self.emit({h: str(b)}, {h}, {h})
                self.lanes[h] = dst
            elif kind in unary_char or kind in ("set", "copy"):
                dst, src = o[1], o[2]
                v = src if dead_after(src, i) else self.dup(src)
                p = self.pos(v)
                if kind == "set":
                    self.emit({p: str(o[3])}, {p}, {p})
                elif kind != "copy":
                    self.emit({p: unary_char[kind]}, {p}, {p})
                self.lanes[p] = dst
            elif kind in ("and", "or", "xor"):
                dst, x, y = o[1], o[2], o[3]
                if dead_after(x, i):
                    c, other = x, y
                elif dead_after(y, i):
                    c, other = y, x
                else:
                    c, other = self.dup(x), y
                if self.pos(other) == 0:
                    self.swap(0)
                if self.pos(c) > self.pos(other):
                    self.move(c, self.pos(other))        # c passes other
                else:
                    self.move(c, self.pos(other) - 1)
                p = self.pos(c)
                self.emit({p: {"and": "&", "or": "|", "xor": "^"}[kind]}, {p, p + 1}, {p})
                self.lanes[p] = dst
                if dead_after(other, i):
                    self.lanes[self.pos(other)] = None
            elif kind in ("import_l", "import_r"):
                dst, src = o[1], o[2]
                left = kind == "import_l"
                src_lane, dst_lane = (self.K - 1, 0) if left else (0, self.K - 1)
                self.move(src, src_lane)
                if self.lanes[dst_lane] is not None:
                    h = self.hole_near(dst_lane, avoid={src_lane})
                    while h > dst_lane:
                        self.swap(h - 1); h -= 1
                    while h < dst_lane:
                        self.swap(h); h += 1
                self.emit({dst_lane: "\\" if left else "/"}, {src_lane}, {dst_lane})
                self.lanes[dst_lane] = dst
                if dead_after(src, i):
                    self.lanes[self.pos(src)] = None
            elif kind == "print":
                _, v, tag = o
                p = self.pos(v)
                self.emit({p: "."}, {p}, set(), io=tag)
                if dead_after(v, i):
                    self.lanes[p] = None
            elif kind == "final":
                v = o[1]
                if "s" in self.lanes:
                    self.lanes[self.pos("s")] = None
                self.move(v, self.home)
            else:
                raise ValueError(kind)
            # Values that die here become junk.  Temporaries made by dup() are
            # always consumed by the operation that made them.
            if kind != "final":
                for j, name in enumerate(self.lanes):
                    if name is not None and name != "s" and (name.startswith("_t") or dead_after(name, i)):
                        self.lanes[j] = None
        return self.schedule()

    def schedule(self):
        rows = []                  # list of dict lane -> char
        io_rows = []               # row index -> io tag
        last_write = [-1] * self.K
        last_read = [-1] * self.K
        last_io = -1
        for a in self.actions:
            lo = 0
            for l in a.reads:
                lo = max(lo, last_write[l] + 1)
            for l in a.writes:
                lo = max(lo, last_write[l] + 1, last_read[l])
            if a.io:
                lo = max(lo, last_io + 1)
            r = lo
            while r < len(rows) and (set(a.chars) & set(rows[r]) or (a.io and r in io_rows_set(io_rows))):
                r += 1
            while len(rows) <= r:
                rows.append({})
                io_rows.append(None)
            rows[r].update(a.chars)
            for l in a.reads:
                last_read[l] = max(last_read[l], r)
            for l in a.writes:
                last_write[l] = r
            if a.io:
                io_rows[r] = a.io
                last_io = r
        return rows, io_rows


def io_rows_set(io_rows):
    return {i for i, t in enumerate(io_rows) if t}


# ---------------------------------------------------------------------------
# Assembly

def build(K, home=0):
    H = 8
    W = max(len(l) for l in PATTERN)
    board = [[c == "#" for c in l.ljust(W)] for l in PATTERN]
    rows, io = Compiler(K, home).compile(column_program())

    G = W * K                      # wires used by the board
    c0 = G + 1                     # generation counter n (after a guard wire)
    c1, c2, c3 = c0 + 1, c0 + 2, c0 + 3
    NL = c0 + 4
    width = NL + 1

    def blank():
        return [" "] * width

    def put(line, col, ch):
        assert line[col] == " ", (col, ch, "".join(line))
        line[col] = ch

    out = []
    # initial board: the digit r in a column sets bit r, so this block *is* the board
    for r in range(H):
        line = blank()
        for c in range(W):
            if board[r][c]:
                put(line, c * K + home, str(r))
        if r == 0:
            put(line, c2, "0")          # T = 1 so the main loop is entered
            put(line, NL, "3")
        if r == 1:
            put(line, NL, "1")
        out.append(line)
    init_rows = len(out)

    for i, row in enumerate(rows):
        line = blank()
        for c in range(W):
            for lane, ch in row.items():
                put(line, c * K + lane, ch)
        if i == 0:
            put(line, c1, "0")          # carry = 1 for n += 1
            put(line, c2, "?")          # while T != 0
        if io[i]:
            if io[i] == "sep":
                put(line, c0, "@")      # generation number
            put(line, NL, ".")
        out.append(line)

    # n += 1 (the ripple-carry adder from fizzbuzz.pie), then T = n xor GENERATIONS
    def ctl(*cells):
        line = blank()
        for col, ch in cells:
            put(line, col, ch)
        out.append(line)

    ctl((c1, "?"))
    ctl((c1, "\\"), (c2, "\\"))
    ctl((c1, "/"), (c2, "\\"), (c3, "\\"))
    ctl((c0, "^"), (c2, "&"))
    ctl((c2, "<"))
    ctl((c1, "/"))
    ctl((c1, ":"))
    bits = [b for b in range(8) if GENERATIONS >> b & 1]
    ctl((c1, "\\"), (c3, "!"))
    ctl((c1, "!"), (c2, "\\"), (c3, str(bits[0])))
    for b in bits[1:]:
        ctl((c3, str(b)))
    ctl((c2, "^"))
    ctl((c2, ":"))

    text = [
        "# Conway's Game of Life on a 48 x 8 board that wraps top to bottom.",
        "#",
        "# Each board column is one byte (bit r = row r) carried by one wire, and",
        f"# owns a group of {K} wires; the same {K}-wire circuit is repeated across the page.",
        "# Per generation each group rotates its column up and down, adds the three",
        "# vertical neighbours with bit-sliced adders, swaps sums with the groups on",
        "# either side through its edge wires, and applies  new = (neighbours | self) == 3.",
        "# Values travel by swapping adjacent wires: /\\ on one row.",
        "#",
        "# Generated by tools/life.py, which also explains the circuit.",
        "#",
        "# The initial board (digit r sets row r):",
    ]
    lines = ["".join(l).rstrip() for l in out]
    body = "\n".join(lines[:init_rows]) + "\n# ---- one generation per pass\n" + "\n".join(lines[init_rows:]) + "\n"
    return "\n".join(text) + "\n" + body, board, len(rows)


def expected_output(board):
    H, W = len(board), len(board[0])
    out = []
    for g in range(GENERATIONS):
        for r in range(H):
            out.append("".join("#" if x else " " for x in board[r]))
        out.append("-" * W + str(g))
        board = life_step(board)
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default="examples/life.pie")
    ap.add_argument("--lanes", type=int, default=0, help="wires per column (default: try a few, keep the shortest)")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--piebald", default="./piebald")
    args = ap.parse_args()

    best = None
    for K in ([args.lanes] if args.lanes else range(6, 13)):
        try:
            text, board, nrows = build(K)
        except RuntimeError:
            continue
        print(f"K={K}: {nrows} rows per generation", file=sys.stderr)
        if best is None or nrows * K < best[2] * best[3]:
            best = (text, board, nrows, K)
    text, board, nrows, K = best
    print(f"using K={K}", file=sys.stderr)
    with open(args.output, "w", newline="\n") as f:
        f.write(text)

    if args.check:
        got = subprocess.run([args.piebald, args.output], capture_output=True).stdout.decode()
        want = expected_output(board)
        if got == want:
            print(f"check: all {GENERATIONS} generations match", file=sys.stderr)
        else:
            g, w = got.splitlines(), want.splitlines()
            for i, (x, y) in enumerate(zip(g, w)):
                if x != y:
                    print(f"check: first difference at output line {i + 1}:\n  got  {x!r}\n  want {y!r}", file=sys.stderr)
                    break
            else:
                print(f"check: lengths differ ({len(g)} vs {len(w)} lines)", file=sys.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
