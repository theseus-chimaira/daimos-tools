#!/usr/bin/env python3
"""
mkrim.py - Generate a standard RIM10B paper-tape image.

Library API:
    make_rim10b(words_dict, start_addr) -> bytes

Import-safe: command-line behavior is only under __main__.

Standard RIM10B header used here:
    header left half = 777762 for the standard 14-word bootstrap skip count.
    It is followed by 14 zero bootstrap words.
"""

from __future__ import print_function
import os
import re
import subprocess
import sys

MASK18 = (1 << 18) - 1
MASK36 = (1 << 36) - 1
TOOL_TIMEOUT = int(os.environ.get('PDP10_TOOL_TIMEOUT', '30'))

if 'PDP10_PREFIX' not in os.environ:
    raise SystemExit('error: PDP10_PREFIX must be set')

PDP10_PREFIX = os.environ['PDP10_PREFIX']
ASSEMBLER = os.environ.get('PDP10_AS', os.path.join(PDP10_PREFIX, 'bin', 'pdp10-dec-none-as'))
MKRIM_VERSION = "mkrim-label-contract-v4_20260601a+no-fallback-v1_20260614a"


def write_word(buf, word):
    """Append one 36-bit word as six RIM bytes, bit 7 set."""
    word &= MASK36
    for shift in range(30, -6, -6):
        buf.append(((word >> shift) & 0o77) | 0o200)


def make_rim10b(words_dict, start_addr):
    """Return bytes containing a standard RIM10B tape image."""
    buf = []

    # Standard RIM10B header: 14 bootstrap words follow.
    write_word(buf, (0o777762 << 18) | 0)
    for _ in range(14):
        write_word(buf, 0)

    addrs = sorted(words_dict.keys())
    blocks = []
    block = []
    for addr in addrs:
        if block and addr != block[-1] + 1:
            blocks.append(block)
            block = []
        block.append(addr)
    if block:
        blocks.append(block)

    for block in blocks:
        count = len(block)
        origin = block[0]
        # IOWD uses an 18-bit two's-complement negative count in the left half
        # and origin-1 in the right half. AOBJ-style loading increments to zero.
        iowd = (((-count) & MASK18) << 18) | ((origin - 1) & MASK18)
        checksum = iowd
        write_word(buf, iowd)
        for addr in block:
            data = words_dict[addr] & MASK36
            checksum = (checksum + data) & MASK36
            write_word(buf, data)
        write_word(buf, checksum)

    # Terminator: JRST start_addr.
    jrst = (0o254 << 27) | (int(start_addr) & MASK18)
    write_word(buf, jrst)
    return bytes(buf)


def assemble_with_binary(start_addr, source_files):
    """Use the installed pdp10-dec-none-as to assemble sources and return words + entry."""
    cmd = [ASSEMBLER, '--start', format(int(start_addr), 'o')] + list(source_files) + ['-o', '-']
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.PIPE, universal_newlines=True, timeout=TOOL_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise RuntimeError('pdp10-dec-none-as timed out after %ss: %s' % (TOOL_TIMEOUT, ' '.join(cmd)))
    except subprocess.CalledProcessError as e:
        sys.stderr.write(e.stderr or str(e))
        raise

    words = {}
    entry = start_addr

    for line in out.splitlines():
        m = re.match(r'deposit\s+(\d+)\s+(\d+)', line)
        if m:
            addr = int(m.group(1), 8)
            val = int(m.group(2), 8)
            words[addr] = val
            continue
        m = re.search(r'go\s+(\d+)', line)
        if m:
            entry = int(m.group(1), 8)

    # Also look for explicit "entry at" comments the assembler sometimes emits
    m = re.search(r'entry at (\d+)', out)
    if m:
        entry = int(m.group(1), 8)

    return words, entry


def main(argv=None):
    argv = list(sys.argv if argv is None else argv)
    if len(argv) == 2 and argv[1] == '--version':
        print(MKRIM_VERSION)
        return 0
    if len(argv) < 3:
        print('Usage: %s start_octal file.s [file2.s ...] > out.rim' % argv[0], file=sys.stderr)
        return 1

    start_addr = int(argv[1], 8)
    source_files = argv[2:]

    # Drive the installed assembler.  Assembly failures are fatal; there is no
    # fallback assembler path because that can mask strict diagnostic failures.
    try:
        words, entry = assemble_with_binary(start_addr, source_files)
    except Exception as e:
        sys.stderr.write("mkrim: assembler failed: %s\n" % e)
        return 1

    tape = make_rim10b(words, entry)
    sys.stdout.buffer.write(tape)
    print('; RIM10B: %d words, entry=%o' % (len(words), entry), file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
