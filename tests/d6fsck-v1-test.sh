#!/bin/sh
set -eu

: "${TMPDIR:?TMPDIR must be set}"
work=$TMPDIR/d6fsck-v1-$$
trap 'rm -rf "$work"' 0 1 2 3 15
mkdir -p "$work/disks"

cat > "$work/payload.words" <<'EOT'
0444141515557
000001000000
000000000000
EOT
cat > "$work/init.words" <<'EOT'
012345670123
076543210765
EOT

./mkdsk -n 1 -m clean -p "$work/payload.words" -o "$work/disks" \
    --d6fs-layout --logstore-blocks 1 --swap-tail-blocks 1 \
    --member-sectors 02000 >/dev/null
./mkd6fs -n 1 -d "$work/disks" \
    -f /SYSTEM/INIT:"$work/init.words":0755:words >/dev/null
./d6fsck -n 1 -d "$work/disks" >"$work/clean.out" 2>&1

grep -q 'clean$' "$work/clean.out"

# Corrupt only superblock A.  For this fixed tiny fixture, the D6FS physical
# base is sector 1 and superblock A is logical block 2, hence physical 3.
# d6fsck must continue from B but return non-zero because redundancy is bad.
cp "$work/disks/dsk0.dsk" "$work/dsk0.good"
dd if=/dev/zero of="$work/disks/dsk0.dsk" bs=8 seek=$((3 * 128)) count=1 conv=notrunc 2>/dev/null
if ./d6fsck -n 1 -d "$work/disks" >"$work/super.out" 2>&1; then
        echo 'd6fsck-v1: corrupt superblock was not reported' >&2
        exit 1
fi
grep -q 'invalid superblock' "$work/super.out"
grep -q 'selected superblock 3' "$work/super.out"

# Restore and flip the first free-summary bit.  The superblock says summary
# starts at logical 015 octal for this fixture, therefore physical sector 14.
cp "$work/dsk0.good" "$work/disks/dsk0.dsk"
python3 - "$work/disks/dsk0.dsk" <<'PY'
import struct, sys
p = sys.argv[1]
sector = 1 + 0o15
with open(p, 'r+b') as f:
    f.seek(sector * 128 * 8)
    raw = f.read(8)
    v = struct.unpack('<Q', raw)[0]
    v ^= 1 << 35
    f.seek(sector * 128 * 8)
    f.write(struct.pack('<Q', v))
PY
if ./d6fsck -n 1 -d "$work/disks" >"$work/summary.out" 2>&1; then
        echo 'd6fsck-v1: corrupt free summary was not reported' >&2
        exit 1
fi
grep -q 'free-summary mismatch' "$work/summary.out"

echo 'd6fsck-v1 PASS'
