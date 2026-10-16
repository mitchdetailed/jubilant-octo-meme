#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Self-authored text fixtures for the CSV importers (AC-M2-13, 05 sections 6, 7 and 9.1; D-041).

Writes, for each family below, fixtures/<format>/<name>/:
  input.csv | input.txt            the text file (synthetic values from closed-form functions of the row; CC0-1.0)
  expected.channels/NNN_<name>.csv every channel's expected samples "index,timeUs,value,valid", computed here from
                                   the values this script printed (never from the reader): sparse "hold" cells
                                   repeat the previous value, sentinels are invalid, text columns are enumeration
                                   ids in first-seen order, timestamped rows follow the time rule of 05 section 7.1
                                   (later times kept, an equal time replaces the previous row, earlier ones dropped)
  fixture.json                     fixture-1 with the input's sha256 and size, the dialect the probe must report and
                                   the oracle kind "generator"
  README.md                        what the family exercises
expected.probe.json is `esses-cli info --json` output, reviewed against the facts above (esses-cli fixture verify
--update writes it once; afterwards fixture verify compares it byte for byte).

Families:
  motec.csv/dialect-basic                 the legacy export skeleton: eleven quoted key/value lines, names and units
                                          rows, every field quoted, CRLF, 20.48 Hz from the details, sentinels, held
                                          empty cells, a Lap counter
  generic.csv/semicolon-decimal-comma     a free-text preamble, ';' delimiter, ',' decimal point, units in names,
                                          a text column, 50 Hz inferred
  generic.csv/sparse-gps                  a units row, 100 Hz rows with 10 Hz GPS columns left empty in between
                                          (sparse "timestamped": own time columns), a Lap counter
  generic.csv/timestamped-tab             tab-delimited, epoch milliseconds with jitter, a repeated and an earlier
                                          time (timestamped channels, last wins, one row dropped)

Standard library only:  python -I tools/gen_text_fixtures.py --root <repo> [--check]
--check regenerates into memory and exits 1 when a committed file differs (the generator is deterministic).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import sys
from fractions import Fraction

VERSION = "1.0.0"
TOLERANCE = {"F32": "1ulp", "F64": "1ulp", "I32": "exact", "timesUs": "exact"}


def us(seconds: Fraction | float) -> int:
    """Microseconds of an exact time, rounded half up like Rate::timeAt."""
    if isinstance(seconds, Fraction):
        q = seconds * 1_000_000
        return math.floor(q + Fraction(1, 2))
    return int(math.floor(seconds * 1_000_000 + 0.5))


def fmt(v: float, dp: int) -> str:
    return f"{v:.{dp}f}"


def repr_value(v: float | None) -> str:
    if v is None:
        return "nan"
    if float(v).is_integer() and abs(v) < 1e15:
        return repr(float(v))
    return repr(float(v))


class Channel:
    """One channel's expected samples: (index, timeUs, value or None)."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.samples: list[tuple[int, int, float | None]] = []

    def add(self, t_us: int, value: float | None) -> None:
        self.samples.append((len(self.samples), t_us, value))

    def csv(self) -> str:
        lines = ["index,timeUs,value,valid"]
        for i, t, v in self.samples:
            lines.append(f"{i},{t},{repr_value(v)},{0 if v is None else 1}")
        return "\n".join(lines) + "\n"


def encode_name(name: str) -> str:
    out = []
    for ch in name:
        if ch.isascii() and (ch.isalnum() or ch in " ._-()"):
            out.append(ch)
        else:
            for b in ch.encode("utf-8"):
                out.append(f"%{b:02X}")
    return "".join(out)


class Family:
    def __init__(self, fmt_id: str, name: str, ext: str, dialect: str, source: str, notes: str, readme: str) -> None:
        self.format = fmt_id
        self.name = name
        self.ext = ext
        self.dialect = dialect
        self.source = source
        self.notes = notes
        self.readme = readme
        self.text = ""
        self.channels: list[Channel] = []

    def files(self) -> dict[str, bytes]:
        data = self.text.encode("utf-8")
        out: dict[str, bytes] = {f"input.{self.ext}": data}
        for k, ch in enumerate(self.channels):
            out[f"expected.channels/{k:03d}_{encode_name(ch.name)}.csv"] = ch.csv().encode("utf-8")
        fx = {
            "$schema": "urn:esses:fixture-1",
            "schemaVersion": 1,
            "channelsCompared": "all",
            "dialect": self.dialect,
            "format": self.format,
            "input": {"bytes": len(data), "file": f"input.{self.ext}", "sha256": hashlib.sha256(data).hexdigest()},
            "licence": "CC0-1.0",
            "name": self.name,
            "notes": self.notes,
            "oracle": {"kind": "generator", "ref": f"tools/gen_text_fixtures.py {VERSION} (MIT)",
                       "tolerance": TOLERANCE, "validity": "strict"},
            "private": False,
            "provenance": "synthetic",
            "source": self.source,
        }
        out["fixture.json"] = canonical(fx).encode("utf-8")
        out["README.md"] = self.readme.encode("utf-8")
        return out


def canonical(value) -> str:
    """02 section 6.5 rule 1 for the documents written here (objects, strings, integers, inline scalar arrays)."""
    def key(k: str):
        return (0 if k == "$schema" else 1 if k == "schemaVersion" else 2, k.encode("utf-8"))

    def ser(v, ind: int) -> str:
        sp = "  " * ind
        if isinstance(v, dict):
            if not v:
                return "{}"
            items = [f"{sp}  {json.dumps(k, ensure_ascii=False)}: {ser(v[k], ind + 1)}" for k in sorted(v, key=key)]
            return "{\n" + ",\n".join(items) + "\n" + sp + "}"
        if isinstance(v, list):
            if not v:
                return "[]"
            if any(isinstance(x, (dict, list)) for x in v):
                return "[\n" + ",\n".join(sp + "  " + ser(x, ind + 1) for x in v) + "\n" + sp + "]"
            return "[" + ", ".join(ser(x, ind + 1) for x in v) + "]"
        if isinstance(v, bool):
            return "true" if v else "false"
        return json.dumps(v, ensure_ascii=False)

    return ser(value, 0) + "\n"


# ---- families -------------------------------------------------------------------------------------------

def legacy_dialect_basic() -> Family:
    readme = """# motec.csv / dialect-basic

