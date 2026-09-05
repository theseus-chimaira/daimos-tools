#!/bin/sh
set -eu

: "${TMPDIR:?TMPDIR must be set}"
work=$TMPDIR/mkd6fs-dir-v1-$$
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/disks"
cat > "$work/payload.words" <<'EOT'
0444141515557
000001000000
000000000000
EOT

./mkdsk -n 1 -m clean -p "$work/payload.words" -o "$work/disks" --d6fs-layout \
    --member-sectors 02000 >/dev/null
./mkd6fs -n 1 -d "$work/disks" \
    -D /MOUNT/RAMFS0:0755 -D /TEMP:0555 -D /MOUNT/DT0:0755 >/dev/null
./d6fsck -n 1 -d "$work/disks" >"$work/check.out" 2>&1
grep -q 'clean$' "$work/check.out"

echo 'mkd6fs-dir-v1 PASS'
