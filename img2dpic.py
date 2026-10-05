#!/usr/bin/env python3
import struct
import subprocess
import sys

TARGET_SIZE = 1024
NEAR_SQUARE_DIVISOR = 64
VECTOR_MAX_DELTA = 127
INTENSITY_MAX = 7

MODE_PARAM = 0
MODE_POINT = 1
MODE_VECTOR = 4


def usage(fp):
    fp.write(
        "usage: img2dpic [-t 340] INPUT OUTPUT\n"
        "       img2dpic -h\n"
        "\n"
        "Reads an opaque image through ImageMagick, scales it to 1024x1024,\n"
        "converts it to 1-bit, and emits a Type 340 display program as\n"
        "little-endian 64-bit containers whose low 36 bits hold one PDP-10\n"
        "word.\n"
    )


def die(msg):
    sys.stderr.write(f"img2dpic: {msg}\n")
    raise SystemExit(1)


def run_checked(argv):
    try:
        return subprocess.run(argv, check=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
    except FileNotFoundError as exc:
        die(f"{argv[0]}: {exc.strerror}")
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr.decode("utf-8", errors="replace").strip()
        if stderr:
            die(stderr)
        die(f"command failed: {' '.join(argv)}")


def bit18(bit):
    if bit < 0 or bit > 17:
        die("internal invalid Type 340 bit index")
    return 1 << (17 - bit)


def field18(start, end, value):
    if start > end or start < 0 or end > 17:
        die("internal invalid Type 340 field")
    width = end - start + 1
    mask = (1 << width) - 1
    if value & ~mask:
        die("internal Type 340 field overflow")
    return (value & mask) << (17 - end)


def ty340_param(next_mode, set_intensity=True, intensity=INTENSITY_MAX):
    inst = field18(2, 4, next_mode & 0o7)
    if set_intensity:
        inst |= bit18(14)
        inst |= field18(15, 17, intensity & 0o7)
    return inst


def ty340_point(next_mode, coord, y_axis, intensify):
    if coord < 0 or coord >= TARGET_SIZE:
        die("internal point coordinate out of range")
    inst = field18(2, 4, next_mode & 0o7) | field18(8, 17, coord)
    if y_axis:
        inst |= bit18(1)
    if intensify:
        inst |= bit18(7)
    return inst


def ty340_vector(escape, intensify, sy, dy, sx, dx):
    if dy > VECTOR_MAX_DELTA or dx > VECTOR_MAX_DELTA:
        die("internal vector delta out of range")
    inst = field18(3, 9, dy) | field18(11, 17, dx)
    if escape:
        inst |= bit18(0)
    if intensify:
        inst |= bit18(1)
    if sy:
        inst |= bit18(2)
    if sx:
        inst |= bit18(10)
    return inst


def near_square(width, height):
    delta = abs(width - height)
    limit = max(1, (max(width, height) + NEAR_SQUARE_DIVISOR - 1) //
                NEAR_SQUARE_DIVISOR)
    return delta <= limit


def identify_image(path):
    fmt = "%n|%w|%h|%[opaque]"
    proc = run_checked(["magick", "identify", "-ping", "-format", fmt, path])
    text = proc.stdout.decode("utf-8", errors="strict")
    parts = text.split("|")
    if len(parts) != 4:
        die(f"{path}: unexpected identify output")
    frames = int(parts[0])
    width = int(parts[1])
    height = int(parts[2])
    opaque = parts[3].strip().lower()
    if frames != 1:
        die(f"{path}: only single-image inputs are supported")
    if width <= TARGET_SIZE or height <= TARGET_SIZE:
        die(f"{path}: both dimensions must be greater than {TARGET_SIZE} pixels (got {width}x{height})")
    if not near_square(width, height):
        die(f"{path}: image must be square or near-square (got {width}x{height}; max delta 1/{NEAR_SQUARE_DIVISOR} of the larger side)")
    if opaque != "true":
        die(f"{path}: transparency is not allowed for Type 340 input")
    return width, height


def convert_to_pbm(path):
    proc = run_checked([
        "magick", path,
        "-resize", f"{TARGET_SIZE}x{TARGET_SIZE}!",
        "-colorspace", "Gray",
        "-threshold", "50%",
        "-type", "bilevel",
        "pbm:-",
    ])
    return proc.stdout


def pbm_token(data, pos):
    n = len(data)
    while pos < n:
        c = data[pos]
        if c == 35:
            while pos < n and data[pos] not in (10, 13):
                pos += 1
            continue
        if c in b" \t\r\n\f\v":
            pos += 1
            continue
        break
    if pos >= n:
        die("unexpected end of PBM header")
    start = pos
    while pos < n and data[pos] not in b" \t\r\n\f\v":
        pos += 1
    return data[start:pos], pos


def parse_pbm(data):
    pos = 0
    magic, pos = pbm_token(data, pos)
    if magic != b"P4":
        die("ImageMagick did not produce a binary PBM")
    width_tok, pos = pbm_token(data, pos)
    height_tok, pos = pbm_token(data, pos)
    width = int(width_tok)
    height = int(height_tok)
    if width != TARGET_SIZE or height != TARGET_SIZE:
        die(f"unexpected PBM size {width}x{height}")
    if pos >= len(data) or data[pos] not in b" \t\r\n\f\v":
        die("malformed PBM header terminator")
    if data[pos] == 13 and pos + 1 < len(data) and data[pos + 1] == 10:
        pos += 2
    else:
        pos += 1
    return width, height, data[pos:]


class WordWriter:
    def __init__(self, path):
        self.fp = open(path, "wb")
        self.left = 0
        self.have_left = False
        self.words = 0
        self.halfwords = 0

    def put_word(self, word):
        if word >> 36:
            die("internal output word exceeds 36 bits")
        self.fp.write(struct.pack("<Q", word))
        self.words += 1

    def put_half(self, half):
        if half >> 18:
            die("internal output halfword exceeds 18 bits")
        if not self.have_left:
            self.left = half
            self.have_left = True
        else:
            self.put_word((self.left << 18) | half)
            self.left = 0
            self.have_left = False
        self.halfwords += 1

    def finish(self):
        if self.have_left:
            self.put_word(self.left << 18)
            self.left = 0
            self.have_left = False
        if self.words == 0:
            self.put_word(0)
        self.fp.close()


def iter_runs(pbm_payload, width, height):
    stride = (width + 7) // 8
    expect = stride * height
    if len(pbm_payload) != expect:
        die(f"PBM payload size mismatch: expected {expect} bytes, got {len(pbm_payload)}")
    for y in range(height):
        row = pbm_payload[y * stride:(y + 1) * stride]
        x = 0
        while x < width:
            byte = row[x >> 3]
            mask = 0x80 >> (x & 7)
            if (byte & mask) == 0:
                x += 1
                continue
            start = x
            x += 1
            while x < width:
                byte = row[x >> 3]
                mask = 0x80 >> (x & 7)
                if (byte & mask) == 0:
                    break
                x += 1
            yield start, y, x - start


def emit_run(writer, x, y, length, stats):
    stats["runs"] += 1
    stats["lit_pixels"] += length
    writer.put_half(ty340_param(MODE_POINT, True, INTENSITY_MAX))
    writer.put_half(ty340_point(MODE_POINT, x, False, False))
    if length == 1:
        writer.put_half(ty340_point(MODE_PARAM, y, True, True))
        return
    writer.put_half(ty340_point(MODE_VECTOR, y, True, True))
    remaining = length - 1
    while remaining:
        delta = min(remaining, VECTOR_MAX_DELTA)
        remaining -= delta
        writer.put_half(ty340_vector(remaining == 0, True, False, 0, False,
                                     delta))
        stats["vector_segments"] += 1


def convert_type340(inpath, outpath):
    identify_image(inpath)
    data = convert_to_pbm(inpath)
    width, height, payload = parse_pbm(data)
    writer = WordWriter(outpath)
    stats = {"lit_pixels": 0, "runs": 0, "vector_segments": 0}
    for x, y, length in iter_runs(payload, width, height):
        emit_run(writer, x, y, length, stats)
    writer.finish()
    sys.stderr.write(
        f"img2dpic: TYPE340 {inpath} -> {outpath}\n"
        f"img2dpic: converted to 1-bit and scaled image {TARGET_SIZE}x{TARGET_SIZE}, "
        f"lit pixels={stats['lit_pixels']}, runs={stats['runs']}, "
        f"vector segments={stats['vector_segments']}, "
        f"halfwords={writer.halfwords}, words={writer.words}\n"
    )


def main(argv):
    out_type = "340"
    i = 1
    while i < len(argv) and argv[i].startswith("-"):
        if argv[i] in ("-h", "--help"):
            usage(sys.stdout)
            return 0
        if argv[i] in ("-t", "--type"):
            if i + 1 >= len(argv):
                die("missing argument after -t/--type")
            out_type = argv[i + 1]
            i += 2
            continue
        die(f"unknown option: {argv[i]}")
    if len(argv) - i != 2:
        usage(sys.stderr)
        return 1
    if out_type not in ("340", "type340"):
        die("only Type 340 output is implemented")
    convert_type340(argv[i], argv[i + 1])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
