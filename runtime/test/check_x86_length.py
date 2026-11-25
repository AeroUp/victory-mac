"""Compare src/x86_length.h with llvm-mc's x86-64 disassembler on generated instructions.

Usage:
    python -I runtime/test/check_x86_length.py <x86_length_dump> [--llvm-mc PATH] [--cases N] [--seed S] [--verbose]

Each case is 15 bytes shaped like an instruction: legacy prefixes and REX, then a one-byte opcode, 0F, 0F 38, 0F 3A,
VEX (C4/C5), EVEX (62) or XOP (8F), then random ModRM, SIB, displacement and immediate bytes. llvm-mc decodes the
case's first instruction and x86_length_dump prints the header's length for it. Every case llvm-mc decodes must get
the same length. The cases where the two disagree on whether the bytes are an instruction at all are counted and
printed by kind, and allowed: they are encodings llvm-mc does not implement (3DNow!, some AMD or system opcodes) or
ones the header accepts by shape although the CPU raises #UD. A sweep over a function's code only meets them in data.

Exit 0 when every length matches, 1 on a mismatch, 77 (skipped) when llvm-mc is not found. Standard library only.
"""
from __future__ import annotations

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
from collections import Counter

LEGACY = [0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x3E, 0x26, 0x36, 0x64, 0x65, 0xF0]
# Mnemonics llvm-objdump prints alone for a prefix it does not fold into the instruction after it.
PREFIX_WORDS = {"lock", "rep", "repe", "repz", "repne", "repnz", "xacquire", "xrelease", "notrack", "data16",
                "data32", "addr32", "addr16", "cs", "ds", "es", "ss", "fs", "gs", "rex", "rex64", "bnd"} | {
                    f"rex.{s}" for s in ("w", "r", "x", "b", "wr", "wx", "wb", "rx", "rb", "xb", "wrx", "wrb", "wxb",
                                         "rxb", "wrxb", "W", "R", "X", "B", "WR", "WX", "WB", "RX", "RB", "XB",
                                         "WRX", "WRB", "WXB", "RXB", "WRXB")}


def find_llvm_mc(given: str | None) -> str | None:
    if given:
        return given if os.path.exists(given) else None
    if os.environ.get("LLVM_MC"):
        return os.environ["LLVM_MC"]
    found = shutil.which("llvm-mc")
    if found:
        return found
    for root in ("/opt/homebrew/opt", "/usr/local/opt"):
        for keg in ("llvm",) + tuple(f"llvm@{v}" for v in range(30, 14, -1)):
            path = os.path.join(root, keg, "bin", "llvm-mc")
            if os.path.exists(path):
                return path
    return None


def make_case(rng: random.Random) -> tuple[str, list[int]]:
    out: list[int] = []
    for _ in range(rng.choice((0, 0, 0, 1, 1, 2, 3))):
        out.append(rng.choice(LEGACY))
    if rng.random() < 0.5:
        out.append(0x40 | rng.randrange(16))
    kind = rng.choices(["one", "0f", "0f38", "0f3a", "vex2", "vex3", "evex", "xop"],
                       weights=[30, 30, 8, 8, 8, 8, 4, 4])[0]
    if kind == "one":
        out.append(rng.randrange(256))
    elif kind == "0f":
        out += [0x0F, rng.randrange(256)]
    elif kind == "0f38":
        out += [0x0F, 0x38, rng.randrange(256)]
    elif kind == "0f3a":
        out += [0x0F, 0x3A, rng.randrange(256)]
    elif kind == "vex2":
        out += [0xC5, rng.randrange(256), rng.randrange(256)]
    elif kind == "vex3":
        out += [0xC4, (rng.randrange(8) << 5) | rng.choice((1, 2, 3, rng.randrange(32))), rng.randrange(256),
                rng.randrange(256)]
    elif kind == "evex":
        out += [0x62, (rng.randrange(16) << 4) | rng.choice((1, 2, 3, 5, 6, rng.randrange(8))),
                rng.randrange(256) | 0x04, rng.randrange(256), rng.randrange(256)]
    else:
        out += [0x8F, (rng.randrange(8) << 5) | rng.choice((8, 9, 10, rng.randrange(32))), rng.randrange(256),
                rng.randrange(256)]
    while len(out) < 15:
        out.append(rng.randrange(256))
    return kind, out[:15]


