#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""AC-M2-01: esses-cli maths check over a corpus of maths-1 modules, reported as counts only.

The corpus is the user's real i2 maths, pasted as plain-text expressions into maths-1 modules
(tests/maths/user_modules/, git-ignored and checked by the lint job's forbidden-paths step). The report holds
counts only (modules, items per status, parse errors, the number of items per diagnostic code), never an item
name or an expression, so it may be committed as the milestone evidence (docs/milestones/M2.md).

  python -I tools/maths_corpus_check.py --cli <build>/bin/esses-cli --corpus tests/maths/user_modules
         [--log <file>] [--compat-doc docs/maths-compat.md] [--threshold 0.95] [--if-present] [--json]

Pass (exit 0): no parse error, at least --threshold of the enabled items Ok or with warnings only, and (with
--compat-doc) every E-* code that remains is listed in that document. Exit 1 otherwise; exit 2 on a usage or
environment error. With --if-present a missing corpus directory is reported as skipped (exit 0).
Standard library only.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cli", required=True, help="path of esses-cli")
    ap.add_argument("--corpus", required=True, help="directory of *.maths.json modules")
    ap.add_argument("--log", help="resolve names against this log (else without data)")
    ap.add_argument("--compat-doc", help="docs/maths-compat.md: every remaining E-* code must be listed there")
    ap.add_argument("--threshold", type=float, default=0.95, help="share of enabled items Ok or Warning")
    ap.add_argument("--if-present", action="store_true", help="a missing corpus is skipped, not an error")
    ap.add_argument("--json", action="store_true", help="print the counts as JSON")
    a = ap.parse_args(argv)

    corpus = pathlib.Path(a.corpus)
    if not corpus.is_dir():
        if a.if_present:
            print(f"maths_corpus_check: skipped (no corpus at {corpus})")
            return 0
        print(f"maths_corpus_check: no corpus directory {corpus}", file=sys.stderr)
        return 2
    # An absolute path: Windows does not run a relative path that contains a directory part, such as
    # build/<dir>/bin/esses-cli.exe, unless it is resolved first.
    cli = pathlib.Path(a.cli)
    if not cli.is_absolute():
        cli = cli.resolve()
    cmd = [str(cli), "maths", "check", str(corpus), "--json"]
    if a.log:
        cmd += ["--log", a.log]
    try:
        run = subprocess.run(cmd, capture_output=True, check=False)
    except OSError as e:
        print(f"maths_corpus_check: cannot run {a.cli}: {e}", file=sys.stderr)
        return 2
    if run.returncode not in (0, 1):
        print(f"maths_corpus_check: esses-cli failed ({run.returncode}): {run.stderr.decode(errors='replace')}",
              file=sys.stderr)
        return 2
    try:
        report = json.loads(run.stdout.decode("utf-8"))
    except ValueError as e:
        print(f"maths_corpus_check: esses-cli did not print JSON: {e}", file=sys.stderr)
        return 2

    summary = report["summary"]
    # Items per code: an item counts once per code it carries.
    items_per_code: dict[str, int] = {}
    for module in report["modules"]:
        for item in module.get("items", []):
            for code in sorted({d["code"] for d in item["diagnostics"]}):
                items_per_code[code] = items_per_code.get(code, 0) + 1
    enabled = summary["items"] - summary["disabled"]
    usable = summary["ok"] + summary["warning"]
    share = usable / enabled if enabled > 0 else 1.0
    error_codes = sorted(c for c in items_per_code if c.startswith("E-"))
    unlisted: list[str] = []
    if a.compat_doc:
        text = pathlib.Path(a.compat_doc).read_text(encoding="utf-8")
        unlisted = [c for c in error_codes if c not in text]
    counts = {
        "modules": summary["modules"],
        "moduleErrors": summary["moduleErrors"],
        "items": summary["items"],
        "enabled": enabled,
        "ok": summary["ok"],
        "warning": summary["warning"],
        "error": summary["error"],
        "disabled": summary["disabled"],
        "parseErrors": summary["parseErrors"],
        "okOrWarningShare": round(share, 4),
        "itemsPerCode": items_per_code,
        "errorCodesNotInCompatDoc": unlisted,
    }
    passed = (summary["parseErrors"] == 0 and summary["moduleErrors"] == 0 and share >= a.threshold
              and not unlisted)
    if a.json:
        print(json.dumps({"counts": counts, "pass": passed}, indent=2, sort_keys=True))
    else:
        print(f"maths_corpus_check: {summary['modules']} modules, {summary['items']} items "
              f"({enabled} enabled): {summary['ok']} ok, {summary['warning']} warning, {summary['error']} error, "
              f"{summary['parseErrors']} parse errors; ok or warning {share:.1%} (threshold {a.threshold:.0%})")
        for code in sorted(items_per_code):
            print(f"  {code}: {items_per_code[code]} items")
        if unlisted:
            print(f"  E-* codes not listed in {a.compat_doc}: {', '.join(unlisted)}")
        print("PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
