#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${TMPDIR:-/tmp}/pdp10-tools-install-cleanup-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM

prefix=$work/root/usr/local
bindir=$prefix/bin
mkdir -p "$bindir"

stale='pdp10-dec-none-ar pdp10-dec-none-ranlib dxr2rim mkrim mkrim.py p10bare p10bare.py'
for f in $stale
do
    printf 'stale\n' > "$bindir/$f"
done

${MAKE:-make} -C "$root" install DESTDIR="$work/root" >/dev/null

for f in $stale
do
    test ! -e "$bindir/$f" || {
        echo "install left stale tool: $f" >&2
        exit 1
    }
done

test -x "$bindir/p10run"
test -x "$bindir/darc"
test -L "$bindir/pdp10-dec-none-darc"

${MAKE:-make} -C "$root" uninstall DESTDIR="$work/root" >/dev/null

for f in mkdsk mkdt mkstream mktap words2pt dlink darc p10run \
    pdp10-dec-none-darc $stale
do
    test ! -e "$bindir/$f" || {
        echo "uninstall left tool: $f" >&2
        exit 1
    }
done

echo "install stale-tool cleanup contract: PASS"
