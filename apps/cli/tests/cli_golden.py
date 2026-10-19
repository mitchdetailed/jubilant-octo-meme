#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""ctest cli.golden.<command> (07 section 2.2 item 11, AC-M4-01; D-214): one golden-JSON test per esses-cli
subcommand, each comparing the command's JSON (or the file it writes) with expectations that do not come from
the code under test (D-041):

  info      `info --json` equals the fixture's committed expected.probe.json byte for byte, and its channel
            count, sample counts and rates agree with tools/ld_ref_decode.py on the same input.
  channels  `channels --json` against tools/ld_ref_decode.py (names, rates, sample counts, vendor units) and,
            for a text family, against the generator's expected.channels (names in order, row counts).
  laps      `laps --json --no-sidecar` equals expected.laps.json (the generator's hand-checked lap tables).
  eval      `eval --json "'<channel>' * 2 + 1"` at the channel's own rate: every value is 2 v + 1 of the
            oracle's value v (expected.channels), invalid samples are null, the counts and the rate agree.
  convert   .ld -> motec.ld -> read back: tools/ld_ref_decode.py on the converted file gives the committed
            oracle CSVs byte for byte (`fixture add --oracle ref-decode`) and the reader agrees with it there
            (`fixture verify`); --range cuts a lap (bounds from expected.laps.json) to exactly the oracle's
            samples of the lap; csv and motec.csv carry the oracle's values on the lap's grid and the motec.csv
            reads back; the input is never overwritten (exit 2, bytes unchanged); --rate with motec.ld is
            refused.
  export    `export --json` (no project) reports the logged channel and writes the oracle's values.
  validate  `validate --json` over every schemas/examples/<schema>/{valid,invalid}: the status of each file is
            the name of the directory it lies in.
  fixture   `fixture verify --json` passes every family of a manifest written here; `fixture add --oracle
            ref-decode` on a copy of an input reproduces the committed oracle CSVs and the input hash.
The `bench` row of item 11 is the benchmark executables' (D-155: there is no `esses-cli bench`; `ctest -L
bench`). Standard library only.

  python -I cli_golden.py <command> --cli <esses-cli> --root <repo> --inputs <build>/fixture-inputs/motec.ld
                          --work <dir>
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import pathlib
import shutil
import subprocess
import sys

LD_FAMILIES = ("synthetic-multirate", "synthetic-gap-blocks", "synthetic-device-events", "synthetic-ldx-companion",
               "synthetic-dtype-03-2", "synthetic-shift-mul-after")
TEXT_FAMILIES = ("motec.csv/dialect-basic", "generic.csv/sparse-gps")
LAP_FAMILIES = ("synthetic-beacons-numbering", "synthetic-beacons-reset", "synthetic-beacons-pit",
                "synthetic-ldx-companion")
LAP_FAMILY = "synthetic-beacons-numbering"  # Throttle Pos at 10 Hz; Lap 8 = [30 s, 50.5 s)
LAP_CHANNEL = "Throttle Pos"
LAP_NUMBER = 8


class Check:
    def __init__(self, name: str):
        self.name = name
        self.failures: list[str] = []
        self.passed = 0

    def expect(self, ok: bool, what: str) -> bool:
        if ok:
            self.passed += 1
        else:
            self.failures.append(what)
            print(f"cli_golden {self.name}: FAIL: {what}", file=sys.stderr)
        return ok

    def finish(self) -> int:
        if self.failures:
            print(f"cli_golden {self.name}: {len(self.failures)} failed, {self.passed} passed")
            return 1
        print(f"cli_golden {self.name}: PASS ({self.passed} checks)")
        return 0


class Env:
    def __init__(self, a: argparse.Namespace):
        self.cli = a.cli
        self.root = pathlib.Path(a.root).resolve()
        self.inputs = pathlib.Path(a.inputs).resolve()
        self.work = pathlib.Path(a.work).resolve()
        self.python = a.python or sys.executable
        if self.work.exists():
            shutil.rmtree(self.work)
        self.work.mkdir(parents=True)

    def fixture(self, family: str) -> pathlib.Path:
        return self.root / "fixtures" / "motec.ld" / family

    def input(self, family: str) -> pathlib.Path:
        return self.inputs / family / "input.ld"

    def run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([self.cli, *args], capture_output=True, check=False)

    def ref_decode(self, ld: pathlib.Path) -> dict:
        tool = self.root / "tools" / "ld_ref_decode.py"
        r = subprocess.run([self.python, "-I", str(tool), "--json", "--no-hash", "--no-stats", str(ld)],
                           capture_output=True, check=False)
        if r.returncode != 0:
            raise RuntimeError(f"ld_ref_decode failed on {ld}: {r.stderr.decode(errors='replace')}")
        return json.loads(r.stdout)


def percent_decode(text: str) -> str:
    """The inverse of the expected.channels file-name encoding (%XX for a byte)."""
    raw = text.encode("utf-8")
    out = bytearray()
    i = 0
    while i < len(raw):
        if raw[i] == ord("%") and i + 3 <= len(raw):
            out.append(int(raw[i + 1:i + 3].decode("ascii"), 16))
            i += 3
        else:
            out.append(raw[i])
            i += 1
    return out.decode("utf-8")


def oracle(fixture_dir: pathlib.Path) -> dict[str, list[dict]]:
    """expected.channels/<NNN>_<name>.csv -> {name: rows} in file order (index, timeUs, value, valid)."""
    out: dict[str, list[dict]] = {}
    for f in sorted((fixture_dir / "expected.channels").glob("*.csv")):
        name = percent_decode(f.stem.split("_", 1)[1])
        with f.open(newline="", encoding="utf-8") as h:
            out[name] = list(csv.DictReader(h))
    return out


def value_of(row: dict) -> float | None:
    return float(row["value"]) if row["valid"] == "1" else None


def close(a: float, b: float, rel: float = 2e-6, absolute: float = 1e-6) -> bool:
    return abs(a - b) <= max(absolute, rel * max(abs(a), abs(b)))


def lap_bounds(laps_json: dict, number: int) -> tuple[int, int]:
    def walk(groups):
        for g in groups:
            if g["path"] == "Outings:Laps":
                for r in g["ranges"]:
                    if r.get("number") == number:
                        return r["startUs"], r["endUs"]
            found = walk(g.get("children", []))
            if found:
                return found
        return None

    found = walk(laps_json["layers"]["Normal"]["groups"])
    if found is None:
        raise RuntimeError(f"no lap {number} in expected.laps.json")
    return found


def read_csv_rows(path: pathlib.Path) -> list[list[str]]:
    with path.open(newline="", encoding="utf-8") as h:
        return list(csv.reader(h))


def decimals(text: str) -> int:
    return len(text.split(".", 1)[1]) if "." in text else 0


# ---- info ----------------------------------------------------------------------------------------------------
def cmd_info(env: Env, c: Check) -> None:
    for family in LD_FAMILIES:
        r = env.run("info", "--json", str(env.input(family)))
        expected = (env.fixture(family) / "expected.probe.json").read_bytes()
        if not c.expect(r.returncode == 0, f"info {family}: exit {r.returncode}"):
            continue
        c.expect(r.stdout == expected, f"info --json {family} differs from expected.probe.json")
        probe = json.loads(r.stdout)
        ref = env.ref_decode(env.input(family))
        chans = probe["layers"][0]["channels"]
        refs = ref["channels"]
        if c.expect(len(chans) == len(refs), f"info {family}: {len(chans)} channels, ref-decode {len(refs)}"):
            for p, q in zip(chans, refs):
                c.expect(p["sampleCount"] == q["count"], f"info {family} {q['name']}: samples")
                c.expect(p["timing"]["rate"] == {"num": q["freq"], "den": 1}, f"info {family} {q['name']}: rate")
    for family in TEXT_FAMILIES:
        d = env.root / "fixtures" / family
        inp = next(p for p in d.iterdir() if p.name.startswith("input."))
        r = env.run("info", "--json", str(inp))
        c.expect(r.returncode == 0 and r.stdout == (d / "expected.probe.json").read_bytes(),
                 f"info --json {family} differs from expected.probe.json")


# ---- channels ------------------------------------------------------------------------------------------------
def cmd_channels(env: Env, c: Check) -> None:
    for family in LD_FAMILIES:
        r = env.run("channels", "--json", "--no-sidecar", str(env.input(family)))
        if not c.expect(r.returncode == 0, f"channels {family}: exit {r.returncode}"):
            continue
        rows = json.loads(r.stdout)["layers"]["Normal"]
        refs = env.ref_decode(env.input(family))["channels"]
        if not c.expect(len(rows) == len(refs), f"channels {family}: {len(rows)} rows, ref-decode {len(refs)}"):
            continue
        for row, q in zip(rows, refs):
            c.expect(row["loggedName"] == q["name"], f"channels {family}: '{row['loggedName']}' != '{q['name']}'")
            c.expect(row["samples"] == q["count"], f"channels {family} {q['name']}: samples")
            c.expect(row["rate"] == {"num": q["freq"], "den": 1}, f"channels {family} {q['name']}: rate")
            # 05 section 3.4 unit rule (c): an empty 12-byte unit field leaves the unit in the 8-byte field
            # (none of these families is c-series), which the reference decoder reports as the short name.
            unit = q["unit"] if q["unit"] else q["short_name"]
            c.expect(row["vendorUnit"] == unit, f"channels {family} {q['name']}: vendor unit")
    d = env.root / "fixtures" / "generic.csv" / "sparse-gps"
    inp = next(p for p in d.iterdir() if p.name.startswith("input."))
    r = env.run("channels", "--json", str(inp))
    if c.expect(r.returncode == 0, "channels sparse-gps: exit"):
        rows = json.loads(r.stdout)["layers"]["Normal"]
        expected = oracle(d)
        c.expect([x["name"] for x in rows] == list(expected), "channels sparse-gps: names and order")
        for x in rows:
            c.expect(x["samples"] == len(expected.get(x["name"], [])), f"channels sparse-gps {x['name']}: samples")


# ---- laps ----------------------------------------------------------------------------------------------------
def cmd_laps(env: Env, c: Check) -> None:
    for family in LAP_FAMILIES:
        r = env.run("laps", "--json", "--no-sidecar", str(env.input(family)))
        c.expect(r.returncode == 0 and r.stdout == (env.fixture(family) / "expected.laps.json").read_bytes(),
                 f"laps --json {family} differs from expected.laps.json")


# ---- eval ----------------------------------------------------------------------------------------------------
def cmd_eval(env: Env, c: Check) -> None:
    for family, channel, rate in (("synthetic-multirate", "Rate 3 Hz", "3"),
                                  ("synthetic-multirate", "Rate 333 Hz", "333"),
                                  ("synthetic-gap-blocks", "Lambda", "20"),
                                  ("synthetic-dtype-03-2", "Scaled (0x03, 2)", "10")):
        expected = oracle(env.fixture(family))[channel]
        r = env.run("eval", "--json", f"'{channel}' * 2 + 1", str(env.input(family)), "--rate", rate)
        if not c.expect(r.returncode == 0, f"eval {family} {channel}: exit {r.returncode} {r.stderr!r}"):
            continue
        result = json.loads(r.stdout)["results"][0]
        c.expect(result["rate"] == {"num": int(rate), "den": 1}, f"eval {family} {channel}: rate {result['rate']}")
        # The result spans the layer (its longest channel): after the channel's own samples it is invalid.
        c.expect(result["count"] >= len(expected), f"eval {family} {channel}: count {result['count']}")
        c.expect(all(v is None for v in result["values"][len(expected):]),
                 f"eval {family} {channel}: values after the channel's end")
        valid = sum(1 for row in expected if row["valid"] == "1")
        c.expect(result["validCount"] == valid, f"eval {family} {channel}: validCount {result['validCount']}")
        bad = 0
        for got, row in zip(result["values"], expected):
            v = value_of(row)
            want = None if v is None or not math.isfinite(v) else 2.0 * v + 1.0
            if (got is None) != (want is None) or (got is not None and not close(got, want)):
                bad += 1
        c.expect(bad == 0, f"eval {family} {channel}: {bad} values differ from 2 v + 1 of the oracle")


# ---- convert -------------------------------------------------------------------------------------------------
def read_back(env: Env, c: Check, ld: pathlib.Path, name: str) -> pathlib.Path | None:
    """fixture add --oracle ref-decode on a converted file: its directory, verified by `fixture verify`."""
    out = env.work / f"readback-{name}"
    r = env.run("fixture", "add", str(ld), "--format", "motec.ld", "--oracle", "ref-decode", "--out", str(out),
                "--licence", "CC0-1.0", "--provenance", "synthetic", "--quiet", "--python", env.python,
                "--ref-decode", str(env.root / "tools" / "ld_ref_decode.py"))
    if not c.expect(r.returncode == 0, f"fixture add on {ld.name}: exit {r.returncode} {r.stderr!r}"):
        return None
    v = env.run("fixture", "verify", str(out))
    c.expect(v.returncode == 0 and b"1 passed, 0 failed" in v.stdout,
             f"the reader disagrees with ld_ref_decode.py on {ld.name}: {v.stdout!r}")
    return out


def cmd_convert(env: Env, c: Check) -> None:
    # (a) .ld -> motec.ld -> read back, every family whole.
    for family in LD_FAMILIES:
        out = env.work / f"{family}.ld"
        r = env.run("convert", str(env.input(family)), "--format", "motec.ld", "-o", str(out), "--json")
        if not c.expect(r.returncode == 0, f"convert {family}: exit {r.returncode} {r.stderr!r}"):
            continue
        report = json.loads(r.stdout)
        c.expect(report["skipped"] == [] and report["format"] == "motec.ld", f"convert {family}: report {report}")
        back = read_back(env, c, out, family)
        if back is None:
            continue
        committed = sorted((env.fixture(family) / "expected.channels").glob("*.csv"))
        got = sorted((back / "expected.channels").glob("*.csv"))
        c.expect([p.name for p in got] == [p.name for p in committed], f"convert {family}: channel files")
        strict = json.loads((env.fixture(family) / "fixture.json").read_text(encoding="utf-8"))["oracle"].get(
            "validity") == "strict"
        for a, b in zip(got, committed):
            if not strict:
                c.expect(a.read_bytes() == b.read_bytes(), f"convert {family}: {a.name} differs after the round trip")
                continue
            # A strict oracle's validity is the generator's declaration (the reference decoder does not decode
            # gap blocks): the values must match, and the reader's validity on the converted file (eval's nulls)
            # must be the declared one.
            mine_rows = list(csv.DictReader(a.open(newline="", encoding="utf-8")))
            want_rows = list(csv.DictReader(b.open(newline="", encoding="utf-8")))
            c.expect([(r["index"], r["timeUs"], r["value"]) for r in mine_rows] ==
                     [(r["index"], r["timeUs"], r["value"]) for r in want_rows],
                     f"convert {family}: {a.name} values differ after the round trip")
            name = percent_decode(a.stem.split("_", 1)[1])
            rate = next(q["freq"] for q in env.ref_decode(out)["channels"] if q["name"] == name)
            e = env.run("eval", "--json", f"'{name}'", str(out), "--rate", str(rate))
            if c.expect(e.returncode == 0, f"eval on the converted {family} '{name}': exit {e.returncode}"):
                nulls = [v is None for v in json.loads(e.stdout)["results"][0]["values"][:len(want_rows)]]
                c.expect(nulls == [r["valid"] == "0" for r in want_rows],
                         f"convert {family}: the validity of '{name}' differs after the round trip")
        # Markers (device events in the .ld, beacons in the .ldx) survive the round trip.
        mine = json.loads((back / "expected.markers.json").read_text(encoding="utf-8"))
        theirs = json.loads((env.fixture(family) / "expected.markers.json").read_text(encoding="utf-8"))
        c.expect(mine == theirs, f"convert {family}: markers differ after the round trip")

    # (b) --range: Lap 8 of the beacon family, cut to exactly the oracle's samples of the lap.
    laps = json.loads((env.fixture(LAP_FAMILY) / "expected.laps.json").read_text(encoding="utf-8"))
    start, end = lap_bounds(laps, LAP_NUMBER)
    lap_rows = [row for row in oracle(env.fixture(LAP_FAMILY))[LAP_CHANNEL] if start <= int(row["timeUs"]) < end]
    cut = env.work / "lap.ld"
    r = env.run("convert", str(env.input(LAP_FAMILY)), "--format", "motec.ld", "--range",
                f"Outings:Laps/{LAP_NUMBER}", "-o", str(cut))
    if c.expect(r.returncode == 0, f"convert --range: exit {r.returncode} {r.stderr!r}"):
        back = read_back(env, c, cut, "lap")
        if back is not None:
            rows = oracle(back)[LAP_CHANNEL]
            c.expect(len(rows) == len(lap_rows), f"convert --range: {len(rows)} samples, lap has {len(lap_rows)}")
            c.expect(all(a["value"] == b["value"] and a["valid"] == b["valid"] for a, b in zip(rows, lap_rows)),
                     "convert --range: values differ from the lap's oracle samples")
            c.expect(rows and int(rows[0]["timeUs"]) == 0, "convert --range: the lap starts at time 0")
            got = json.loads(env.run("laps", "--json", "--no-sidecar", str(cut)).stdout)
            c.expect(got["layers"]["Normal"]["durationUs"] == end - start, "convert --range: duration")

    # (c) csv and (d) motec.csv on the lap's 10 Hz grid; the motec.csv reads back.
    for fmt in ("csv", "motec.csv"):
        out = env.work / f"lap-{fmt.replace('.', '-')}.csv"
        r = env.run("convert", str(env.input(LAP_FAMILY)), "--format", fmt, "--channels", LAP_CHANNEL, "--range",
                    f"Outings:Laps/{LAP_NUMBER}", "--rate", "10", "-o", str(out))
        if not c.expect(r.returncode == 0, f"convert --format {fmt}: exit {r.returncode} {r.stderr!r}"):
            continue
        rows = read_csv_rows(out)
        header = next(i for i, row in enumerate(rows) if row[:2] == ["Time", LAP_CHANNEL])
        data = [row for row in rows[header + 2:] if row and row[0] != ""]
        c.expect(len(data) == len(lap_rows), f"convert {fmt}: {len(data)} rows, the lap has {len(lap_rows)}")
        bad = 0
        for k, (row, want) in enumerate(zip(data, lap_rows)):
            t = float(row[0])
            v = float(row[1])
            ok_t = abs(t - k * 0.1) <= 0.5 * 10 ** -decimals(row[0]) + 1e-9
            ok_v = abs(v - float(want["value"])) <= 0.5 * 10 ** -decimals(row[1]) + 1e-9
            bad += 0 if ok_t and ok_v else 1
        c.expect(bad == 0, f"convert {fmt}: {bad} rows differ from the oracle on the lap's grid")
        if fmt == "motec.csv":
            back = env.run("channels", "--json", str(out))
            if c.expect(back.returncode == 0, "motec.csv read back: exit"):
                doc = json.loads(back.stdout)
                chans = doc["layers"]["Normal"]
                c.expect(doc["importerId"] == "motec.csv" and [x["name"] for x in chans] == [LAP_CHANNEL]
                         and chans[0]["samples"] == len(lap_rows), f"motec.csv read back: {chans}")

    # (e) never the input: the log itself, its companion; the input keeps its bytes.
    src = env.work / "keep.ld"
    shutil.copyfile(env.input("synthetic-ldx-companion"), src)
    shutil.copyfile(env.input("synthetic-ldx-companion").with_suffix(".ldx"), src.with_suffix(".ldx"))
    before = hashlib.sha256(src.read_bytes()).hexdigest()
    # keep.csv as motec.ld: its companion would be keep.ldx, the input's companion.
    for target, fmt in ((src, "motec.ld"), (src, "csv"), (src.with_suffix(".ldx"), "csv"),
                        (env.work / "keep.csv", "motec.ld")):
        r = env.run("convert", str(src), "--format", fmt, "-o", str(target))
        c.expect(r.returncode == 2 and b"refusing to overwrite the input" in r.stderr,
                 f"convert -o {target.name} ({fmt}) should be refused: exit {r.returncode}")
    c.expect(hashlib.sha256(src.read_bytes()).hexdigest() == before, "convert changed its input")
    # (f) the .ld writer does not resample in 0.1.
    r = env.run("convert", str(src), "--format", "motec.ld", "--rate", "10", "-o", str(env.work / "r.ld"))
    c.expect(r.returncode == 2, f"convert --rate with motec.ld should be refused: exit {r.returncode}")
    r = env.run("convert", str(src), "--format", "parquet", "-o", str(env.work / "x.parquet"))
    c.expect(r.returncode == 2, f"convert --format parquet should be a usage error: exit {r.returncode}")
    r = env.run("convert", str(src), "--format", "csv", "--channels", "No Such Channel", "-o",
                str(env.work / "x.csv"))
    c.expect(r.returncode == 1, f"convert of an unknown channel should exit 1: exit {r.returncode}")


# ---- export --------------------------------------------------------------------------------------------------
def cmd_export(env: Env, c: Check) -> None:
    laps = json.loads((env.fixture(LAP_FAMILY) / "expected.laps.json").read_text(encoding="utf-8"))
    start, end = lap_bounds(laps, LAP_NUMBER)
    lap_rows = [row for row in oracle(env.fixture(LAP_FAMILY))[LAP_CHANNEL] if start <= int(row["timeUs"]) < end]
    out = env.work / "export.csv"
    r = env.run("export", str(env.input(LAP_FAMILY)), "--channels", LAP_CHANNEL, "--range",
                f"Outings:Laps/{LAP_NUMBER}", "--rate", "10", "--no-sidecar", "--json", "-o", str(out))
    if not c.expect(r.returncode == 0, f"export: exit {r.returncode} {r.stderr!r}"):
        return
    report = json.loads(r.stdout)
    golden = {"channels": [{"kind": "logged", "name": LAP_CHANNEL}], "format": "csv",
              "range": f"Outings:Laps/{LAP_NUMBER}"}
    c.expect({k: report.get(k) for k in golden} == golden, f"export --json report {report}")
    c.expect(report.get("bytes") == out.stat().st_size, "export --json bytes")
    rows = read_csv_rows(out)
    c.expect(rows[0] == ["Time", LAP_CHANNEL], f"export names row {rows[0]}")
    data = [row for row in rows[2:] if row]
    c.expect(len(data) == len(lap_rows), f"export: {len(data)} rows, the lap has {len(lap_rows)}")
    c.expect(all(abs(float(row[1]) - float(want["value"])) <= 0.5 * 10 ** -decimals(row[1]) + 1e-9
                 for row, want in zip(data, lap_rows)), "export: values differ from the oracle")
    r = env.run("export", str(env.input(LAP_FAMILY)), "--format", "motec.ld", "-o", str(env.work / "x.ld"))
    c.expect(r.returncode == 2 and b"esses-cli convert" in r.stderr, "export --format motec.ld points to convert")


# ---- validate ------------------------------------------------------------------------------------------------
def cmd_validate(env: Env, c: Check) -> None:
    examples = env.root / "schemas" / "examples"
    checked = 0
    for d in sorted(x for x in examples.rglob("*") if x.is_dir() and (x / "valid").is_dir()):
        schema = env.root / "schemas" / d.relative_to(examples).parent / f"{d.name}.schema.json"
        if not schema.is_file():
            continue
        checked += 1
        expect = json.loads((d / "expect.json").read_text(encoding="utf-8")) if (d / "expect.json").is_file() else {}
        subs = [x for x in (d / "valid", d / "invalid") if x.is_dir()]
        r = env.run("validate", "--json", "--schema", str(schema), *map(str, subs))
        any_invalid = any((d / "invalid").glob("*.json")) if (d / "invalid").is_dir() else False
        c.expect(r.returncode == (1 if any_invalid else 0), f"validate {d.name}: exit {r.returncode}")
        report = json.loads(r.stdout)
        expected = {str(p.resolve()): p.parent.name for sub in subs for p in sub.rglob("*.json")}
        got = {str(pathlib.Path(f["path"]).resolve()): f for f in report["files"]}
        c.expect({k: f["status"] for k, f in got.items()} == expected,
                 f"validate {d.relative_to(examples)}: statuses differ from the layout")
        for path, f in got.items():
            name = pathlib.Path(path).name
            if f["status"] == "invalid" and name in expect:
                pointers = [e.get("pointer") for e in f.get("errors", [])]
                c.expect(expect[name]["pointer"] in pointers,
                         f"validate {d.name}/{name}: no error at '{expect[name]['pointer']}' ({pointers})")
    c.expect(checked >= 30, f"only {checked} example directories have a schema")
    # Without --schema, each document is checked against the schema its $schema names.
    r = env.run("validate", "--json", str(examples / "workbook-1" / "valid"), str(examples / "project-1" / "valid"))
    c.expect(r.returncode == 0 and all(f["status"] == "valid" for f in json.loads(r.stdout)["files"]),
             "validate by $schema: the valid workbook and project examples")


# ---- fixture -------------------------------------------------------------------------------------------------
def cmd_fixture(env: Env, c: Check) -> None:
    manifest = {"fixtures": [{"dir": str(env.fixture(f)), "input": str(env.input(f))} for f in LD_FAMILIES]}
    path = env.work / "manifest.json"
    path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    r = env.run("fixture", "verify", "--json", "--manifest", str(path))
    if c.expect(r.returncode == 0, f"fixture verify: exit {r.returncode} {r.stderr!r}"):
        report = json.loads(r.stdout)
        got = {f["id"]: f["status"] for f in report["fixtures"]}
        c.expect(got == {f"motec.ld/{f}": "PASS" for f in LD_FAMILIES}, f"fixture verify --json: {got}")
    family = "synthetic-multirate"
    copy = env.work / "copy" / "input.ld"
    copy.parent.mkdir()
    shutil.copyfile(env.input(family), copy)
    out = env.work / "added"
    r = env.run("fixture", "add", str(copy), "--format", "motec.ld", "--oracle", "ref-decode", "--out", str(out),
                "--licence", "CC0-1.0", "--provenance", "synthetic", "--json", "--python", env.python,
                "--ref-decode", str(env.root / "tools" / "ld_ref_decode.py"))
    if c.expect(r.returncode == 0, f"fixture add: exit {r.returncode} {r.stderr!r}"):
        report = json.loads(r.stdout)
        c.expect(report["channels"] == 14 and report["schemaValid"] is True, f"fixture add --json: {report}")
        committed = json.loads((env.fixture(family) / "fixture.json").read_text(encoding="utf-8"))
        added = json.loads((out / "fixture.json").read_text(encoding="utf-8"))
        c.expect(added["input"]["sha256"] == committed["input"]["sha256"], "fixture add: input sha256")
        for a, b in zip(sorted((out / "expected.channels").glob("*.csv")),
                        sorted((env.fixture(family) / "expected.channels").glob("*.csv"))):
            c.expect(a.name == b.name and a.read_bytes() == b.read_bytes(), f"fixture add: {a.name}")


COMMANDS = {"info": cmd_info, "channels": cmd_channels, "laps": cmd_laps, "eval": cmd_eval, "convert": cmd_convert,
            "export": cmd_export, "validate": cmd_validate, "fixture": cmd_fixture}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=sorted(COMMANDS))
    ap.add_argument("--cli", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--inputs", required=True, help="<build>/fixture-inputs/motec.ld")
    ap.add_argument("--work", required=True)
    ap.add_argument("--python", help="interpreter for tools/ld_ref_decode.py (default: this one)")
    a = ap.parse_args()
    env = Env(a)
    check = Check(a.command)
    COMMANDS[a.command](env, check)
    return check.finish()


if __name__ == "__main__":
    sys.exit(main())
