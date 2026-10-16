#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Seed corpora for the libFuzzer targets (08 section 6.8, 05 section 9.3; AC-M0-06).

  <out>/fuzz_ld_header/<family>.ld   every synthetic family's input.ld (the first --max-bytes of larger ones)
  <out>/fuzz_ldx/<family>.ldx        every synthetic family's input.ldx, plus minimal hand-written documents
                                     and regression reproducers (minimal-<k>.ldx)
  <out>/fuzz_csv_sniff, fuzz_text_profile/<format>-<name>.<ext>
                                     the heads of the committed text fixtures (fixtures/*.csv/*/input.*); the
                                     profile target also gets the shipped text profiles and dictionaries and the
                                     schema examples of importer-profile-1, dictionary-1 and unitmap-1
  <out>/fuzz_maths_parser/<k>.txt    hand-written maths expressions (03 section 2: every token kind, word
                                     operators, units, comments, calls, and a few syntax errors)
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
    # Regression seed (fuzz_ldx, first CI run): elements kept verbatim that contain the writer's reserved
    # placeholder instruction in a PI, a comment, CDATA and attribute values (libs/formats/tests/ldx_tests.cpp).
    b'<?xml version="1.0"?>\n<LDXFile Locale="C" DefaultLocale="C" Version="1.6">\n <Layers>\n  <Layer>\n'
    b'   <MarkerBlock>\n    <MarkerGroup Name="Beacons" Index="3">\n'
    b'     <Note><!-- <?esses-opaque 1?> --><?esses-opaque 2?><![CDATA[<?esses-opaque 3?>]]></Note>\n'
    b'     <Marker Version="100" ClassName="EOL" Name="late" Flags="77" Time="soon"><!-- x --></Marker>\n'
    b'     <Marker Version="100" ClassName="BCN" Name="Manual.1" Flags="77" Time="5000000.00"/>\n'
    b'     <Odd a="]]>"/>\n    </MarkerGroup>\n   </MarkerBlock>\n  </Layer>\n  <Details>\n'
    b'   <String Id="Note" Value="<?esses-opaque 0?>"/>\n  </Details>\n'
    b"  <Extra b='<?esses-opaque 0?>'/>\n </Layers>\n</LDXFile>\n",
]

# Expressions in the maths language (our own; i2-style syntax per 03 section 2).
MATHS_SEEDS = [
    "'Speed'[km/h] * 2",
    "-2^2 + 2^-3^2 - (-2)^2",
    "choose('Speed'[m/s] > 10 AND NOT 'Gear' LT 2, 'Speed'[km/h] / 3.6, invalid())",
    "smooth('Speed'[m/s], 0.2) + derivative('Speed'[m/s], 5) // comment",
    "/* block */ filter_cheby_bp('Speed'[m/s], 1, 5, -4, 0.5)",
    "stat_mean('Speed'[km/h], 'Gear' > 1, range_change(\"Outings:Laps\"))",
    "edge_delay('Speed'[m/s] > 1.5e1, 1.0, rise) | bit_decode(int('Gear'), 1, 2) & 255",
    "'Driver' == \"Joe\" || 'Gain' * .5 >= 1. && ~'Gear' != 0",
    "integrate('Speed'[m/s], 1, range_change(\"Outings:Laps\")) / 1000",
    "max(1 2) + Throttle * (3 + ",
    "'X' km/h + [m] + ((('a')))",
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
    maths_dir = out / "fuzz_maths_parser"
    text_dirs = [out / "fuzz_csv_sniff", out / "fuzz_text_profile"]
    for d in (ld_dir, ldx_dir, maths_dir, *text_dirs):
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
    for k, text in enumerate(MATHS_SEEDS):
        (maths_dir / f"{k:02d}.txt").write_bytes(text.encode("utf-8"))
    # Text targets (M2): the heads of the committed, self-authored text fixtures and the shipped profiles,
    # dictionaries and schema examples of the three text documents.
    repo = pathlib.Path(__file__).resolve().parent.parent
    n_text = 0
    for fixture in sorted((repo / "fixtures").glob("*.csv/*/input.*")):
        head = fixture.read_bytes()[: a.max_bytes]
        for d in text_dirs:
            (d / f"{fixture.parent.parent.name}-{fixture.parent.name}{fixture.suffix}").write_bytes(head)
        n_text += 1
    documents = sorted((repo / "data" / "profiles-text").glob("*.json")) + sorted(
        (repo / "data" / "dictionaries").glob("*.json"))
    for name in ("importer-profile-1", "dictionary-1", "unitmap-1"):
        documents += sorted((repo / "schemas" / "examples" / name).glob("*/*.json"))
    for k, doc in enumerate(documents):
        (text_dirs[1] / f"doc-{k:02d}.json").write_bytes(doc.read_bytes())
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
    print(f"fuzz_seeds: {n_ld} .ld, {n_ldx} .ldx and {n_text} text seeds in {out}")
    return 0 if n_ld > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
