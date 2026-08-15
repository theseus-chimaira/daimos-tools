#!/bin/sh
set -eu

: "${TMPDIR:?TMPDIR must be set}"
work=$(mktemp -d "${TMPDIR%/}/pdp10-objdump-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

./tests/pdp10-objdump-mk "$work/test.dobj"
./pdp10-objdump -d "$work/test.dobj" >"$work/out"

grep -F 'MOVEI   1,000043' "$work/out" >/dev/null
grep -F 'POPJ    17,000000' "$work/out" >/dev/null
