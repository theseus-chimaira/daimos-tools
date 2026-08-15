#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/p10run-functional-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/prefix/bin" "$tmp/work"

cat > "$tmp/prefix/bin/pdp10-dec-none-gcc" <<EOF_GCC
#!/bin/sh
set -eu
if test "\${1-}" = -print-libgcc-file-name; then
    echo "$tmp/prefix/libgcc.darc"
    exit 0
fi
out=
while test \$# -gt 0; do
    if test "\$1" = -o; then
        shift
        out=\$1
    fi
    shift
done
test -n "\$out"
: > "\$out"
EOF_GCC
chmod +x "$tmp/prefix/bin/pdp10-dec-none-gcc"
: > "$tmp/prefix/libgcc.darc"

cat > "$tmp/prefix/bin/pdp10-dec-none-as" <<'EOF_AS'
#!/bin/sh
set -eu
out=
while test $# -gt 0; do
    if test "$1" = -o; then shift; out=$1; fi
    shift
done
test -n "$out"
: > "$out"
EOF_AS
chmod +x "$tmp/prefix/bin/pdp10-dec-none-as"

cat > "$tmp/prefix/bin/dlink" <<'EOF_DLINK'
#!/bin/sh
set -eu
out=
map=
while test $# -gt 0; do
    case "$1" in
        -o) shift; out=$1 ;;
        -M) shift; map=$1 ;;
    esac
    shift
done
test -n "$out"
test -n "$map"
dd if=/dev/zero of="$out" bs=8 count=1 2>/dev/null
printf 'RESULT 10\n' > "$map"
EOF_DLINK
chmod +x "$tmp/prefix/bin/dlink"

cat > "$tmp/prefix/bin/dxrconvert" <<'EOF_CONV'
#!/bin/sh
set -eu
last=
for arg do last=$arg; done
test -n "$last"
: > "$last"
EOF_CONV
chmod +x "$tmp/prefix/bin/dxrconvert"

cat > "$tmp/prefix/bin/pdp10-ka" <<'EOF_SIMH'
#!/bin/sh
set -eu
ini=$1
while IFS= read -r line; do
    case "$line" in
        "echo "*) printf '%s\n' "${line#echo }" ;;
        "examine "*) printf '000000001234\n' ;;
    esac
done < "$ini"
EOF_SIMH
chmod +x "$tmp/prefix/bin/pdp10-ka"

cat > "$tmp/probe.c" <<'EOF_C'
int result;
int main(void) { result = 01234; return 0; }
EOF_C

PDP10_PREFIX="$tmp/prefix" \
"$root/p10run" --machine ka10 --mode deposit --no-default-ini \
    --workdir "$tmp/work" --name functional \
    --expect RESULT=1234 "$tmp/probe.c"

test -f "$tmp/work/functional.dxr"
test -f "$tmp/work/functional.map"
test -f "$tmp/work/functional.simh"
test -f "$tmp/work/functional.report.txt"
grep -q '^result=PASS$' "$tmp/work/functional.report.txt"
grep -q '^RESULT[[:space:]]*001010$' "$tmp/work/functional.labels"
grep -q '__P10RUN_EXPECT__ RESULT 1010 000000001234' "$tmp/work/functional-run.ini"

echo "p10run functional pipeline contract: PASS"
