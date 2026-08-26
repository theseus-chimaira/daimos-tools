#!/bin/sh
# Tests for mkinitfs0 C tool.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MKINITFS0="$root/mkinitfs0"

work=${TMPDIR:-/tmp}/mkinitfs0-test-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# Pack 4 decimal byte values (0-511 each) into a 36-bit PDP-10 word,
# expressed as a 12-digit octal string.
pack4() {
	awk -v a="$1" -v b="$2" -v c="$3" -v d="$4" \
	    'BEGIN { printf "%012o\n", a * 134217728 + b * 262144 + c * 512 + d }'
}

# Extract all .word octal values from an asm output file, one per line.
words_from() {
	sed -n 's/^[[:space:]]*\.word[[:space:]]*0\([0-7]*\).*/\1/p' "$1"
}

# Return the Nth (0-based) .word value from a file.
word_at() {
	words_from "$1" | awk -v n="$(($2 + 1))" 'NR==n{print;exit}'
}

check() {
	label=$1 actual=$2 expected=$3
	if [ "$actual" != "$expected" ]; then
		printf 'FAIL %s: got %s, want %s\n' "$label" "$actual" "$expected" >&2
		exit 1
	fi
}

# --- test_generates_hierarchical_image_with_halt ---

printf '0044457060\n0000000001\n0000000000\n0000000001\n' > "$work/init.dex0"
printf 'H\n' > "$work/halt.dex0"
printf 'OK\n' > "$work/readme"
printf '.text\ninit_start:\n\tmovei 1,1\n\tpushj 17,mach_syscall\n' \
    > "$work/init.s"

"$MKINITFS0" \
    "/SYSTEM/INIT:$work/init.dex0:555:words" \
    "/SYSTEM/EXEC/HALT:$work/halt.dex0:555:binary" \
    "/README:$work/readme:444:binary" \
    > "$work/out.s"

"$MKINITFS0" \
    "/SYSTEM/INIT:$work/init.s:555:asm" \
    > "$work/asm.s"

# Header: magic, version, 5 entries, 8 ent_words, 8 str_words, 6 data_words
check "hdr[0]"  "$(word_at "$work/out.s" 0)" "000051646060"
check "hdr[1]"  "$(word_at "$work/out.s" 1)" "000000000001"
check "hdr[2]"  "$(word_at "$work/out.s" 2)" "000000000005"
check "hdr[3]"  "$(word_at "$work/out.s" 3)" "000000000010"
check "hdr[4]"  "$(word_at "$work/out.s" 4)" "000000000010"
check "hdr[5]"  "$(word_at "$work/out.s" 5)" "000000000006"
check "hdr[6]"  "$(word_at "$work/out.s" 6)" "000000000000"
check "hdr[7]"  "$(word_at "$work/out.s" 7)" "000000000000"

# Entry table: 8 fields per entry, entries start at word 8
# /SYSTEM  dir:  [name_off=0,  type=4, mode=555, data_off=0, data_wc=0, nonets=0, parent=0, flags=0]
check "system[0]" "$(word_at "$work/out.s"  8)" "000000000000"
check "system[1]" "$(word_at "$work/out.s"  9)" "000000000004"
check "system[2]" "$(word_at "$work/out.s" 10)" "000000000555"
check "system[3]" "$(word_at "$work/out.s" 11)" "000000000000"
check "system[4]" "$(word_at "$work/out.s" 12)" "000000000000"
check "system[5]" "$(word_at "$work/out.s" 13)" "000000000000"
check "system[6]" "$(word_at "$work/out.s" 14)" "000000000000"
check "system[7]" "$(word_at "$work/out.s" 15)" "000000000000"

# /SYSTEM/INIT reg: [name_off=7, type=1, mode=555, data_off=0, data_wc=4, nonets=16, parent=1, flags=0]
check "init[0]" "$(word_at "$work/out.s" 16)" "000000000007"
check "init[1]" "$(word_at "$work/out.s" 17)" "000000000001"
check "init[2]" "$(word_at "$work/out.s" 18)" "000000000555"
check "init[3]" "$(word_at "$work/out.s" 19)" "000000000000"
check "init[4]" "$(word_at "$work/out.s" 20)" "000000000004"
check "init[5]" "$(word_at "$work/out.s" 21)" "000000000020"
check "init[6]" "$(word_at "$work/out.s" 22)" "000000000001"
check "init[7]" "$(word_at "$work/out.s" 23)" "000000000000"

# /SYSTEM/EXEC dir: [name_off=12, type=4, mode=555, data_off=0, data_wc=0, nonets=0, parent=1, flags=0]
check "exec[0]" "$(word_at "$work/out.s" 24)" "000000000014"
check "exec[1]" "$(word_at "$work/out.s" 25)" "000000000004"
check "exec[2]" "$(word_at "$work/out.s" 26)" "000000000555"
check "exec[3]" "$(word_at "$work/out.s" 27)" "000000000000"
check "exec[4]" "$(word_at "$work/out.s" 28)" "000000000000"
check "exec[5]" "$(word_at "$work/out.s" 29)" "000000000000"
check "exec[6]" "$(word_at "$work/out.s" 30)" "000000000001"
check "exec[7]" "$(word_at "$work/out.s" 31)" "000000000000"

