#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Keymap coverage lint (AC-M1-06, AC-M3-01; 06_UI_SHELL.md sections 14.3-14.5; 08_REPO_AND_BUILD.md section 5).

Checks data/keymaps/default.json (keymap-1) against the hotkey tests:
  1. Rows: status is implemented, planned or reserved; a planned or implemented row names its milestone (M1, M2,
     M3, M4, v1.0, later), a reserved row none; a row whose milestone is at or before the current milestone is
     implemented, and an implemented row's milestone is not after it; no key sequence is bound twice in one
     context (06 section 14.2 rule 7; sequences compared after the normalisation of
     esses::project::normaliseKeySequence, key names case-insensitively).
  2. Tests (skipped with --rows-only): every command id with an implemented row has a test registered with
     KEYMAP_TEST("<command.id>") under tests/ui/keymap/ (08 section 6.5), and every KEYMAP_TEST names a command
     with an implemented row. Planned rows are exempt until their milestone; reserved rows are covered by the
     generic row of 06 section 14.5.

The current milestone is CURRENT_MILESTONE below (raised when a milestone closes) or --milestone.
Standard library only. `python -I tools/check_keymap_coverage.py [--root DIR] [--milestone M1] [--rows-only]`;
`--self-test` checks the checker on built-in cases. Exit status: 0 clean, 1 findings, 2 usage error.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import tempfile
from typing import Any

CURRENT_MILESTONE = "M1"
MILESTONES = ["M1", "M2", "M3", "M4", "v1.0", "later"]
STATUSES = {"implemented", "planned", "reserved"}
KEYMAP = pathlib.PurePosixPath("data/keymaps/default.json")
TESTS = pathlib.PurePosixPath("tests/ui/keymap")
TEST_RE = re.compile(r'\bKEYMAP_TEST\s*\(\s*"([^"\\]+)"')
MODIFIERS = {"ctrl": "Ctrl", "control": "Ctrl", "alt": "Alt", "shift": "Shift", "meta": "Meta", "cmd": "Meta",
             "win": "Meta"}


def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate key '{key}'")
        out[key] = value
    return out


def normalise_keys(keys: str) -> str:
    """The C++ normaliseKeySequence: modifiers Ctrl, Alt, Shift, Meta first; single letters upper case."""
    parts: list[str] = []
    current = ""
    for i, c in enumerate(keys):
        if c == "+" and current and i + 1 < len(keys):
            parts.append(current)
            current = ""
        else:
            current += c
    parts.append(current)
    mods: set[str] = set()
    key = ""
    for i, part in enumerate(parts):
        folded = part.lower()
        if i + 1 < len(parts) and folded in MODIFIERS:
            mods.add(MODIFIERS[folded])
        else:
            key = part
    if len(key) == 1 and "a" <= key <= "z":
        key = key.upper()
    return "".join(m + "+" for m in ("Ctrl", "Alt", "Shift", "Meta") if m in mods) + key


def check_rows(keymap: Any, current: str) -> tuple[list[str], set[str]]:
    """Returns (findings, command ids with an implemented row)."""
    findings: list[str] = []
    implemented: set[str] = set()
    rows = keymap.get("bindings") if isinstance(keymap, dict) else None
    if not isinstance(rows, list):
        return [f"{KEYMAP}: no bindings array"], implemented
    rank = {m: i for i, m in enumerate(MILESTONES)}
    seen: dict[tuple[str, str], int] = {}  # (context, normalised keys) -> first row index
    for i, row in enumerate(rows):
        where = f"{KEYMAP} /bindings/{i}"
        if not isinstance(row, dict) or not isinstance(row.get("command"), str) \
                or not isinstance(row.get("context"), str) or not isinstance(row.get("keys"), list):
            findings.append(f"{where}: a row has command, context and keys")
            continue
        command, context = row["command"], row["context"]
        what = f"{where} ({command}, {context})"
        status = row.get("status")
        milestone = row.get("milestone")
        if status not in STATUSES:
            findings.append(f"{what}: status {status!r} is not implemented, planned or reserved")
            continue
        if status == "reserved":
            if milestone is not None:
                findings.append(f"{what}: a reserved row names no milestone")
        elif milestone not in rank:
            findings.append(f"{what}: milestone {milestone!r} is not one of {', '.join(MILESTONES)}")
        else:
            due = rank[milestone] <= rank[current]
            if due and status != "implemented":
                findings.append(f"{what}: milestone {milestone} is at or before {current} but the row is {status}")
            if status == "implemented" and not due:
                findings.append(f"{what}: implemented, but its milestone {milestone} is after {current}")
            if status == "implemented":
                implemented.add(command)
        for key in row["keys"]:
            if not isinstance(key, str) or not key:
                findings.append(f"{what}: a key sequence is a non-empty string")
                continue
            norm = normalise_keys(key)
            first = seen.setdefault((context, norm.lower()), i)  # key names compare case-insensitively
            if first != i:
                findings.append(f"{what}: {norm} is already bound in context {context} by "
                                f"{rows[first]['command']} (/bindings/{first})")
    return findings, implemented


