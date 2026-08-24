#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${TMPDIR:-/tmp}/mktap-mtc-7track-v1-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

cat > "$work/words" <<'EOF_WORDS'
000000000000
010203040506
0777777777777
EOF_WORDS

"$root/mktap" -t mtc -p "$work/words" -o "$work/tape"

actual=$(od -An -v -t o1 "$work/tape" | tr -s '[:space:]' ' ' | sed 's/^ //;s/ $//')
expected='022 000 000 000 100 100 100 100 100 100 001 002 103 004 105 106 177 177 177 177 177 177 022 000 000 000 000 000 000 000'

if test "$actual" != "$expected"; then
        echo "mktap 7-track image mismatch" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        exit 1
fi

echo "mktap Type 516 7-track odd-parity media: PASS"