def llvm_lengths(llvm_mc: str, cases: list[list[int]]) -> tuple[list[int | None], list[bool]]:
    """The length of each case's first instruction as llvm-objdump decodes it, None where it finds none.

    `llvm-mc --disassemble --show-encoding` prints the instruction encoded again, without the redundant prefixes it
    read, so the bytes it consumed are taken from llvm-objdump instead: each case is its own section of an object
    llvm-mc assembles from `.byte` lines, so each is decoded from its first byte, and the first instruction's length
    is the offset of the second.
    """
    objdump = os.path.join(os.path.dirname(llvm_mc), "llvm-objdump")
    if not os.path.exists(objdump):
        objdump = shutil.which("llvm-objdump") or objdump
    with tempfile.TemporaryDirectory() as tmp:
        source, obj = os.path.join(tmp, "cases.s"), os.path.join(tmp, "cases.o")
        with open(source, "w", encoding="utf-8", newline="\n") as f:
            for i, case in enumerate(cases):
                f.write(f'.section .text.c{i},"ax",@progbits\n.byte ' + ",".join(str(b) for b in case) + "\n")
        subprocess.run([llvm_mc, "-triple=x86_64-unknown-linux-gnu", "-filetype=obj", source, "-o", obj],
                       check=True)
        run = subprocess.run([objdump, "-d", "--no-show-raw-insn", obj], capture_output=True, text=True, check=True)
    # The first instructions of each section: (offset, text). llvm-objdump prints a prefix it does not fold into the
    # instruction (LOCK, REP, REPNE, XACQUIRE, a segment override, ...) as a line of its own, so the first instruction
    # runs from the section's start to the end of the first line that is not only prefixes.
    found: list[list[tuple[int, str]]] = [[] for _ in cases]
    section = -1
    for line in run.stdout.splitlines():
        if line.startswith("Disassembly of section .text.c"):
            section = int(line.rsplit("c", 1)[1].rstrip(":"))
            continue
        if section < 0 or len(found[section]) >= 16 or ":" not in line:
            continue
        head, _, rest = line.partition(":")
        try:
            offset = int(head.strip(), 16)
        except ValueError:
            continue
        found[section].append((offset, rest.strip()))
    lengths: list[int | None] = []
    split: list[bool] = []  # a prefix was printed on a line of its own
    for entries in found:
        k = 0
        while k < len(entries) and entries[k][1] and all(w in PREFIX_WORDS for w in entries[k][1].split()):
            k += 1
        split.append(k > 0)
        if k >= len(entries) or "(bad)" in entries[k][1] or "<unknown>" in entries[k][1]:
            lengths.append(None)
            continue
        end = entries[k + 1][0] if k + 1 < len(entries) else 15
        lengths.append(end - entries[0][0] if end <= 15 else None)
    return lengths, split


def near_branch_with_66(case: list[int]) -> bool:
    """A near CALL, JMP or Jcc rel32 after a 66 prefix: Intel keeps rel32 (the header), AMD and LLVM take rel16."""
    i, p66 = 0, False
    while i < len(case) and (case[i] in LEGACY or (case[i] & 0xF0) == 0x40):
        p66 = p66 or case[i] == 0x66
        i += 1
    if not p66 or i >= len(case):
        return False
    return case[i] in (0xE8, 0xE9) or (case[i] == 0x0F and i + 1 < len(case) and 0x80 <= case[i + 1] <= 0x8F)