def collect_tests(root: pathlib.Path) -> tuple[dict[str, list[str]], list[str]]:
    """KEYMAP_TEST ids -> files; findings."""
    tests: dict[str, list[str]] = {}
    folder = root / TESTS
    if not folder.is_dir():
        return tests, [f"{TESTS}: missing (the hotkey tests of AC-M1-06 live here)"]
    for path in sorted(folder.rglob("*")):
        if path.suffix not in {".cpp", ".hpp", ".h"} or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in TEST_RE.finditer(text):
            tests.setdefault(m.group(1), []).append(path.relative_to(root).as_posix())
    return tests, []


def check(root: pathlib.Path, current: str, rows_only: bool) -> list[str]:
    path = root / KEYMAP
    try:
        keymap = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_no_duplicates)
    except (OSError, ValueError) as exc:
        return [f"{KEYMAP}: {exc}"]
    findings, implemented = check_rows(keymap, current)
    if rows_only:
        return findings
    tests, test_findings = collect_tests(root)
    findings += test_findings
    if test_findings:
        return findings
    for command in sorted(implemented - set(tests)):
        findings.append(f"{command}: implemented in {KEYMAP} but no KEYMAP_TEST(\"{command}\") under {TESTS}")
    rows = {r.get("command") for r in keymap.get("bindings", []) if isinstance(r, dict)}
    for command in sorted(set(tests) - implemented):
        where = ", ".join(tests[command])
        if command in rows:
            findings.append(f"{where}: KEYMAP_TEST(\"{command}\") tests a row that is not implemented")
        else:
            findings.append(f"{where}: KEYMAP_TEST(\"{command}\") names no row of {KEYMAP}")
    return findings


