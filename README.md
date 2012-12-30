Piebald
=======

A Turing Tarpit for the www.pltgames.com December 2012 game.

The system memory is made up of 1-byte cells. In this implementation there is 32000 cells. Each line of code includes a number of instructions which act on the memory cells. Each instruction acts on the cell in the same column and the instructions update the memory cells simultaneously.

The instructions are:

```
!               Zero the cell
0, 1, 2,..., 7  Set the n-th bit
~               Apply NOT operator
|, &, ^         Apply OR, AND, XOR operator with cell to the right         
<, >            Left shift, right shift
}               Copy from cell to the right
{               Copy from cell to the left
,               Input character to cell
.               Output character from cell
@               Output integer from cell
?               If cell is zero, jump to matching ?
:               If cell is non-zero, jump to matching :
#               Everything after is a comment
```

Usage
=====

To build type

```
make
```

and then

```
./piebald filename.pie    e.g. ./piebald examples/helloworld.pie  
```

Some example codes can be found in the examples directory.

Hello World
===========

<pre>
# Set the character codes
6666656666653 
35555 4555501
 2333 23432  
 0222 1212   
    1 01     
    0  0
# Output the characters
................
</pre>
