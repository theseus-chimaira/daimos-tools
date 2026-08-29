#!/bin/sh
set -eu

: "${TMPDIR:?TMPDIR must be set}"
work=$TMPDIR/mkdsk-member-sectors-v1-$$
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/out"

# One tiny DAIMON stream sector is sufficient for media-layout testing.
{
        echo 0444141515557
        echo 000001000000
        echo 000000000000
} > "$work/payload.words"

./mkdsk -n 3 -m clean -p "$work/payload.words" -o "$work/out" \
    --d6fs-layout --logstore-blocks 1 --swap-tail-blocks 1 \
    --member-sectors 02000,01400,01000

set -- 1048576 786432 524288
for u in 0 1 2; do
        got=$(wc -c < "$work/out/dsk$u.dsk" | tr -d ' ')
        eval want=\${$((u + 1))}
        [ "$got" -eq "$want" ] || {
                echo "mkdsk-member-sectors-v1: dsk$u size $got != $want" >&2
                exit 1
        }
done

echo 'mkdsk-member-sectors-v1 PASS'
