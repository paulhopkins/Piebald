Piebald
=======

A Turing tar-pit for the www.pltgames.com December 2012 game.

A Piebald program is a circuit board. Every **column is a wire** carrying one
byte, and every **line is a moment in time**. An instruction acts on the wire
in its own column, and all the instructions on a line fire **at once**. Data
moves between neighbouring wires, and loops run up and down a single wire.

```
# Hello World: each column is one character, each digit sets one bit
6666656666653
35555 4555501
 2333 23432
 0222 1212
    1 01
    0  0
# Print the thirteen wires
.............
```

The machine
-----------

* A circular tape of 65,536 byte cells, all initially zero.
* The program is placed on the tape: column *c* of the source is wired to cell
  *base + c*. The base starts at 0 and moves only with `(` and `)`.
* Rows run top to bottom. Each row is executed **simultaneously**: every
  instruction reads the cells as they were before the row started. So `/\`
  swaps two wires, and `\\\` shifts three wires along by one.
* Input and output on a row happen left to right.

Instructions
------------

```
0 1 2 ... 7   set bit n of the cell
!             zero the cell
~             NOT
<  >          shift left / shift right by one bit
|  &  ^       OR / AND / XOR with the cell to the right
\             copy from the cell to the left    (data runs down and to the right)
/             copy from the cell to the right   (data runs down and to the left)
,             read a byte into the cell (end of input reads as 0)
.             write the cell as a byte
@             write the cell as a decimal number
?             if the cell is zero, jump to the row after the matching :
:             if the cell is non-zero, jump back to the matching ? row
(  )          after this row, slide the program one cell left / right along the tape
#             comment to the end of the line
```

The copy instructions draw the path the data takes. A value carried three
wires to the right over three rows is a diagonal wire on the page:

```
\
 \
  \
```

`{` and `}` are accepted as old spellings of `/` and `\`, so programs from 2012
still run.

Any other character is a no-op, and letters will always be no-ops, so you can
write short notes inline (keep digits and symbols out of them!). Tabs are an
error, because column positions matter.

**Loops.** A `?` pairs with the next `:` below it *in the same column*, nesting
like brackets. A `:` jumps back to its `?` row, which runs again in full, so
`?`...`:` is a while loop on one wire. Loops on different wires may overlap
freely (crossed wires). A row may hold at most one `?` or `:`. To make an
*if*, clear the wire before the `:`:

```
?     if the wire is non-zero
.       do something
!       clear it
:     so this does not loop
```

**Slides.** `(` and `)` move the whole program along the tape after the row
has run. Their column does not matter, and several on one row add up. Sliding
is what lets a fixed-width program reach unbounded memory. Without it, a
program can only touch as many cells as it has columns.

**Blank lines** and comment-only lines are ignored. A line of spaces is a row
that does nothing, which just takes a moment.

Usage
-----

```
make                      # or: make CC="zig cc"  (any gcc/clang-compatible C99 compiler)
./piebald examples/fizzbuzz.pie
./piebald -c examples/rule30.pie > rule30.c && cc -O2 rule30.c -o rule30   # compile to C
make test
```

Options: `-c` writes an equivalent C program instead of running, `-O0` turns
off optimisation, and `-s` prints statistics to stderr.

Examples
--------

| File | What it does |
|------|--------------|
| [helloworld.pie](examples/helloworld.pie) | Hello World |
| [addition.pie](examples/addition.pie) | 79 + 15 with a ripple-carry adder: XOR is the sum, AND shifted left is the carry |
| [cat.pie](examples/cat.pie) | copies input to output |
| [fizzbuzz.pie](examples/fizzbuzz.pie) | FizzBuzz from 1 to 100, using one-hot counters for the 3s and 5s |
| [rule30.pie](examples/rule30.pie) | the Rule 30 cellular automaton on 96 cells: one generation is four simultaneous rows |
| [reverse.pie](examples/reverse.pie) | prints its input backwards; slides along the tape, so it handles input of any length |
| [bench.pie](examples/bench.pie) | three nested 8-bit counters (16.6M increments), used as a benchmark |

### Piebald patterns

[examples/piebald/](examples/piebald) holds the same programs laid out to look
like a piebald coat when zoomed out (try your editor's minimap). They behave
exactly like the originals. Their dark patches are made of real instructions
on **dead wires**: wires that are never printed, never tested, and never read
by a live wire. Those instructions compute pointlessly, and the interpreter
deletes them before running. Where the patches cross the real code, they are
made of letters. The live code shows through the white areas as ticking.

```
python tools/piebaldify.py examples/fizzbuzz.pie --seed 6 --height 80 -o coat.pie
```

Design notes
------------

Changes from the 2012 version:

* **Turing-completeness.** Instructions are bound to columns, so the original
  language could only ever reach as many cells as the program had columns. It
  was a finite-state machine. The new `(` `)` slide instructions move the
  program along the tape, like a Brainfuck tape head moving a whole board at
  once. This restores Turing-completeness (up to the tape size, as in
  Brainfuck), and no existing program changes meaning.
* **Defined behaviour.**
  * The tape is zeroed. It was uninitialised stack memory.
  * The tape wraps at both ends. A `\` in column 0 used to read `cells[-1]`.
  * End of input reads as 0. It used to read as 255, so `cat` never stopped.
  * Nested loops in one column now match like brackets. They used to mis-pair.
  * There is no limit on loops or lines. It was 100 loops and 10,000 lines,
    unchecked.
* **Wires you can see.** The copy instructions are now `\` and `/`, which draw
  the diagonal the data travels along. The old `{` and `}` still work, and the
  old README had them the wrong way round: `}` copies from the *left*.
* **Error messages** give the file, line and column.

Ideas for later that were *not* adopted, to keep the tar-pit small: allowing
several branches on one row (it is unclear what that would mean), and a
decrement instruction. Counting down is a nice exercise with the adder.

Implementation
--------------

`piebald.c` is a single file with no dependencies beyond libc.

1. **Parsing** resolves every `?`/`:` pair up front with a per-column stack.
2. **Dead-wire elimination.** For programs that never slide, the interpreter
   finds every wire whose value can reach output or a branch, following
   neighbour reads backwards, and deletes instructions on all other wires.
   Rows left empty are removed and jumps are retargeted.
3. **Lowering.** Each row becomes a straight sequence of micro-ops. To keep
   the simultaneous semantics without double-buffering, the ops of a row are
   topologically sorted so that every cell is read before it is overwritten.
   Only genuine cycles, like the swap `/\`, need a temporary.
4. **Execution** uses a direct-threaded interpreter (computed goto), with a
   separate fast path for programs that never slide.
5. **`-c`** instead translates the program to C, one block per row with
   `goto`s for the branches. A C compiler can then keep the wires in registers.

On `examples/bench.pie` (282 million rows executed), on one Windows laptop:

| | time | speed-up |
|---|---|---|
| original interpreter | 6.1 s | 1× |
| new interpreter | 1.3 s | 4.6× |
| compiled with `-c` | 0.28 s | 21× |
