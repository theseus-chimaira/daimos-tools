#!/bin/sh
# Test mkinitfs0 DXR v1 encoding support.
set -eu

: "${TMPDIR:?TMPDIR must be set}"

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work="$TMPDIR/pdp10-tools-mkinitfs0-dxr-v1-test.$$"
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# Create a minimal 4-word DXR1 binary fixture without Python.
#
# DXR container: each 36-bit word is stored in the lower 36 bits of an
# 8-byte little-endian value.  Bits 36-63 must be zero.
#
# Words:
#   [0] = 0o447062210000  magic: SIXBIT("DXR1  ")  (left half = SIXBIT("DXR"))
#   [1] = 0o000001000000  image_words = 1 in left half
#   [2] = 0o012345670123  the single image word
#   [3] = 0o400000000000  the single reloc word (1 word for ceil(1/36)=1)
#
# Byte layout for each word (little-endian, word value W in lower 36 bits):
#   bytes 0-3 = W bits 0-31, bytes 4-7 = W bits 32-35 (upper 4 bits only)
#
# Word 0 = 39607406592 = 0x938C91000: 00 10 C9 38 09 00 00 00
# Word 1 = 262144      = 0x040000:    00 00 04 00 00 00 00 00
# Word 2 = 1402433619  = 0x53977053:  53 70 97 53 00 00 00 00
# Word 3 = 34359738368 = 0x800000000: 00 00 00 00 08 00 00 00
printf '\000\020\311\070\011\000\000\000\000\000\004\000\000\000\000\000\123\160\227\123\000\000\000\000\000\000\000\000\010\000\000\000' \
    > "$work/init.dxr"

"$root/mkinitfs0" --format words -o "$work/initfs.words" \
    "/SYSTEM/INIT:$work/init.dxr:555:dxr"

# The data section is the last 4 words of the output (after header + entries +
# string table).  The data must be the 4 DXR words verbatim.
tail -n 4 "$work/initfs.words" > "$work/data.words"

cat > "$work/expected.words" <<'EOF_WORDS'
447062210000
000001000000
012345670123
400000000000
EOF_WORDS

cmp "$work/expected.words" "$work/data.words"
echo "mkinitfs0 DXR v1 test: PASS"