Self-authored (CC0-1.0), written by `tools/gen_text_fixtures.py`; every value comes from a closed-form function of
the row. It reproduces the layout facts of the legacy CSV export (05 section 6): eleven quoted key/value metadata
lines with a second pair in columns 5-7, two blank lines, a names row and a units row, two blank lines, data;
every field double-quoted, CRLF, '.' decimal point, one sample rate (20.48 Hz, stated in the details and printed
with three decimals in the time column).

Exercised: details from the key/value columns (empty values are no detail), the rate from the `Sample Rate`
detail ({512, 25}) validated against the printed times, `Corr Dist` stored F64 by the profile's column override,
`-###` sentinels (invalid samples), empty cells held at the previous value (sparse "hold"), integer columns,
the `Lap` counter as the profile's fallback lap source (a beacon at each change).

Expected channel values: `expected.channels/` from the generator (oracle kind `generator`). `expected.probe.json`
is `esses-cli info --json` output reviewed against those facts.
"""
    fam = Family("motec.csv", "dialect-basic", "csv", "",
                 "synthetic: tools/gen_text_fixtures.py, the legacy CSV export skeleton with invented values",
                 "05 section 6 layout facts reproduced with self-authored values; 20.48 Hz from the details",
                 readme)
    rate = Fraction(512, 25)
    n = 1843
    names = ["Corr Dist", "Ground Speed", "Engine RPM", "Throttle Pos", "Gear", "Lap"]
    units = ["m", "km/h", "rpm", "%", "", ""]
    duration = Fraction(n) / rate
    eol = "\r\n"

    def q(s: str) -> str:
        return '"' + s.replace('"', '""') + '"'

    meta = [
        [q("Format"), q("MoTeC CSV File"), "", "", q("Workbook"), q("Fixture")],
        [q("Venue"), q("Synthetic Park"), "", "", q("Worksheet"), q("Overview")],
        [q("Vehicle"), q("SYN-01"), "", "", q("Vehicle Desc"), q("")],
        [q("Driver"), q("Test Driver"), "", "", q("Engine ID"), q("")],
        [q("Device"), q("SYNLOG")],
        [q("Comment"), q("self-authored fixture"), "", "", q("Session"), q("1")],
        [q("Log Date"), q("12/10/2026"), "", "", q("Origin Time"), q("0.000"), q("s")],
        [q("Log Time"), q("09:30:00"), "", "", q("Start Time"), q("0.000"), q("s")],
        [q("Sample Rate"), q("20.480"), q("Hz"), "", q("End Time"), q(fmt(float(duration), 3)), q("s")],
        [q("Duration"), q(fmt(float(duration), 3)), q("s"), "", q("Start Distance"), q("0"), q("m")],
        [q("Range"), q("full log"), "", "", q("End Distance"), q("0"), q("m")],
    ]
    lines = [",".join(r) for r in meta] + ["", ""]
    lines.append(",".join(q(x) for x in ["Time"] + names))
    lines.append(",".join(q(x) for x in ["s"] + units))
    lines += ["", ""]
    chans = [Channel(nm) for nm in names]
    held: list[float | None] = [None] * len(names)
    dist = 0.0
    for i in range(n):
        t = Fraction(i) / rate
        speed = 80.0 + 60.0 * math.sin(2 * math.pi * i / 600.0)
        dist += speed / 3.6 / 20.48
        rpm = int(round(3000 + 40 * speed))
        throttle = 50.0 + 45.0 * math.sin(2 * math.pi * i / 211.0)
        gear = 1 + (i // 300) % 5
        lap = 1 + i // 614
        cells = [fmt(dist, 1), fmt(speed, 1), str(rpm), fmt(throttle, 1), str(gear), str(lap)]
        if i % 97 == 13:
            cells[2] = "-###"          # sentinel: invalid
        if i % 50 in (7, 8):
            cells[3] = ""              # empty: held
        if i % 333 == 5:
            cells[1] = "+###"          # sentinel
        if i == 0:
            cells[4] = ""              # an empty cell before any value: invalid until the first value
        row = [q(fmt(float(t), 3))] + [q(c) for c in cells]
        lines.append(",".join(row))
        for k, c in enumerate(cells):
            if c == "":
                v = held[k]
            elif c in ("-###", "+###"):
                v = None
                held[k] = None
            else:
                v = float(c)
                held[k] = v
            chans[k].add(us(t), v)
    fam.text = eol.join(lines) + eol
    fam.channels = chans
    return fam


def generic_semicolon_decimal_comma() -> Family:
    readme = """# generic.csv / semicolon-decimal-comma

