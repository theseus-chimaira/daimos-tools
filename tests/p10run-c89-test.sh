#!/bin/sh
set -eu

cc=${CC:-cc}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/p10run-c89-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

"$cc" -std=c89 -pedantic -Wall -Wextra -Werror \
    -o "$tmp/p10run" "$root/p10run.c"

v=$($tmp/p10run --version)
case "$v" in
    p10run-dobj-v1_*) ;;
    *) echo "unexpected p10run version: $v" >&2; exit 1 ;;
esac

# The C runner is the sole implementation; no legacy runner name remains.
test -f "$root/p10run.c"
test ! -e "$root/p10bare.c"
test ! -e "$root/legacy/p10bare.py"

echo "p10run C89 build contract: PASS"
