#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""ctest cli.export.project: esses-cli export with a project over the generated synthetic-acc-dialect input.

Runs the command (07 section 5.1: project-aware CSV with maths) and checks the CSV against relations that do not
come from the code under test (D-041): the project's mapping renames GEAR to Gear at open; "Speed Twice" is
'SPEED'[km/h] * 2 written in its declared km/h, so it is 7.2 x SPEED (m/s) on every row; "Gear Plus" is Gear + 1;
"Lap Max Speed" (stat_max without reset) is the maximum of SPEED over the exported lap, the Main range of the
selection-dependent node; the override writes Corr Speed in km/h; the time column runs from 0 at the lap start
on the --rate grid; a channel that does not exist is exit status 1, and so is a maths channel whose input fails
at evaluation ("Too Fast" filters above half the rate), reported with the failure, not a "Calculating" status.
Standard library only.

  python -I export_check.py --cli <esses-cli> --project <dir> --input <input.ld> --work <dir>
"""
from __future__ import annotations

import argparse
import csv
import pathlib
import subprocess
import sys


def fail(message: str) -> int:
    print(f"export_check: FAIL: {message}", file=sys.stderr)
    return 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--project", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    work = pathlib.Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    out = work / "lap2.csv"
    channels = "Corr Speed,SPEED,Speed Twice,Lap Max Speed,Gear,Gear Plus"
    cmd = [a.cli, "export", a.project, a.input, "--channels", channels, "--range", "Outings:Laps/2", "--rate", "100",
           "--no-sidecar", "-o", str(out)]
    run = subprocess.run(cmd, capture_output=True, text=True, check=False)
    print(run.stdout, end="")
    if run.returncode != 0:
        return fail(f"exit {run.returncode}: {run.stderr}")
    with out.open(newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    if rows[0] != ["Time", *channels.split(",")]:
        return fail(f"names row {rows[0]}")
    units = dict(zip(rows[0], rows[1]))
    if units["Corr Speed"] != "km/h" or units["Speed Twice"] != "km/h" or units["SPEED"] != "m/s":
        return fail(f"units row {rows[1]}")
    data = [[float(v) if v != "" else None for v in r] for r in rows[2:]]
    if len(data) < 1000:
        return fail(f"only {len(data)} rows")
    col = {name: i for i, name in enumerate(rows[0])}
    speeds = [r[col["SPEED"]] for r in data]
    peak = max(s for s in speeds if s is not None)
    for k, r in enumerate(data):
        t, speed = r[col["Time"]], r[col["SPEED"]]
        if abs(t - k * 0.01) > 1e-6:
            return fail(f"row {k}: time {t}")
        if speed is None:
            return fail(f"row {k}: SPEED invalid")
        if abs(r[col["Speed Twice"]] - 7.2 * speed) > 1e-3 * max(1.0, abs(7.2 * speed)):
            return fail(f"row {k}: Speed Twice {r[col['Speed Twice']]} != 7.2 x {speed}")
        if r[col["Gear Plus"]] != r[col["Gear"]] + 1:
            return fail(f"row {k}: Gear Plus {r[col['Gear Plus']]} != Gear {r[col['Gear']]} + 1")
        if abs(r[col["Lap Max Speed"]] - peak) > 1e-4 * peak:
            return fail(f"row {k}: Lap Max Speed {r[col['Lap Max Speed']]} != {peak}")
        if abs(r[col["Corr Speed"]] - 3.6 * speed) > 0.05 * max(1.0, 3.6 * speed):
            return fail(f"row {k}: Corr Speed {r[col['Corr Speed']]} km/h far from SPEED {speed} m/s")
    # An unknown channel is exit status 1.
    bad = subprocess.run([a.cli, "export", a.project, a.input, "--channels", "No Such Channel", "-o",
                          str(work / "bad.csv")], capture_output=True, text=True, check=False)
    if bad.returncode != 1 or "No Such Channel" not in bad.stderr:
        return fail(f"unknown channel: exit {bad.returncode}, {bad.stderr}")
    # A maths channel whose input fails at evaluation: the failure is reported (E-ARG-VALUE upstream).
    failing = subprocess.run([a.cli, "export", a.project, a.input, "--channels", "Too Fast Plus", "--no-sidecar",
                              "-o", str(work / "failing.csv")], capture_output=True, text=True, check=False)
    if (failing.returncode != 1 or "Calculating" in failing.stderr or
            "'Too Fast Plus' cannot be evaluated: input 'Too Fast'" not in failing.stderr):
        return fail(f"failing maths channel: exit {failing.returncode}, {failing.stderr}")
    print(f"export_check: {len(data)} rows, {len(rows[0]) - 1} channels: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