Self-authored (CC0-1.0), written by `tools/gen_text_fixtures.py`. A logger-style export the generic profile reads
with every `auto` rule: a two-line free-text preamble and a blank line, then a names row with units in brackets
(`Speed (km/h)`), ';' as the delimiter and ',' as the decimal point, LF line ends, 50 Hz (inferred from the time
column), an integer column and a text column (`Mode`, an enumeration in first-seen order).

Expected channel values: `expected.channels/` from the generator (oracle kind `generator`). `expected.probe.json`
is `esses-cli info --json` output reviewed against those facts (dialect `semicolon,decimal-comma`).
"""
    fam = Family("text.profile:generic.csv", "semicolon-decimal-comma", "csv", "semicolon,decimal-comma",
                 "synthetic: tools/gen_text_fixtures.py, a semicolon and decimal-comma table with a free-text preamble",
                 "decimal comma, units in names, a text column, 50 Hz inferred", readme)
    n = 1500
    names = ["Speed", "Brake Pres", "Gear", "Mode"]
    header = "Time;Speed (km/h);Brake Pres (bar);Gear;Mode"
    lines = ["Synthetic logger export", "Driver: Test Driver", "", header]
    chans = [Channel(nm) for nm in names]
    modes = ["RACE", "PIT", "SAFE"]
    mode_ids: dict[str, int] = {}
    for i in range(n):
        t = Fraction(i, 50)
        speed = 120.0 + 80.0 * math.cos(2 * math.pi * i / 400.0)
        brake = max(0.0, 40.0 * math.sin(2 * math.pi * i / 150.0))
        gear = 2 + (i // 250) % 4
        mode = modes[0] if i < 900 else modes[1] if i < 1100 else modes[2] if i < 1200 else modes[0]
        cells = [fmt(speed, 2).replace(".", ","), fmt(brake, 2).replace(".", ","), str(gear), mode]
        lines.append(";".join([fmt(float(t), 3).replace(".", ",")] + cells))
        if mode not in mode_ids:
            mode_ids[mode] = len(mode_ids)
        values = [float(fmt(speed, 2)), float(fmt(brake, 2)), float(gear), float(mode_ids[mode])]
        for k, v in enumerate(values):
            chans[k].add(us(t), v)
    fam.text = "\n".join(lines) + "\n"
    fam.channels = chans
    return fam


def generic_sparse_gps() -> Family:
    readme = """# generic.csv / sparse-gps

Self-authored (CC0-1.0), written by `tools/gen_text_fixtures.py`. A 100 Hz table with a units row in which the GPS
columns (`GPS Lat`, `GPS Lon`, 10 Hz) are filled on every tenth row only. Under the generic profile's sparse
"timestamped" rule a column with empty cells keeps only its own samples, each with the time of its row, in its
own time column; the dense columns stay uniform at 100 Hz. The coordinates have seven decimals (stored F64). A
`Lap` counter gives a beacon at each change.

