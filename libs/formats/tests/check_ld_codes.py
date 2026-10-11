#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""One vocabulary for the .ld reader and the independent oracle (05 section 1.3 rule 9, D-041).

Checks libs/formats/src/ld/ld_codes.{hpp,cpp} against `tools/ld_ref_decode.py --list-codes`:
  1. every code constant of ld_codes.hpp has a row in the table of ld_codes.cpp, and every row a constant;
  2. a row marked shared with the oracle names a code the oracle emits, and a row not marked shared names a
     code the oracle does not have (so equal names always mean the same thing);
  3. no source file under libs/formats/src spells an "ld." / "ldx." code outside ld_codes.hpp.

Standard library only. `python -I check_ld_codes.py --root <repo>`; exit 0 clean, 1 findings, 2 usage error.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

CONST_RE = re.compile(r'inline constexpr std::string_view (k\w+) = "((?:ld|ldx)\.[a-z0-9_]+)";')
ROW_RE = re.compile(r"CodeInfo\{\s*code::(k\w+),\s*L::(\w+),\s*(true|false)", re.S)
LITERAL_RE = re.compile(r'"((?:ld|ldx)\.[a-z][a-z0-9_]*)"')


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", required=True, help="repository root")
    a = ap.parse_args(argv)
    root = pathlib.Path(a.root)
    hpp = root / "libs/formats/src/ld/ld_codes.hpp"
    cpp = root / "libs/formats/src/ld/ld_codes.cpp"
    oracle = root / "tools/ld_ref_decode.py"
    if not (hpp.exists() and cpp.exists() and oracle.exists()):
        print("check_ld_codes: ld_codes.hpp/.cpp or tools/ld_ref_decode.py not found")
        return 2
    consts = dict(CONST_RE.findall(hpp.read_text(encoding="utf-8")))
    rows = {name: (level, shared == "true") for name, level, shared in ROW_RE.findall(cpp.read_text(encoding="utf-8"))}
    listing = subprocess.run([sys.executable, "-I", str(oracle), "--list-codes"], capture_output=True, text=True)
    if listing.returncode != 0:
        print(listing.stdout + listing.stderr)
        return 2
    oracle_codes = {line.split("\t", 1)[0] for line in listing.stdout.splitlines() if line.strip()}
    findings: list[str] = []
    for name in sorted(set(consts) - set(rows)):
        findings.append(f"{consts[name]} ({name}) has no row in the code table")
    for name in sorted(set(rows) - set(consts)):
        findings.append(f"table row {name} names no constant")
    for name, (_level, shared) in sorted(rows.items()):
        code = consts.get(name)
        if code is None:
            continue
        if shared and code not in oracle_codes:
            findings.append(f"{code} is marked shared but tools/ld_ref_decode.py has no such code")
        if not shared and code in oracle_codes:
            findings.append(f"{code} exists in tools/ld_ref_decode.py: mark it shared (same meaning) or rename it")
    for src in sorted((root / "libs/formats/src").rglob("*")):
        if src.suffix not in {".cpp", ".hpp", ".h"} or src.name == "ld_codes.hpp":
            continue
        for lit in LITERAL_RE.findall(src.read_text(encoding="utf-8")):
            findings.append(f"{src.relative_to(root).as_posix()}: code literal \"{lit}\"; use the ld_codes.hpp constant")
    for f in findings:
        print(f)
    shared = sum(1 for _l, s in rows.values() if s)
    print(f"check_ld_codes: {len(rows)} codes ({shared} shared with the oracle), {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
