#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Import Log codes documented (AC-M0-12, 08 section 1): every code the Esses readers emit on the corpus appears
in docs/formats/ld.md (the .ld/.ldx reader and the open pipeline) or docs/formats/text.md (the text engine, M2).

Codes are collected from
  1. warnings[].code of every expected.probe.json under fixtures/: the public synthetic families and, when the
     private checkout is present, the user's real files (fixtures/private, kept current by fixture verify);
  2. with --cli <esses-cli>: the live `esses-cli info --json` of every .ld file listed in
     fixtures/private/s1/corpus.json that exists on this machine (the user's real files, spike S-1).
A code counts as documented when docs/formats/ld.md or docs/formats/text.md contains it in backticks
(`ld.header_only`, `text.auto_delimiter`).

Standard library only. `python -I tools/check_importlog_codes.py --root <repo> [--cli <esses-cli>]`;
`--self-test` checks the checker. Exit status: 0 clean, 1 findings, 2 usage or tool error.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile

CODE_RE = re.compile(r"`((?:ld|ldx|open|sidecar|companion|model|detail|text)\.[a-z0-9_]+)`")
DOCS = ("ld.md", "text.md")


def documented(doc: pathlib.Path) -> set[str]:
    return set(CODE_RE.findall(doc.read_text(encoding="utf-8"))) if doc.exists() else set()


def codes_of_probe(text: str) -> set[str]:
    doc = json.loads(text)
    return {w["code"] for w in doc.get("warnings", []) if isinstance(w, dict) and "code" in w}


def collect(root: pathlib.Path, cli: str | None) -> tuple[dict[str, set[str]], int, int]:
    """code -> where it was seen; number of probe files read; number of live probes."""
    seen: dict[str, set[str]] = {}
    files = 0
    for p in sorted((root / "fixtures").rglob("expected.probe.json")):
        try:
            for c in codes_of_probe(p.read_text(encoding="utf-8")):
                seen.setdefault(c, set()).add(p.parent.name)
        except (OSError, ValueError) as e:
            raise RuntimeError(f"{p}: {e}") from e
        files += 1
    live = 0
    corpus = root / "fixtures" / "private" / "s1" / "corpus.json"
    if cli and corpus.exists():
        for e in json.loads(corpus.read_text(encoding="utf-8")):
            path = pathlib.Path(e.get("path", ""))
            if e.get("ext") != ".ld" or not path.exists():
                continue
            r = subprocess.run([cli, "info", str(path), "--json"], capture_output=True)
            if r.returncode != 0:
                raise RuntimeError(f"esses-cli info {path} failed: {r.stderr.decode('utf-8', 'replace')}")
            for c in codes_of_probe(r.stdout.decode("utf-8")):
                seen.setdefault(c, set()).add(path.name)
            live += 1
    return seen, files, live


def check(root: pathlib.Path, cli: str | None) -> tuple[list[str], str]:
    known: set[str] = set()
    for name in DOCS:
        known |= documented(root / "docs" / "formats" / name)
    seen, files, live = collect(root, cli)
    findings = []
    for code in sorted(seen):
        if code not in known:
            where = sorted(seen[code])
            findings.append(f"{code} is emitted ({', '.join(where[:3])}{' ...' if len(where) > 3 else ''}) "
                            f"but not documented in docs/formats/ld.md or docs/formats/text.md")
    summary = (f"check_importlog_codes: {len(seen)} code(s) emitted over {files} expected.probe.json file(s) and "
               f"{live} live probe(s); {len(known)} documented; {len(findings)} finding(s)")
    return findings, summary


def self_test() -> int:
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        (root / "docs" / "formats").mkdir(parents=True)
        (root / "docs" / "formats" / "ld.md").write_text("| `ld.known` | info |\nprose ld.unquoted\n",
                                                         encoding="utf-8")
        fx = root / "fixtures" / "motec.ld" / "a"
        fx.mkdir(parents=True)
        probe = {"warnings": [{"code": "ld.known"}, {"code": "ld.unquoted"}, {"code": "ldx.missing_doc"}]}
        (fx / "expected.probe.json").write_text(json.dumps(probe), encoding="utf-8")
        findings, _ = check(root, None)
        got = {f.split(" ", 1)[0] for f in findings}
        if got != {"ld.unquoted", "ldx.missing_doc"}:
            failures.append(f"expected two undocumented codes, got {sorted(got)}")
        (root / "docs" / "formats" / "ld.md").write_text("`ld.known` `ld.unquoted` `ldx.missing_doc`\n",
                                                         encoding="utf-8")
        findings, _ = check(root, None)
        if findings:
            failures.append(f"clean tree reported {findings}")
        # Text-engine codes are documented in docs/formats/text.md.
        tx = root / "fixtures" / "generic.csv" / "b"
        tx.mkdir(parents=True)
        probe = {"warnings": [{"code": "text.known"}, {"code": "text.missing_doc"}]}
        (tx / "expected.probe.json").write_text(json.dumps(probe), encoding="utf-8")
        (root / "docs" / "formats" / "text.md").write_text("| `text.known` | info |\n", encoding="utf-8")
        findings, _ = check(root, None)
        got = {f.split(" ", 1)[0] for f in findings}
        if got != {"text.missing_doc"}:
            failures.append(f"expected one undocumented text code, got {sorted(got)}")
    for f in failures:
        print("self-test:", f)
    print(f"check_importlog_codes self-test: {'FAILED' if failures else 'ok'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--cli", help="esses-cli: also probe the user's real files live (fixtures/private/s1)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    root = pathlib.Path(a.root).resolve()
    if not (root / "docs" / "formats" / "ld.md").exists():
        print("check_importlog_codes: docs/formats/ld.md not found (--root)")
        return 2
    cli = str(pathlib.Path(a.cli).resolve()) if a.cli else None
    try:
        findings, summary = check(root, cli)
    except RuntimeError as e:
        print(f"check_importlog_codes: {e}")
        return 2
    for f in findings:
        print(f)
    print(summary)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