# /SYSTEM/EXEC/HALT reg: [name_off=17, type=1, mode=555, data_off=4, data_wc=1, nonets=2, parent=3, flags=0]
check "halt[0]" "$(word_at "$work/out.s" 32)" "000000000021"
check "halt[1]" "$(word_at "$work/out.s" 33)" "000000000001"
check "halt[2]" "$(word_at "$work/out.s" 34)" "000000000555"
check "halt[3]" "$(word_at "$work/out.s" 35)" "000000000004"
check "halt[4]" "$(word_at "$work/out.s" 36)" "000000000001"
check "halt[5]" "$(word_at "$work/out.s" 37)" "000000000002"
check "halt[6]" "$(word_at "$work/out.s" 38)" "000000000003"
check "halt[7]" "$(word_at "$work/out.s" 39)" "000000000000"

# /README reg: [name_off=22, type=1, mode=444, data_off=5, data_wc=1, nonets=3, parent=0, flags=0]
check "readme[0]" "$(word_at "$work/out.s" 40)" "000000000026"
check "readme[1]" "$(word_at "$work/out.s" 41)" "000000000001"
check "readme[2]" "$(word_at "$work/out.s" 42)" "000000000444"
check "readme[3]" "$(word_at "$work/out.s" 43)" "000000000005"
check "readme[4]" "$(word_at "$work/out.s" 44)" "000000000001"
check "readme[5]" "$(word_at "$work/out.s" 45)" "000000000003"
check "readme[6]" "$(word_at "$work/out.s" 46)" "000000000000"
check "readme[7]" "$(word_at "$work/out.s" 47)" "000000000000"

# String section (words 48-55): packed ASCII names with NUL terminators
# SYSTEM\0 INIT\0 EXEC\0 HALT\0 README\0  (29 nonets, padded to 32 = 8 words)
check "str[0]" "$(word_at "$work/out.s" 48)" "$(pack4 83 89 83 84)"   # SYST
check "str[1]" "$(word_at "$work/out.s" 49)" "$(pack4 69 77  0 73)"   # EM\0I
check "str[2]" "$(word_at "$work/out.s" 50)" "$(pack4 78 73 84  0)"   # NIT\0
check "str[3]" "$(word_at "$work/out.s" 51)" "$(pack4 69 88 69 67)"   # EXEC
check "str[4]" "$(word_at "$work/out.s" 52)" "$(pack4  0 72 65 76)"   # \0HAL
check "str[5]" "$(word_at "$work/out.s" 53)" "$(pack4 84  0 82 69)"   # T\0RE
check "str[6]" "$(word_at "$work/out.s" 54)" "$(pack4 65 68 77 69)"   # ADME
check "str[7]" "$(word_at "$work/out.s" 55)" "$(pack4  0  0  0  0)"   # \0000

# Data section (words 56-61): INIT(4) + HALT(1) + README(1)
check "data[0]" "$(word_at "$work/out.s" 56)" "000044457060"   # DEX0_MAGIC
check "data[1]" "$(word_at "$work/out.s" 57)" "000000000001"
check "data[2]" "$(word_at "$work/out.s" 58)" "000000000000"
check "data[3]" "$(word_at "$work/out.s" 59)" "000000000001"
check "data[4]" "$(word_at "$work/out.s" 60)" "$(pack4 72 10 0 0)"   # H\n
check "data[5]" "$(word_at "$work/out.s" 61)" "$(pack4 79 75 10 0)"  # OK\n

# Path comment appears in entry output
grep -F '/SYSTEM/EXEC/HALT' "$work/out.s" > /dev/null

# asm: passthrough lines present
grep -F '	movei 1,1'          "$work/asm.s" > /dev/null
grep -F '	pushj 17,mach_syscall' "$work/asm.s" > /dev/null

# asm header: version=1, nentries=2 (SYSTEM+INIT), str_words=3, data_wc=2
check "asm-hdr[1]" "$(word_at "$work/asm.s" 1)" "000000000001"
check "asm-hdr[2]" "$(word_at "$work/asm.s" 2)" "000000000002"
check "asm-hdr[4]" "$(word_at "$work/asm.s" 4)" "000000000003"
check "asm-hdr[5]" "$(word_at "$work/asm.s" 5)" "000000000002"

# --- test_rejects_duplicate_path ---

printf 'x' > "$work/payload"
spec="/SYSTEM/INIT:$work/payload:555:binary"
err=$("$MKINITFS0" "$spec" "$spec" 2>&1 || true)
case "$err" in *"duplicate image path"*) ;; *)
    printf 'FAIL duplicate path: expected "duplicate image path" in stderr\n' >&2
    exit 1
esac

# --- test_rejects_bad_component ---

err=$("$MKINITFS0" "/BAD@NAME/INIT:$work/payload:555:binary" 2>&1 || true)
case "$err" in *"not a SIXBIT V1 name"*) ;; *)
    printf 'FAIL bad component: expected "not a SIXBIT V1 name" in stderr\n' >&2
    exit 1
esac

# --- test_rejects_bad_word_file ---

printf '888\n' > "$work/bad.words"
err=$("$MKINITFS0" "/SYSTEM/INIT:$work/bad.words:555:words" 2>&1 || true)
case "$err" in *"not octal"*) ;; *)
    printf 'FAIL bad word file: expected "not octal" in stderr\n' >&2
    exit 1
esac

echo "test-mkinitfs0: ok"
