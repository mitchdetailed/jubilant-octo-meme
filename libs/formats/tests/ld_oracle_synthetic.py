#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Synthetic .ld families against the independent oracle (D-041, AC-M0-03).

1. Runs the hidden Catch2 case "[.oracle_fixtures]" of esses_formats_ld_tests, which writes every synthetic
   family with LdWriter into --work together with <family>.expect.json: (a) the generator's expectations and
   (b) what the Esses reader decodes.
2. Runs tools/ld_ref_decode.py (written from the public notes, independently of esses_formats) on each file.
3. Checks that the oracle sees exactly the generator's channel list, encodings and stored values (so a writer
   error cannot hide behind a matching reader error), and that the reader's decoded values equal the oracle's
   (integers bit-exact, floats within one F32 ulp, a mean also within half an F32 ulp of the largest
   magnitude it averages).

Standard library only. `python -I ld_oracle_synthetic.py --exe <tests.exe> --oracle tools/ld_ref_decode.py
--work <dir>`; exit status 0 when everything agrees, 1 on a difference, 2 on a usage or tool error.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import shutil
import struct
import subprocess
import sys


def f32(x: float) -> float:
    if math.isnan(x) or math.isinf(x):
        return x
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def f32_key(x: float) -> int:
    (u,) = struct.unpack("<I", struct.pack("<f", f32(x)))
    return 0x80000000 - (u & 0x7FFFFFFF) if u & 0x80000000 else 0x80000000 + u


def f32_ulps(a: float, b: float) -> int:
    if math.isnan(a) and math.isnan(b):
        return 0
    if math.isnan(a) or math.isnan(b):
        return 1 << 40
    return abs(f32_key(a) - f32_key(b))


def f32_ulp(x: float) -> float:
    """Spacing of floats at |x| (rounded to float)."""
    v = abs(f32(x))
    if math.isinf(v) or math.isnan(v):
        return math.inf
    (u,) = struct.unpack("<I", struct.pack("<f", v))
    if u >= 0x7F7FFFFF:
        return math.inf
    return struct.unpack("<f", struct.pack("<I", u + 1))[0] - v


def num(v):
    if isinstance(v, str):
        return {"nan": math.nan, "inf": math.inf, "-inf": -math.inf}.get(v)
    return v


def compare(family: str, expect: dict, oracle: dict, problems: list[str]) -> int:
    och = oracle.get("channels", [])
    ech = expect["channels"]
    if len(och) != len(ech):
        problems.append(f"{family}: oracle sees {len(och)} channels, generator wrote {len(ech)}")
        return 0
    for i, (e, o) in enumerate(zip(ech, och)):
        where = f"{family}[{i}] {e['name']!r}"
        # (a) the writer, checked by the oracle against the generator's own input
        for key in ("name", "dtype_a", "dtype", "freq", "shift", "mul", "scale", "dec"):
            if o.get(key) != e[key]:
                problems.append(f"{where}: oracle {key} {o.get(key)!r} != generator {e[key]!r}")
        if o.get("count_decoded") != e["count"]:
            problems.append(f"{where}: oracle count {o.get('count_decoded')} != generator {e['count']}")
        rs = o.get("raw_stats", {})
        if [num(x) for x in rs.get("first", [])] != e["raw_first"] or \
                [num(x) for x in rs.get("last", [])] != e["raw_last"]:
            problems.append(f"{where}: oracle stored first/last differ from the generator")
        if num(rs.get("min")) != e["raw_min"] or num(rs.get("max")) != e["raw_max"]:
            problems.append(f"{where}: oracle stored min/max {rs.get('min')}/{rs.get('max')} != "
                            f"{e['raw_min']}/{e['raw_max']}")
        # (b) the reader, checked against the oracle (afterMul, D-037; equal to beforeMul unless shift and mul)
        stats = o.get("stats", {})
        order = "afterMul" if "afterMul" in stats else "beforeMul"
        if e["shift"] != 0 and e["mul"] not in (0, 1) and order != "afterMul":
            problems.append(f"{where}: oracle has no afterMul statistics")
            continue
        s = stats.get(order, {})
        r = e["reader"]
        exact = r["dtype"] == "I32"

        def close(mine, theirs):
            theirs = num(theirs)
            if theirs is None:
                return False
            return mine == theirs if exact else f32_ulps(mine, theirs) <= 1

        for key in ("first", "last"):
            mine, theirs = r[key], s.get(key, [])
            if len(mine) != len(theirs) or not all(close(a, b) for a, b in zip(mine, theirs)):
                problems.append(f"{where}: reader {key} {mine} != oracle {theirs}")
        for key in ("min", "max"):
            if not close(r[key], s.get(key)):
                problems.append(f"{where}: reader {key} {r[key]!r} != oracle {s.get(key)!r}")
        omean, omin, omax = num(s.get("mean")), num(s.get("min")), num(s.get("max"))
        if omean is None or omin is None or omax is None:
            problems.append(f"{where}: oracle has no scaled statistics")
            continue
        tol = f32_ulp(omean)
        if r["dtype"] == "F32":
            tol += math.ldexp(max(abs(omin), abs(omax)), -24)
        if not abs(r["mean"] - omean) <= tol:
            problems.append(f"{where}: reader mean {r['mean']!r} != oracle {omean!r}")
    return len(ech)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", required=True, help="esses_formats_ld_tests executable")
    ap.add_argument("--oracle", required=True, help="tools/ld_ref_decode.py")
    ap.add_argument("--work", required=True, help="scratch directory (recreated)")
    a = ap.parse_args(argv)
    work = pathlib.Path(a.work)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    env = dict(os.environ)
    env["ESSES_LD_ORACLE_OUT"] = str(work)
    exe = str(pathlib.Path(a.exe).resolve())
    gen = subprocess.run([exe, "[.oracle_fixtures]"], env=env, capture_output=True, text=True)
    if gen.returncode != 0:
        print(gen.stdout + gen.stderr)
        print("ld_oracle_synthetic: the fixture generator failed")
        return 2
    problems: list[str] = []
    files = sorted(work.glob("*.ld"))
    if not files:
        print("ld_oracle_synthetic: no fixtures were written")
        return 2
    channels = 0
    for ld in files:
        out = ld.with_suffix(".oracle.json")
        dec = subprocess.run([sys.executable, "-I", a.oracle, str(ld), "--json", "--no-ldx", "--no-hash",
                              "--max-stat-samples", "0", "-o", str(out)], capture_output=True, text=True)
        if dec.returncode != 0:
            print(dec.stdout + dec.stderr)
            print(f"ld_oracle_synthetic: the oracle failed on {ld.name}")
            return 2
        expect = json.loads(ld.with_suffix(".expect.json").read_text(encoding="utf-8"))
        oracle = json.loads(out.read_text(encoding="utf-8"))
        channels += compare(ld.stem, expect, oracle, problems)
    for p in problems[:60]:
        print(p)
    print(f"ld_oracle_synthetic: {len(files)} families, {channels} channels, {len(problems)} difference(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
