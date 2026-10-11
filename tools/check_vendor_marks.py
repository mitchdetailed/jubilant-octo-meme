#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Vendor-mark lint (D-003, D-125; 00_OVERVIEW.md section 8; 08_REPO_AND_BUILD.md section 5).

Two scans, both outside the allow-list (tools/vendor_marks_allowlist.txt):
  1. Words: (?i)\\bmotec\\b | \\bi2\\b | i2pro | \\bi2w(kb|sp)\\b in string literals, comments, JSON values, and
     file and asset names under libs/ apps/ plugins/ sdk/ data/ schemas/ tests/ui/ tools/packaging/.
  2. Identifiers (C/C++, CMake, Python, JSON keys) and file stems under libs/ apps/ plugins/ sdk/: a
     case-insensitive substring "motec", or a token equal to i2, i2pro, i2wkb, i2wsp or mtc after splitting at
     '_', '-', '.' and case changes (I2MathsImporter, xI2 and IMtcMathPlugin are caught; kPi2 is not).

Standard library only. Run with `python -I tools/check_vendor_marks.py [--root DIR]`; `--self-test` checks the
scanner itself. Exit status: 0 clean, 1 findings, 2 usage error.
"""
from __future__ import annotations

import argparse
import fnmatch
import os
import pathlib
import re
import subprocess
import sys
import tempfile

WORD_RE = re.compile(r"(?i)\bmotec\b|\bi2\b|i2pro|\bi2w(?:kb|sp)\b")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
TOKEN_RE = re.compile(r"[A-Z]+[0-9]*(?=[A-Z][a-z])|[A-Z]?[a-z]+(?:[0-9]+[a-z]*)*|[A-Z]+[0-9]*|[0-9]+")
BAD_TOKENS = {"i2", "i2pro", "i2wkb", "i2wsp", "mtc"}
WORD_SCOPE = ("libs/", "apps/", "plugins/", "sdk/", "data/", "schemas/", "tests/ui/", "tools/packaging/")
IDENT_SCOPE = ("libs/", "apps/", "plugins/", "sdk/")
CPP_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}
TEXT_EXT = CPP_EXT | {".in", ".cmake", ".txt", ".py", ".json", ".md", ".yml", ".yaml", ".toml", ".ini", ".qrc",
                      ".ui", ".xml", ".svg", ".css", ".qss", ".lua", ".csv"}
SKIP_DIRS = {".git", "build", "out", "third_party", "__pycache__", ".vs", "node_modules"}


class Allowlist:
    def __init__(self) -> None:
        self.literals: list[str] = []
        self.identifiers: set[str] = set()
        self.paths: list[str] = []

    @classmethod
    def load(cls, path: pathlib.Path) -> "Allowlist":
        allow = cls()
        for raw in path.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            kind, _, value = line.partition(" ")
            value = value.strip()
            if kind == "literal":
                allow.literals.append(value)
            elif kind == "ident":
                allow.identifiers.add(value)
            elif kind == "path":
                allow.paths.append(value)
            else:
                raise ValueError(f"{path}: unknown allow-list entry '{line}'")
        allow.literals.sort(key=len, reverse=True)
        return allow

    def path_allowed(self, rel: str) -> bool:
        return any(fnmatch.fnmatchcase(rel, pattern) for pattern in self.paths)

    def strip_literals(self, text: str) -> str:
        for lit in self.literals:
            text = text.replace(lit, " " * len(lit))
        return text


def split_identifier(ident: str) -> list[str]:
    tokens: list[str] = []
    for part in re.split(r"[_\-.]+", ident):
        tokens.extend(TOKEN_RE.findall(part))
    return tokens


def identifier_is_bad(ident: str) -> bool:
    if "motec" in ident.lower():
        return True
    return any(tok.lower() in BAD_TOKENS for tok in split_identifier(ident))


def split_cpp(text: str) -> tuple[str, str]:
    """Splits C/C++ source into (code, prose). Prose holds comments and literal contents; both keep line breaks
    so line numbers stay valid."""
    code: list[str] = []
    prose: list[str] = []
    i, n = 0, len(text)

    def emit(ch: str, to_code: bool) -> None:
        if ch == "\n":
            code.append("\n")
            prose.append("\n")
        elif to_code:
            code.append(ch)
            prose.append(" ")
        else:
            code.append(" ")
            prose.append(ch)

    while i < n:
        c = text[i]
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                emit(text[i], False)
                i += 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for ch in text[i:end]:
                emit(ch, False)
            i = end
        elif c == "R" and i + 1 < n and text[i + 1] == '"' and (i == 0 or not (text[i - 1].isalnum() and text[i - 1] not in "uUL8")):
            m = re.match(r'R"([^ ()\\\t\n]{0,16})\(', text[i:])
            if not m:
                emit(c, True)
                i += 1
                continue
            close = ")" + m.group(1) + '"'
            end = text.find(close, i + m.end())
            end = n if end < 0 else end + len(close)
            for ch in text[i:end]:
                emit(ch, False)
            i = end
        elif c == "'" and 0 < i < n - 1 and text[i - 1].isalnum() and text[i + 1].isalnum() and not (
                text[i - 1] in "LuU8" and (i < 2 or not text[i - 2].isalnum())):
            emit(c, True)  # C++14 digit separator, as in 1'000'000
            i += 1
        elif c in "\"'":
            quote = c
            emit(c, True)
            i += 1
            while i < n and text[i] != quote and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n:
                    emit(text[i], False)
                    i += 1
                emit(text[i], False)
                i += 1
            if i < n and text[i] == quote:
                emit(quote, True)
                i += 1
        else:
            emit(c, True)
            i += 1
    return "".join(code), "".join(prose)


def list_files(root: pathlib.Path) -> list[str]:
    try:
        out = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root,
                             capture_output=True, check=True)
        files = [f for f in out.stdout.decode("utf-8", "replace").split("\0") if f]
        if files:
            return sorted(set(files))
    except (OSError, subprocess.CalledProcessError):
        pass
    result = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            result.append(pathlib.Path(dirpath, name).relative_to(root).as_posix())
    return sorted(result)


def scan(root: pathlib.Path, allow: Allowlist, files: list[str] | None = None) -> list[str]:
    findings: list[str] = []
    for rel in files if files is not None else list_files(root):
        rel = rel.replace("\\", "/")
        if any(part in SKIP_DIRS for part in rel.split("/")[:-1]):
            continue
        in_words = rel.startswith(WORD_SCOPE)
        in_idents = rel.startswith(IDENT_SCOPE)
        if not (in_words or in_idents) or allow.path_allowed(rel):
            continue
        # File and asset names.
        name_text = allow.strip_literals(rel)
        for m in WORD_RE.finditer(name_text):
            findings.append(f"{rel}: file name contains '{m.group(0)}'")
        if in_idents:
            stem = pathlib.PurePosixPath(rel).name.split(".")[0]
            if stem not in allow.identifiers and identifier_is_bad(stem):
                findings.append(f"{rel}: file name token '{stem}'")
        path = root / rel
        if path.suffix.lower() not in TEXT_EXT and path.name != "CMakeLists.txt":
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        text = allow.strip_literals(text)
        if path.suffix.lower() in CPP_EXT:
            code, prose = split_cpp(text)
        else:
            code, prose = text, text
        if in_words:
            for lineno, line in enumerate(prose.splitlines(), 1):
                for m in WORD_RE.finditer(line):
                    findings.append(f"{rel}:{lineno}: word '{m.group(0)}'")
        if in_idents:
            for lineno, line in enumerate(code.splitlines(), 1):
                for m in IDENT_RE.finditer(line):
                    ident = m.group(0)
                    if ident not in allow.identifiers and identifier_is_bad(ident):
                        findings.append(f"{rel}:{lineno}: identifier '{ident}'")
    return findings


def self_test(allowlist_path: pathlib.Path) -> int:
    allow = Allowlist.load(allowlist_path)
    cases = {
        "libs/x/src/a.cpp": (
            "// Copyright\n"
            "class I2MathsImporter {};\n"            # bad identifier
            "int xI2 = 0;\n"                        # allow-list class (d)
            "struct IMtcMathPlugin {};\n"           # bad identifier
            "constexpr double kPi2 = 6.28;\n"       # fine
            "// reads MoTeC files\n"                # bad word in a comment
            "const char* id = \"motec.ld\";\n"      # allow-list class (a)
            "int i2 = 0;\n"                         # bad identifier
            "const char* s = \"an i2 workbook\";\n" # bad word in a string
            "const char* r = R\"x(MoTeC)x\";\n"     # bad word in a raw string
            "constexpr int kBig = 1'000'000; int i2c = 0;\n"  # digit separators; i2c is one token
        ),
        "libs/y/I2Thing.hpp": "#pragma once\n",     # bad file name token
        "data/foo.json": "{\"name\": \"i2pro export\"}\n",
        "data/dictionaries/motec.ld.json": "{}\n",  # allowed importer id in a file name
        "docs/notes.md": "MoTeC i2 prose is fine here\n",
        "tools/other.py": "i2 = 'MoTeC'  # out of scope\n",
    }
    expected = {
        "libs/x/src/a.cpp:2: identifier 'I2MathsImporter'",
        "libs/x/src/a.cpp:4: identifier 'IMtcMathPlugin'",
        "libs/x/src/a.cpp:6: word 'MoTeC'",
        "libs/x/src/a.cpp:8: identifier 'i2'",
        "libs/x/src/a.cpp:9: word 'i2'",
        "libs/x/src/a.cpp:10: word 'MoTeC'",
        "libs/y/I2Thing.hpp: file name token 'I2Thing'",
        "data/foo.json:1: word 'i2pro'",
    }
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        for rel, content in cases.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content, encoding="utf-8", newline="\n")
        found = set(scan(root, allow, sorted(cases)))
    ok = found == expected
    for item in sorted(expected - found):
        print(f"self-test: missed {item}")
    for item in sorted(found - expected):
        print(f"self-test: unexpected {item}")
    assert split_identifier("I2MathsImporter")[0] == "I2"
    assert split_identifier("kPi2") == ["k", "Pi2"]
    assert split_identifier("i2c") == ["i2c"] and split_identifier("xI2") == ["x", "I2"]
    assert split_identifier("IMtcMathPlugin")[:2] == ["I", "Mtc"]
    print("check_vendor_marks self-test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=here.parent, help="repository root")
    parser.add_argument("--allowlist", type=pathlib.Path, default=here / "vendor_marks_allowlist.txt")
    parser.add_argument("--self-test", action="store_true", help="test the scanner on built-in cases")
    args = parser.parse_args()
    if args.self_test:
        return self_test(args.allowlist)
    findings = scan(args.root.resolve(), Allowlist.load(args.allowlist))
    for f in findings:
        print(f)
    print(f"check_vendor_marks: {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
