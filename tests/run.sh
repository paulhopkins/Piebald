#!/bin/sh
# Run every test program and compare its output with tests/NAME.expected.
#
# Each program is checked
#   - with the interpreter, optimised and unoptimised (-O0),
#   - in its piebald layout (examples/piebald/NAME.pie), if there is one,
#   - compiled to C with `piebald -c` and built with $CC, if CC is set.
#
# usage: tests/run.sh [path-to-piebald]       (CC="zig cc" tests/run.sh)

PIEBALD=${1:-./piebald}
TMP=tests/tmp
mkdir -p "$TMP"
pass=0
fail=0

check() { # description, actual-output-file, expected-file
  if cmp -s "$2" "$3"; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL: $1"
  fi
}

for expected in tests/*.expected; do
  name=$(basename "$expected" .expected)
  if [ -f "examples/$name.pie" ]; then prog="examples/$name.pie"; else prog="tests/$name.pie"; fi

  "$PIEBALD" "$prog" < tests/input.txt > "$TMP/out"
  check "$prog" "$TMP/out" "$expected"
  "$PIEBALD" -O0 "$prog" < tests/input.txt > "$TMP/out"
  check "$prog (-O0)" "$TMP/out" "$expected"

  if [ -f "examples/piebald/$name.pie" ]; then
    "$PIEBALD" "examples/piebald/$name.pie" < tests/input.txt > "$TMP/out"
    check "examples/piebald/$name.pie" "$TMP/out" "$expected"
  fi

  if [ -n "$CC" ]; then
    "$PIEBALD" -c "$prog" > "$TMP/$name.c" &&
      $CC -O2 -o "$TMP/$name.exe" "$TMP/$name.c" &&
      "$TMP/$name.exe" < tests/input.txt > "$TMP/out"
    check "$prog (compiled)" "$TMP/out" "$expected"
  fi
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
