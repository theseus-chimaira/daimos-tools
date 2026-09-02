#!/bin/sh
set -eu
: "${TMPDIR:?TMPDIR must be set}"
work=$TMPDIR/d6-maintenance-v2-$$
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
./mkdsk -n 3 -m clean -p "$work/payload.words" -o "$work/disks" \
    --d6fs-layout --logstore-blocks 4 --badmap-blocks 1 \
    --swap-tail-blocks 2 --member-sectors 02000,01400,01000 >/dev/null
./mkd6fs -n 3 -d "$work/disks" \
    -f /SYSTEM/INIT:"$work/init.words":0755:words >/dev/null
if ./d6bad -n 3 -d "$work/disks" --add 0:1 >"$work/bad-allocated.out" 2>&1; then
        echo 'd6bad accepted an allocated/reserved physical sector' >&2
        exit 1
fi
grep -q 'allocated block' "$work/bad-allocated.out"
./d6bad -n 3 -d "$work/disks" --add 0:0700:2 >"$work/bad.out" 2>&1
./d6bad -n 3 -d "$work/disks" --list >>"$work/bad.out"
grep -q 'member=0 physical=700 count=2' "$work/bad.out"
badlogical=$(sed -n 's/.*member=0 physical=700 count=2 logical=\([0-7][0-7]*\).*/\1/p' "$work/bad.out" | head -1)
[ -n "$badlogical" ]
# Simulate a power cut after the bad-sector table became durable but before
# the corresponding allocation-map bit was written.  d6fsck -r must derive
# the logical block from the physical bad-map entry and reserve it again.
python3 - "$work/disks" "$badlogical" <<'PYBAD'
import os, struct, sys
dirpath=sys.argv[1]
logical=int(sys.argv[2],8)
BW=128
MAGIC=0o442654636222
MASK=(1<<36)-1
def rd(f,sec,w):
    f.seek((sec*BW+w)*8)
    return struct.unpack('<Q',f.read(8))[0]&MASK
def wr(f,sec,w,v):
    f.seek((sec*BW+w)*8)
    f.write(struct.pack('<Q',v&MASK))
fps=[open(os.path.join(dirpath,'dsk%d.dsk'%i),'r+b') for i in range(3)]
try:
    members=[]
    desc0=None
    for f in fps:
        found=None
        for sec in range(0o200):
            if rd(f,sec,0o6)==MAGIC:
                words=[rd(f,sec,i) for i in range(0o20)]
                base=(words[0o7]>>18)&0o777777
                blocks=words[0o7]&0o777777
                found=(base,blocks,words)
                break
        if found is None:
            raise SystemExit('descriptor not found')
        members.append(found[:2])
        if desc0 is None:
            desc0=found[2]
    def mapblock(lbn):
        floor=0
        while True:
            active=[i for i,(_,blocks) in enumerate(members) if blocks>floor]
            if not active:
                raise ValueError('logical block outside set')
            nxt=min(members[i][1] for i in active)
            zone=(nxt-floor)*len(active)
            if lbn<zone:
                slot=lbn%len(active)
                rel=lbn//len(active)
                mi=active[slot]
                return mi,members[mi][0]+floor+rel
            lbn-=zone
            floor=nxt
    def readlogical(lbn):
        mi,sec=mapblock(lbn)
        return [rd(fps[mi],sec,i) for i in range(BW)]
    super_a=desc0[0o10]
    sup=readlogical(super_a)
    fm=sup[0o13]
    bit=logical%(BW*36)
    mbi=logical//(BW*36)
    wi=bit//36
    bi=bit%36
    mi,sec=mapblock(fm+mbi)
    v=rd(fps[mi],sec,wi)
    wr(fps[mi],sec,wi,v&~(1<<(35-bi)))
finally:
    for f in fps:
        f.close()
PYBAD
if ./d6fsck -n 3 -d "$work/disks" >"$work/bad-drift.out" 2>&1; then
        echo 'd6fsck missed bad-map/allocation drift' >&2
        exit 1
fi
./d6fsck -r -n 3 -d "$work/disks" >"$work/bad-repair.out" 2>&1
grep -q 'repair verification: clean' "$work/bad-repair.out"
./logstore -n 3 -d "$work/disks" --init >/dev/null
./logstore -n 3 -d "$work/disks" --append -s 3 -S 0123 -t 1234 -p 0777 >/dev/null
./logstore -n 3 -d "$work/disks" --append -s 5 -S 0456 -t 5678 -p 01234 -p 05670 >/dev/null
./logstore -n 3 -d "$work/disks" --dump >"$work/log.out"
grep -q 'seq=1 ' "$work/log.out"
grep -q 'seq=2 ' "$work/log.out"
cp -R "$work/disks" "$work/prepack-source"
./packfs -n 3 -d "$work/disks" --output "$work/packed" >/dev/null
./d6fsck -n 3 -d "$work/disks" >"$work/fsck-prepack-source.out" 2>&1
grep -q 'clean$' "$work/fsck-prepack-source.out"
./d6fsck -n 3 -d "$work/packed" >"$work/fsck-packed.out" 2>&1
grep -q 'clean$' "$work/fsck-packed.out"
for m in 0 1 2; do
        cmp "$work/prepack-source/dsk$m.dsk" "$work/disks/dsk$m.dsk"
done
./d6swap -n 3 -d "$work/disks" --plan --ram-words 040000 >"$work/swap-plan.out"
grep -q 'policy=1x-RAM' "$work/swap-plan.out"
./d6swap -n 3 -d "$work/disks" --resize-tail 3 --output "$work/resized" >"$work/swap-resize.out" 2>&1
./d6swap -n 3 -d "$work/disks" --show >"$work/swap-source.out"
grep -q 'tail-blocks/member=2' "$work/swap-source.out"
./d6swap -n 3 -d "$work/resized" --show >"$work/swap-show.out"
grep -q 'tail-blocks/member=3' "$work/swap-show.out"
./d6fsck -n 3 -d "$work/disks" >"$work/fsck-source.out" 2>&1
grep -q 'clean$' "$work/fsck-source.out"
./d6fsck -n 3 -d "$work/resized" >"$work/fsck.out" 2>&1
grep -q 'clean$' "$work/fsck.out"
echo 'd6-maintenance-v2 PASS'
