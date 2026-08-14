#!/bin/sh
set -eu

cc=${CC:-cc}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/p10bare-c89-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"

"$cc" -std=c89 -pedantic -Wall -Wextra -Werror \
    -o "$tmp/p10bare" "$root/p10bare.c"

v=$($tmp/p10bare --version)
case "$v" in
    p10bare-dobj-v1_*) ;;
    *) echo "unexpected p10bare version: $v" >&2; exit 1 ;;
esac

# The Python implementation is legacy only; the active runner is C.
test -f "$root/legacy/p10bare.py"
test -f "$root/p10bare.c"

echo "p10bare C89 build contract: PASS"
