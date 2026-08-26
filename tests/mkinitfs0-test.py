#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
MKINITFS0 = ROOT / "mkinitfs0"


def run(args, **kwargs):
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        **kwargs,
    )

def asm_words(text):
    words = []
    for line in text.splitlines():
        match = re.match(r"\s*\.word\s+0([0-7]+)", line)
        if match:
            words.append(int(match.group(1), 8))
    return words


def pack_nonets(nonets):
    words = []
    for i in range(0, len(nonets), 4):
        chunk = list(nonets[i:i + 4])
        while len(chunk) < 4:
            chunk.append(0)
        word = 0
        for nonet in chunk:
            word = (word << 9) | nonet
        words.append(word)
    return words


def test_generates_hierarchical_image_with_halt():
    with tempfile.TemporaryDirectory(prefix="mkinitfs0.") as td:
        tmp = pathlib.Path(td)
        init_file = tmp / "init.dex0"
        halt_file = tmp / "halt.dex0"
        readme_file = tmp / "README"
        asm_file = tmp / "init.s"
        init_file.write_text(
            "0044457060 ; DEX0_MAGIC\n"
            "0000000001\n"
            "0000000000\n"
            "0000000001\n",
            encoding="ascii",
        )
        halt_file.write_bytes(b"H\n")
        readme_file.write_text("OK\n", encoding="ascii")
        asm_file.write_text(
            ".text\n"
            "init_start:\n"
            "\tmovei 1,1\n"
            "\tpushj 17,mach_syscall\n",
            encoding="ascii",
        )

        proc = run([
            sys.executable, str(MKINITFS0),
            f"/SYSTEM/INIT:{init_file}:555:words",
            f"/SYSTEM/EXEC/HALT:{halt_file}:555:binary",
            f"/README:{readme_file}:444:binary",
        ])
        assert proc.returncode == 0, proc.stderr

        asm_proc = run([
            sys.executable, str(MKINITFS0),
            f"/SYSTEM/INIT:{asm_file}:555:asm",
        ])
        assert asm_proc.returncode == 0, asm_proc.stderr

    words = asm_words(proc.stdout)
    assert words[0:8] == [
        0o51646060,
        1,
        5,
        8,
        8,
        6,
        0,
        0,
    ]

    system_ent = words[8:16]
    init_ent = words[16:24]
    exec_ent = words[24:32]
    halt_ent = words[32:40]
    readme_ent = words[40:48]
    assert system_ent == [0, 4, 0o555, 0, 0, 0, 0, 0]
    assert init_ent == [7, 1, 0o555, 0, 4, 16, 1, 0]
    assert exec_ent == [12, 4, 0o555, 0, 0, 0, 1, 0]
    assert halt_ent == [17, 1, 0o555, 4, 1, 2, 3, 0]
    assert readme_ent == [22, 1, 0o444, 5, 1, 3, 0, 0]

    names = (
        b"SYSTEM\0" + b"INIT\0" + b"EXEC\0" + b"HALT\0" + b"README\0"
    )
    assert words[48:56] == pack_nonets(names)
    assert words[56:60] == [0o44457060, 1, 0, 1]
    assert words[60] == pack_nonets(b"H\n")[0]
    assert words[61] == pack_nonets(b"OK\n")[0]

    assert "/SYSTEM/EXEC/HALT" in proc.stdout
    assert "\tmovei 1,1\n" in asm_proc.stdout
    assert "\tpushj 17,mach_syscall\n" in asm_proc.stdout
    asm_image_words = asm_words(asm_proc.stdout)
    assert asm_image_words[1] == 1
    assert asm_image_words[2] == 2
    assert asm_image_words[4] == 3
    assert asm_image_words[5] == 2


def test_rejects_duplicate_path():
    with tempfile.TemporaryDirectory(prefix="mkinitfs0.") as td:
        tmp = pathlib.Path(td)
        payload = tmp / "payload"
        payload.write_text("x", encoding="ascii")
        spec = f"/SYSTEM/INIT:{payload}:555:binary"
        proc = run([sys.executable, str(MKINITFS0), spec, spec])
        assert proc.returncode != 0
        assert "duplicate image path" in proc.stderr


def test_rejects_bad_component():
    with tempfile.TemporaryDirectory(prefix="mkinitfs0.") as td:
        tmp = pathlib.Path(td)
        payload = tmp / "payload"
        payload.write_text("x", encoding="ascii")
        proc = run([
            sys.executable, str(MKINITFS0),
            f"/BAD@NAME/INIT:{payload}:555:binary",
        ])
        assert proc.returncode != 0
        assert "not a SIXBIT V1 name" in proc.stderr


def test_rejects_bad_word_file():
    with tempfile.TemporaryDirectory(prefix="mkinitfs0.") as td:
        tmp = pathlib.Path(td)
        payload = tmp / "payload.words"
        payload.write_text("888\n", encoding="ascii")
        proc = run([
            sys.executable, str(MKINITFS0),
            f"/SYSTEM/INIT:{payload}:555:words",
        ])
        assert proc.returncode != 0
        assert "not octal" in proc.stderr


def main():
    test_generates_hierarchical_image_with_halt()
    test_rejects_duplicate_path()
    test_rejects_bad_component()
    test_rejects_bad_word_file()
    print("test-mkinitfs0: ok")


if __name__ == "__main__":
    main()
