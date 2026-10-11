#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Seed corpora for the libFuzzer targets (08 section 6.8, 05 section 9.3; AC-M0-06).

  <out>/fuzz_ld_header/<family>.ld   every synthetic family's input.ld (the first --max-bytes of larger ones)
  <out>/fuzz_ldx/<family>.ldx        every synthetic family's input.ldx, plus minimal hand-written documents
With --corpus fixtures/private/s1/corpus.json (local runs only, never committed): the heads of the user's real
.ld files and their .ldx companions as well.

Standard library only:
  python -I tools/fuzz_seeds.py --inputs <dir of family dirs, from esses-gen-fixtures --inputs-only> --out <dir>
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import sys

MINIMAL_LDX = [
    b'<?xml version="1.0"?>\n<LDXFile Version="1.6"/>\n',
    b'<?xml version="1.0"?>\n<LDXFile Locale="C" DefaultLocale="C" Version="1.6">\n <Layers>\n  <Layer>\n'
    b'   <MarkerBlock>\n    <MarkerGroup Name="Beacons" Index="3">\n'
    b'     <Marker Version="100" ClassName="BCN" Name="Manual.1" Flags="77" Time="5000000.00"/>\n'
    b'    </MarkerGroup>\n   </MarkerBlock>\n  </Layer>\n  <Details>\n'
    b'   <String Id="Total Laps" Value="1"/>\n  </Details>\n </Layers>\n</LDXFile>\n',
]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--inputs", required=True, help="directory of generated family directories")
    ap.add_argument("--out", required=True, help="seed root directory")
    ap.add_argument("--corpus", help="fixtures/private/s1/corpus.json: add the user's files (local only)")
    ap.add_argument("--max-bytes", type=int, default=256 * 1024, help="cap per .ld seed (default 256 KiB)")
    a = ap.parse_args(argv)
    inputs = pathlib.Path(a.inputs)
    out = pathlib.Path(a.out)
    ld_dir = out / "fuzz_ld_header"
    ldx_dir = out / "fuzz_ldx"
    for d in (ld_dir, ldx_dir):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
    n_ld = n_ldx = 0
    for fam in sorted(p for p in inputs.iterdir() if p.is_dir()):
        ld = fam / "input.ld"
        if ld.exists():
            with open(ld, "rb") as fh:
                (ld_dir / f"{fam.name}.ld").write_bytes(fh.read(a.max_bytes))
            n_ld += 1
        ldx = fam / "input.ldx"
        if ldx.exists():
            shutil.copyfile(ldx, ldx_dir / f"{fam.name}.ldx")
            n_ldx += 1
    for k, text in enumerate(MINIMAL_LDX):
        (ldx_dir / f"minimal-{k}.ldx").write_bytes(text)
        n_ldx += 1
    if a.corpus and pathlib.Path(a.corpus).exists():
        for k, e in enumerate(json.loads(pathlib.Path(a.corpus).read_text(encoding="utf-8"))):
            p = pathlib.Path(e.get("path", ""))
            if not p.exists():
                continue
            if e.get("ext") == ".ld":
                with open(p, "rb") as fh:
                    (ld_dir / f"private-{k:03d}.ld").write_bytes(fh.read(a.max_bytes))
                n_ld += 1
            elif e.get("ext") == ".ldx":
                shutil.copyfile(p, ldx_dir / f"private-{k:03d}.ldx")
                n_ldx += 1
    print(f"fuzz_seeds: {n_ld} .ld and {n_ldx} .ldx seeds in {out}")
    return 0 if n_ld > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