# ---- self-test ----------------------------------------------------------------------------------------------
def self_test() -> int:
    failures: list[str] = []

    def row(command: str, keys: list[str], context: str, status: str, milestone: str | None = None) -> dict:
        r: dict[str, Any] = {"command": command, "keys": keys, "context": context, "status": status}
        if milestone is not None:
            r["milestone"] = milestone
        return r

    good_rows = [row("data.toggle_domain", ["F9"], "global", "implemented", "M1"),
                 row("zoom.in", ["Up"], "graph", "implemented", "M1"),
                 row("zoom.in", ["Up"], "summary", "planned", "M3"),
                 row("file.print", ["Ctrl+P"], "global", "planned", "v1.0"),
                 row("file.unload_device", ["Ctrl+F8"], "global", "reserved")]
    good_tests = 'KEYMAP_TEST("data.toggle_domain") {}\nKEYMAP_TEST( "zoom.in" ) {}\n'

    def run(rows: list[dict], tests: str | None, current: str = "M1", rows_only: bool = False) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / KEYMAP).parent.mkdir(parents=True)
            (root / KEYMAP).write_text(json.dumps({"$schema": "urn:esses:keymap-1", "schemaVersion": 1,
                                                   "name": "default", "inherits": None, "bindings": rows,
                                                   "unbind": []}), encoding="utf-8")
            if tests is not None:
                (root / TESTS).mkdir(parents=True)
                (root / TESTS / "data_tests.cpp").write_text(tests, encoding="utf-8")
            return check(root, current, rows_only)

    if run(good_rows, good_tests):
        failures.append(f"good keymap: {run(good_rows, good_tests)}")
    cases: list[tuple[str, list[dict], str | None, str, bool, str]] = [
        ("missing test", good_rows, 'KEYMAP_TEST("zoom.in")', "M1", False, "no KEYMAP_TEST(\"data.toggle_domain\")"),
        ("test without row", good_rows, good_tests + 'KEYMAP_TEST("zoom.sideways")', "M1", False, "names no row"),
        ("test of planned row", good_rows, good_tests + 'KEYMAP_TEST("file.print")', "M1", False,
         "not implemented"),
        ("due but planned", good_rows + [row("pan.left", ["Shift+Left"], "graph", "planned", "M1")], good_tests,
         "M1", False, "at or before M1 but the row is planned"),
        ("implemented too early", good_rows + [row("pan.left", ["Shift+Left"], "graph", "implemented", "M2")],
         good_tests + 'KEYMAP_TEST("pan.left")', "M1", False, "milestone M2 is after M1"),
        ("bad status", good_rows + [row("pan.left", ["B"], "graph", "done", "M2")], good_tests, "M1", False,
         "status 'done'"),
        ("no milestone", good_rows + [row("pan.left", ["B"], "graph", "planned")], good_tests, "M1", False,
         "milestone None"),
        ("reserved with milestone", good_rows + [row("tools.setup", ["Ctrl+T"], "global", "reserved", "M2")],
         good_tests, "M1", False, "a reserved row names no milestone"),
        ("conflict", good_rows + [row("view.other", ["f9"], "global", "planned", "M2")], good_tests, "M1", False,
         "f9 is already bound in context global by data.toggle_domain"),
        ("conflict after normalising", good_rows + [row("a.x", ["shift+ctrl+c"], "graph", "planned", "M3"),
                                                    row("a.y", ["Ctrl+Shift+C"], "graph", "planned", "M3")],
         good_tests, "M1", False, "Ctrl+Shift+C is already bound"),
        ("missing tests dir", good_rows, None, "M1", False, "missing"),
        ("milestone moves on", good_rows, good_tests, "M3", False, "(zoom.in, summary): milestone M3 is at or"),
    ]
    for label, rows, tests, current, rows_only, needle in cases:
        found = run(rows, tests, current, rows_only)
        if not any(needle in f for f in found):
            failures.append(f"{label}: expected '{needle}' in {found}")
    # --rows-only ignores the tests but not the rows.
    if run(good_rows, None, rows_only=True):
        failures.append("rows-only with no tests directory reported findings")
    if not run(good_rows + [row("pan.left", ["B"], "graph", "planned", "M1")], None, rows_only=True):
        failures.append("rows-only missed a due planned row")
    # Same key in different contexts is allowed (Delete: component.delete vs data.close_file).
    shared = good_rows + [row("component.delete", ["Delete"], "worksheet", "planned", "M3"),
                          row("data.close_file", ["Delete"], "data_window", "planned", "M2")]
    if run(shared, good_tests):
        failures.append(f"same key in two contexts: {run(shared, good_tests)}")
    for raw, norm in [("shift+ctrl+c", "Ctrl+Shift+C"), ("Ctrl++", "Ctrl++"), ("+", "+"), ("F9", "F9"),
                      ("Meta+Alt+x", "Alt+Meta+X")]:
        if normalise_keys(raw) != norm:
            failures.append(f"normalise_keys({raw!r}) = {normalise_keys(raw)!r}, not {norm!r}")
    for failure in failures:
        print("check_keymap_coverage self-test: " + failure)
    print("check_keymap_coverage self-test: " + ("passed" if not failures else "FAILED"))
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=pathlib.Path(__file__).resolve().parent.parent, type=pathlib.Path)
    parser.add_argument("--milestone", default=CURRENT_MILESTONE, choices=MILESTONES)
    parser.add_argument("--rows-only", action="store_true", help="check the rows, not the tests")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not (args.root / KEYMAP).is_file():
        print(f"check_keymap_coverage: {args.root / KEYMAP} not found", file=sys.stderr)
        return 2
    findings = check(args.root, args.milestone, args.rows_only)
    for f in findings:
        print(f)
    scope = "rows" if args.rows_only else "rows and tests"
    print(f"check_keymap_coverage ({scope}, milestone {args.milestone}): {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