def sse4a_extrq_form(case: list[int]) -> bool:
    """66 0F 78 with a register operand. The APM defines EXTRQ's immediate form as /0 only (the header and
    src/cpu_emulation.h read it so); llvm-mc decodes other forms its own way. A Zen 2 raises #UD on them."""
    i, p66 = 0, False
    while i < len(case) and (case[i] in LEGACY or (case[i] & 0xF0) == 0x40):
        p66 = p66 or case[i] == 0x66
        i += 1
    return p66 and case[i:i + 2] == [0x0F, 0x78] and i + 2 < len(case) and case[i + 2] >> 6 == 3


def header_lengths(dump: str, cases: list[list[int]]) -> list[int]:
    text = "".join(" ".join(f"{b:02x}" for b in case) + "\n" for case in cases)
    run = subprocess.run([dump], input=text, capture_output=True, text=True, check=True)
    return [int(x) for x in run.stdout.split()]


def main() -> int:
    sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", nargs="+", help="x86_length_dump, with a runner before it if it needs one (wine)")
    parser.add_argument("--llvm-mc")
    parser.add_argument("--cases", type=int, default=200_000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    llvm_mc = find_llvm_mc(args.llvm_mc)
    if not llvm_mc:
        print("SKIP: llvm-mc not found (set LLVM_MC)")
        return 77
    rng = random.Random(args.seed)
    kinds, cases = zip(*(make_case(rng) for _ in range(args.cases)))
    ours = header_lengths(args.dump[-1], list(cases)) if len(args.dump) == 1 else None
    if ours is None:
        text = "".join(" ".join(f"{b:02x}" for b in c) + "\n" for c in cases)
        run = subprocess.run(args.dump, input=text, capture_output=True, text=True, check=True)
        ours = [int(x) for x in run.stdout.split()]
    theirs, split = llvm_lengths(llvm_mc, list(cases))
    if len(ours) != len(cases):
        print(f"FAIL: x86_length_dump printed {len(ours)} lengths for {len(cases)} cases")
        return 1
    matched = 0
    mismatches: list[str] = []
    only_ours: Counter[str] = Counter()
    only_theirs: Counter[str] = Counter()
    branches_66 = split_prefix = extrq = 0
    for kind, case, mine, llvm, was_split in zip(kinds, cases, ours, theirs, split):
        if llvm is None and mine == 0:
            continue
        if llvm is not None and mine and mine != llvm and near_branch_with_66(list(case)):
            branches_66 += 1
            continue
        if llvm is not None and mine and mine != llvm and sse4a_extrq_form(list(case)):
            extrq += 1
            continue
        if llvm is not None and mine and mine != llvm and was_split:
            # llvm-objdump printed a prefix it could not fold (LOCK on an instruction that takes none, say: #UD)
            # alone and decoded the rest without the prefixes before it, a 66 included.
            split_prefix += 1
            continue
        if llvm is None:
            only_ours[kind] += 1
            continue
        if mine == 0:
            only_theirs[kind] += 1
            if args.verbose:
                print(f"llvm-mc only ({kind}): {' '.join(f'{b:02x}' for b in case[:llvm])}")
            continue
        if mine == llvm:
            matched += 1
        else:
            mismatches.append(f"{kind}: {' '.join(f'{b:02x}' for b in case)} -> header {mine}, llvm-mc {llvm}")
    print(f"x86_length: {matched} of {len(cases)} cases match llvm-mc's length; "
          f"{sum(only_ours.values())} only the header decodes {dict(only_ours)}, "
          f"{sum(only_theirs.values())} only llvm-mc decodes {dict(only_theirs)}; "
          f"{branches_66} near branches after 66 (Intel rel32, LLVM rel16); {split_prefix} decoded by llvm-objdump "
          f"after a prefix it split off; {extrq} EXTRQ forms the APM leaves undefined; {len(mismatches)} mismatched")
    for m in mismatches[:40]:
        print("MISMATCH", m)
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