Expected channel values: `expected.channels/` from the generator (oracle kind `generator`), with the times of the
GPS samples. `expected.probe.json` is `esses-cli info --json` output reviewed against those facts.
"""
    fam = Family("text.profile:generic.csv", "sparse-gps", "csv", "",
                 "synthetic: tools/gen_text_fixtures.py, a 100 Hz table with 10 Hz GPS columns left empty in between",
                 "sparse timestamped columns with their own times; a lap counter", readme)
    n = 4000
    names = ["Speed", "GPS Lat", "GPS Lon", "Lap"]
    lines = ["Time,Speed,GPS Lat,GPS Lon,Lap", "s,km/h,deg,deg,"]
    chans = [Channel(nm) for nm in names]
    for i in range(n):
        t = Fraction(i, 100)
        speed = 90.0 + 30.0 * math.sin(2 * math.pi * i / 1000.0)
        lap = 1 + i // 1500
        cells = [fmt(speed, 2), "", "", str(lap)]
        if i % 10 == 0:
            lat = 51.5 + 0.004 * math.sin(2 * math.pi * i / 1500.0)
            lon = -1.25 + 0.006 * math.cos(2 * math.pi * i / 1500.0)
            cells[1] = fmt(lat, 7)
            cells[2] = fmt(lon, 7)
        lines.append(",".join([fmt(float(t), 2)] + cells))
        chans[0].add(us(t), float(cells[0]))
        if cells[1]:
            chans[1].add(us(t), float(cells[1]))
            chans[2].add(us(t), float(cells[2]))
        chans[3].add(us(t), float(lap))
    fam.text = "\n".join(lines) + "\n"
    fam.channels = chans
    return fam


def generic_timestamped_tab() -> Family:
    readme = """# generic.csv / timestamped-tab

Self-authored (CC0-1.0), written by `tools/gen_text_fixtures.py`. A tab-delimited table with a units row whose
`Timestamp` column holds Unix epoch milliseconds with jitter (18 to 23 ms steps) and a 1.5 s pause, so no rate
fits and the channels are timestamped (05 section 7.1). One row repeats the previous time (it replaces it: last wins) and one
row goes back in time (it is dropped). The epoch unit sets the layer's UTC anchor; times count from the first row.

Expected channel values: `expected.channels/` from the generator (oracle kind `generator`), applying the time rule
to the printed rows. `expected.probe.json` is `esses-cli info --json` output reviewed against those facts.
"""
    fam = Family("text.profile:generic.csv", "timestamped-tab", "txt", "tab,timestamped",
                 "synthetic: tools/gen_text_fixtures.py, tab-separated epoch-millisecond rows with jitter",
                 "timestamped rows: jitter and a pause, a repeated time (last wins), an earlier time (dropped)", readme)
    names = ["Battery", "Coolant"]
    lines = ["Timestamp\tBattery\tCoolant", "ms\tV\tdeg C"]
    t0 = 1_791_800_000_000
    rows: list[tuple[int, float, float]] = []
    t = t0
    for i in range(600):
        step = 18 + (i * 7) % 6
        if i == 300:
            step += 1500                                 # a logging pause: no rate fits the whole file
        t += step
        battery = 13.8 + 0.2 * math.sin(i / 25.0)
        coolant = 85.0 + 5.0 * math.sin(i / 90.0)
        rows.append((t, round(battery, 3), round(coolant, 2)))
    rows.insert(250, (rows[249][0], 12.0, 80.0))        # the same time as row 249: replaces it (last wins)
    rows.insert(400, (rows[399][0] - 5, 11.0, 70.0))    # earlier than the row before: dropped
    for ts, b, c in rows:
        lines.append(f"{ts}\t{fmt(b, 3)}\t{fmt(c, 2)}")
    # The time rule on the printed rows (05 section 7.1): pending sample, last wins, earlier dropped.
    kept: list[tuple[int, float, float]] = []
    pending = None
    for row in rows:
        if pending is None:
            if kept and row[0] <= kept[-1][0]:
                continue
            pending = row
        elif row[0] > pending[0]:
            kept.append(pending)
            pending = row
        elif row[0] == pending[0]:
            pending = row
    if pending is not None:
        kept.append(pending)
    chans = [Channel(nm) for nm in names]
    origin = rows[0][0]
    for ts, b, c in kept:
        chans[0].add((ts - origin) * 1000, float(fmt(b, 3)))
        chans[1].add((ts - origin) * 1000, float(fmt(c, 2)))
    fam.text = "\n".join(lines) + "\n"
    fam.channels = chans
    return fam


FAMILIES = [legacy_dialect_basic, generic_semicolon_decimal_comma, generic_sparse_gps, generic_timestamped_tab]


def stem(fmt_id: str) -> str:
    return fmt_id[len("text.profile:"):] if fmt_id.startswith("text.profile:") else fmt_id


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", required=True, help="repository root")
    ap.add_argument("--check", action="store_true", help="compare instead of writing")
    a = ap.parse_args(argv)
    root = pathlib.Path(a.root)
    differences = 0
    for make in FAMILIES:
        fam = make()
        d = root / "fixtures" / stem(fam.format) / fam.name
        for rel, data in fam.files().items():
            p = d / rel
            if a.check:
                if not p.exists() or p.read_bytes() != data:
                    print(f"differs: {p}")
                    differences += 1
                continue
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        if not a.check:
            print(f"wrote {d}")
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
