#!/bin/sh
set -eu

: "${TMPDIR:?TMPDIR must be set}"
work=$TMPDIR/d6fsck-repair-v2-$$
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
    --d6fs-layout --logstore-blocks 4 --badmap-blocks 1 \
    --swap-tail-blocks 2 --member-sectors 02000 >/dev/null
./mkd6fs -n 1 -d "$work/disks" \
    -f /SYSTEM/INIT:"$work/init.words":0755:words >/dev/null
./d6fsck -n 1 -d "$work/disks" >/dev/null 2>&1

# Inject only deterministic repair cases:
# - destroy superblock A;
# - give INIT the wrong parent;
# - create a zero-length orphan FCB in slot 3;
# - mark one otherwise-free data block allocated;
# - flip the free-summary bit.
python3 - "$work/disks/dsk0.dsk" <<'PY'
import struct, sys
P=sys.argv[1]
MASK=(1<<36)-1
BW=128
FW=16

def rdw(f, sector, word):
    f.seek((sector*BW+word)*8)
    return struct.unpack('<Q',f.read(8))[0]&MASK

def wrw(f, sector, word, value):
    f.seek((sector*BW+word)*8)
    f.write(struct.pack('<Q',value&MASK))

with open(P,'r+b') as f:
    desc=[rdw(f,0,i) for i in range(0o20)]
    base=(desc[0o7]>>18)&0o777777
    sa=desc[0o10]; sb=desc[0o11]
    # Save super metadata before destroying A.
    super=[rdw(f,base+sb,i) for i in range(0o20)]
    fcb_start=super[0o11]
    freemap_start=super[0o13]
    summary_start=super[0o15]
    total=super[0o7]

    # Super A structural corruption.
    wrw(f,base+sa,0,0)

    # INIT is node 2 in this formatter fixture.  Make parent root (0), while
    # the directory entry in SYSTEM still uniquely names it.
    fcb_word=2*FW+4
    wrw(f,base+fcb_start+(fcb_word//BW),fcb_word%BW,0)

    # Slot 3: valid, allocated zero-length regular FCB but no namespace ref.
    off=3*FW
    sector=base+fcb_start+(off//BW)
    wi=off%BW
    wrw(f,sector,wi+0,(1<<33)|(0o644<<12))
    wrw(f,sector,wi+4,0)

    # Find a free block well above metadata and set its allocation bit.
    leak=max(summary_start+super[0o16]+8,total//2)
    if leak>=total:
        leak=total-2
    mbi=leak//(BW*36)
    bit=leak%(BW*36)
    wi=bit//36
    bi=bit%36
    sec=base+freemap_start+mbi
    v=rdw(f,sec,wi)
    wrw(f,sec,wi,v|(1<<(35-bi)))

    # Corrupt summary independently.
    v=rdw(f,base+summary_start,0)
    wrw(f,base+summary_start,0,v^(1<<35))
PY

if ./d6fsck -n 1 -d "$work/disks" >"$work/pre.out" 2>&1; then
        echo 'd6fsck repair fixture unexpectedly clean' >&2
        exit 1
fi
grep -q 'invalid superblock' "$work/pre.out"
grep -q 'FCB parent mismatch' "$work/pre.out"
grep -q 'unreachable FCB' "$work/pre.out"
grep -q 'allocated leaked block' "$work/pre.out"
grep -q 'free-summary mismatch' "$work/pre.out"

./d6fsck -r -n 1 -d "$work/disks" >"$work/repair.out" 2>&1
grep -q 'repair verification: clean' "$work/repair.out"
./d6fsck -n 1 -d "$work/disks" >"$work/post.out" 2>&1
grep -q 'clean$' "$work/post.out"

echo 'd6fsck-repair-v2 PASS'

# Repair ordering under a simulated host power cut.  Build a checker with a
# compile-time-only fault hook so the production binary has no crash-testing
# interface.  A one-bit summary error is enough to force the full
# DIRTY -> allocation metadata -> CLEAN protocol.
cc ${CFLAGS:--Wall -Wextra -O2 -std=c99} -DD6FSCK_TEST_FAULTS \
    -o "$work/d6fsck-fault" d6fsck.c
mkdir -p "$work/order-base"
./mkdsk -n 1 -m clean -p "$work/payload.words" -o "$work/order-base" \
    --d6fs-layout --logstore-blocks 4 --badmap-blocks 1 \
    --swap-tail-blocks 2 --member-sectors 02000 >/dev/null
./mkd6fs -n 1 -d "$work/order-base" \
    -f /SYSTEM/INIT:"$work/init.words":0755:words >/dev/null
python3 - "$work/order-base/dsk0.dsk" <<'PY'
import struct, sys
P=sys.argv[1]
MASK=(1<<36)-1
BW=128

def rdw(f, sector, word):
    f.seek((sector*BW+word)*8)
    return struct.unpack('<Q',f.read(8))[0]&MASK

def wrw(f, sector, word, value):
    f.seek((sector*BW+word)*8)
    f.write(struct.pack('<Q',value&MASK))

with open(P,'r+b') as f:
    desc=[rdw(f,0,i) for i in range(0o20)]
    base=(desc[0o7]>>18)&0o777777
    sa=desc[0o10]; sb=desc[0o11]
    a=[rdw(f,base+sa,i) for i in range(0o20)]
    b=[rdw(f,base+sb,i) for i in range(0o20)]
    super=a if a[1] >= b[1] else b
    summary_start=super[0o15]
    v=rdw(f,base+summary_start,0)
    wrw(f,base+summary_start,0,v^(1<<35))
PY

for cut in 1 2 3 4; do
        rm -rf "$work/order-$cut"
        mkdir -p "$work/order-$cut"
        cp "$work/order-base/dsk0.dsk" "$work/order-$cut/dsk0.dsk"
        set +e
        D6FSCK_TEST_ABORT_WRITE=$cut "$work/d6fsck-fault" -r -n 1 \
            -d "$work/order-$cut" >"$work/order-$cut.out" 2>&1
        rc=$?
        set -e
        if [ "$rc" -ne 99 ]; then
                echo "d6fsck fault cut $cut did not abort at durable write (rc=$rc)" >&2
                exit 1
        fi
        ./d6fsck -n 1 -d "$work/order-$cut" \
            >"$work/order-$cut.check" 2>&1 || true
        if [ "$cut" -lt 4 ]; then
                grep -q 'selected superblock .* state=DIRTY' "$work/order-$cut.check"
        fi
        ./d6fsck -r -n 1 -d "$work/order-$cut" \
            >"$work/order-$cut.repair" 2>&1
        ./d6fsck -n 1 -d "$work/order-$cut" \
            >"$work/order-$cut.final" 2>&1
        grep -q 'clean$' "$work/order-$cut.final"
done

# Ambiguous corruption must never cause -r to make opportunistic changes.
# Make two live FCBs claim the same data extent, preserve a byte-for-byte copy,
# and verify the failed repair leaves the disk member identical.
mkdir -p "$work/ambiguous"
./mkdsk -n 1 -m clean -p "$work/payload.words" -o "$work/ambiguous" \
    --d6fs-layout --logstore-blocks 4 --badmap-blocks 1 \
    --swap-tail-blocks 2 --member-sectors 02000 >/dev/null
./mkd6fs -n 1 -d "$work/ambiguous" \
    -f /SYSTEM/A:"$work/init.words":0644:words \
    -f /SYSTEM/B:"$work/init.words":0644:words >/dev/null
python3 - "$work/ambiguous/dsk0.dsk" <<'PY'
import struct, sys
P=sys.argv[1]
MASK=(1<<36)-1
BW=128
FW=16

def rdw(f, sector, word):
    f.seek((sector*BW+word)*8)
    return struct.unpack('<Q',f.read(8))[0]&MASK

def wrw(f, sector, word, value):
    f.seek((sector*BW+word)*8)
    f.write(struct.pack('<Q',value&MASK))

with open(P,'r+b') as f:
    desc=[rdw(f,0,i) for i in range(0o20)]
    base=(desc[0o7]>>18)&0o777777
    sa=desc[0o10]; sb=desc[0o11]
    a=[rdw(f,base+sa,i) for i in range(0o20)]
    b=[rdw(f,base+sb,i) for i in range(0o20)]
    super=a if a[1] >= b[1] else b
    fcb_start=super[0o11]
    # Formatter allocates ROOT=0, SYSTEM=1, then A=2, B=3.  Copy A's first
    # extent descriptor into B while leaving both namespace references live.
    aoff=2*FW+6
    boff=3*FW+6
    av=rdw(f,base+fcb_start+aoff//BW,aoff%BW)
    wrw(f,base+fcb_start+boff//BW,boff%BW,av)
PY
cp "$work/ambiguous/dsk0.dsk" "$work/ambiguous.before"
if ./d6fsck -r -n 1 -d "$work/ambiguous" \
    >"$work/ambiguous.out" 2>&1; then
        echo 'd6fsck unexpectedly repaired ambiguous cross-link' >&2
        exit 1
fi
grep -q 'ambiguous corruption remains; no repairs written' "$work/ambiguous.out"
cmp "$work/ambiguous.before" "$work/ambiguous/dsk0.dsk"

echo 'd6fsck-repair-v2 ordering PASS'
