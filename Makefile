# Any C99 compiler with GCC extensions (gcc, clang, or `zig cc`) will do:
#   make                 build the interpreter
#   make test            run the test suite (interpreted and compiled to C)
#   make CC="zig cc"     on Windows, using Zig's bundled clang

CC      = cc
CFLAGS  = -O2 -std=gnu99 -Wall -Wextra

ifeq ($(OS),Windows_NT)
EXE = .exe
endif

piebald$(EXE): piebald.c
	$(CC) $(CFLAGS) piebald.c -o $@

test: piebald$(EXE)
	CC="$(CC)" sh tests/run.sh ./piebald$(EXE)

clean:
	rm -rf piebald$(EXE) tests/tmp

.PHONY: test clean
