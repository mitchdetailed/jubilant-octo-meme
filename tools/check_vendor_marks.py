#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Vendor-mark lint (D-003, D-125; 00_OVERVIEW.md section 8; 08_REPO_AND_BUILD.md section 5).

Two scans of the source tree, both outside the allow-list (tools/vendor_marks_allowlist.txt):
  1. Words: (?i)\\bmotec\\b | \\bi2\\b | i2pro | \\bi2w(kb|sp)\\b in string literals, comments, JSON values, and
     file and asset names under libs/ apps/ plugins/ sdk/ data/ schemas/ tests/ui/ tools/packaging/ (the
     installer script, the desktop entry, the Info.plist template, the icons) and in the package workflow.
  2. Identifiers (C/C++, CMake, Python, JSON keys) and file stems under libs/ apps/ plugins/ sdk/: a
     case-insensitive substring "motec", or a token equal to i2, i2pro, i2wkb, i2wsp or mtc after splitting at
     '_', '-', '.' and case changes (I2MathsImporter, xI2 and IMtcMathPlugin are caught; kPi2 is not).
--tree DIR scans a release tree instead (AC-M4-08, D-212; tools/packaging/package.py runs it on every staged
package before it is archived): the words in every file and folder name, in every text file (by extension, and
files without one that read as UTF-8 text, such as AppRun) and in the strings of our own programs (esses,
esses-cli, esses-render; for binaries only the long marks, motec, i2pro and i2wkb/i2wsp, since two printable
bytes "i2" occur by chance). Third-party binaries (Qt, the docking library, the C++ runtime) are checked by name
only. The allow-list's literals and path patterns apply as in the source tree; in a binary also a prefix of a
literal that is a whole number of 8-byte words and is cut off there (an optimiser stores a short literal as
immediates: GCC and Apple Clang at -O2 write "motec.csv" as the 8-byte word "motec.cs", an opcode, then "v", so
the printable run reads "motec.csH"; D-217).

Standard library only. Run with `python -I tools/check_vendor_marks.py [--root DIR | --tree DIR]`; `--self-test`
checks the scanner itself. Exit status: 0 clean, 1 findings, 2 usage error.
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
WORD_SCOPE = ("libs/", "apps/", "plugins/", "sdk/", "data/", "schemas/", "tests/ui/", "tools/packaging/",
              ".github/workflows/package.yml")
IDENT_SCOPE = ("libs/", "apps/", "plugins/", "sdk/")
CPP_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}
TEXT_EXT = CPP_EXT | {".in", ".cmake", ".txt", ".py", ".json", ".md", ".yml", ".yaml", ".toml", ".ini", ".qrc",
                      ".ui", ".xml", ".svg", ".css", ".qss", ".lua", ".csv",
                      # release metadata (AC-M4-08): installer, Windows resources, desktop entry, bundle, scripts
                      ".iss", ".isl", ".rc", ".manifest", ".desktop", ".plist", ".entitlements", ".sh", ".ps1",
                      ".cmd", ".bat", ".conf", ".appdata", ".metainfo"}
# --tree: the long marks only in binaries; our own programs' file names.
BINARY_RE = re.compile(rb"(?i)\bmotec\b|i2pro|\bi2w(?:kb|sp)\b")
OUR_PROGRAM = re.compile(r"^esses(-cli|-render)?(\.exe)?$")
STRINGS_RE = re.compile(rb"[\x20-\x7e]{4,}|(?:[\x20-\x7e]\x00){4,}")
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


def binary_strings(data: bytes) -> list[bytes]:
    """The printable runs of at least four characters of a binary, ASCII and UTF-16LE (as `strings -e S -e l`)."""
    return [m.group(0).replace(b"\x00", b"") if b"\x00" in m.group(0) else m.group(0)
            for m in STRINGS_RE.finditer(data)]


def strip_binary_literals(run: bytes, literals: list[bytes]) -> bytes:
    """`run` with the allow-listed literals blanked, and the prefixes of a literal that an immediate store cut
    short: a prefix of 8, 16, ... bytes (whole 64-bit words) of a longer literal that the literal's next byte does
    not follow (D-217). A prefix of another length, and a mark outside every literal, stay."""
    for lit in literals:
        run = run.replace(lit, b" " * len(lit))
    for lit in literals:
        for n in range(8, len(lit), 8):
            prefix = lit[:n]
            start = run.find(prefix)
            while start >= 0:
                end = start + n
                if end >= len(run) or run[end] != lit[n]:
                    run = run[:start] + b" " * n + run[end:]
                start = run.find(prefix, start + 1)
    return run


def scan_tree(tree: pathlib.Path, allow: Allowlist) -> list[str]:
    """A release tree (AC-M4-08): names, text files, the strings of our programs."""
    findings: list[str] = []
    literals = [lit.encode("utf-8") for lit in allow.literals]
    for path in sorted(tree.rglob("*"), key=lambda p: p.as_posix()):
        rel = path.relative_to(tree).as_posix()
        name_text = allow.strip_literals(rel)
        for m in WORD_RE.finditer(name_text):
            findings.append(f"{rel}: file name contains '{m.group(0)}'")
        if not path.is_file() or path.is_symlink() or allow.path_allowed(rel):
            continue
        suffix = path.suffix.lower()
        try:
            data = path.read_bytes()
        except OSError:
            continue
        text = None
        if suffix in TEXT_EXT or path.name == "CMakeLists.txt":
            text = data.decode("utf-8", errors="replace")
        elif suffix == "" and len(data) < (1 << 20) and b"\x00" not in data:
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError:
                text = None
        if text is not None:
            for lineno, line in enumerate(allow.strip_literals(text).splitlines(), 1):
                for m in WORD_RE.finditer(line):
                    findings.append(f"{rel}:{lineno}: word '{m.group(0)}'")
        elif OUR_PROGRAM.match(path.name):
            for run in binary_strings(data):
                run = strip_binary_literals(run, literals)
                for m in BINARY_RE.finditer(run):
                    findings.append(f"{rel}: string '{run.decode('ascii', 'replace')[:80]}' contains "
                                    f"'{m.group(0).decode('ascii', 'replace')}'")
    return findings


def self_test_tree(allow: Allowlist) -> bool:
    files = {
        "esses.exe": b"MZ\x00\x00motec.ld\x00MoTeC CSV File\x00\x00ordinary string\x00",  # allowed literals only
        "esses-render.exe": b"\x00\x00a MoTeC export\x00",                                 # a mark in our program
        "esses-cli": "workbook i2wkb".encode("utf-16-le"),                                  # UTF-16 strings too
        "Qt6Core.dll": b"\x00MoTeC\x00",                                                    # third party: name only
        "share/esses/NOTICES.md": "the MoTeC i2 formats\n",                                 # allowed prose
        "share/applications/esses.desktop": "[Desktop Entry]\nName=Esses for i2 users\n",   # a mark in metadata
        "Contents/Info.plist": "<string>Esses</string>\n",                                  # clean
        "AppRun": "#!/bin/sh\n# runs i2pro\n",                                              # text without extension
        "icons/motec.png": b"\x89PNG",                                                       # a mark in a name
        # Release builds (D-217): "motec.csv" and "MoTeC CSV File" stored as 8-byte immediates, cut after a word
        "bin/esses-cli": b"\x00motec.csH\x89\x00motec.csH3\x00MoTeC CSH\x8b\x00motec.ldH\x00",
        # ... but not a cut of another length, and not the mark itself
        "bin/esses": b"\x00motec.lH\x89\x00a motec tool\x00",
    }
    expected = {
        "bin/esses: string 'motec.lH' contains 'motec'",
        "bin/esses: string 'a motec tool' contains 'motec'",
        "esses-render.exe: string 'a MoTeC export' contains 'MoTeC'",
        "esses-cli: string 'workbook i2wkb' contains 'i2wkb'",
        "share/applications/esses.desktop:2: word 'i2'",
        "AppRun:2: word 'i2pro'",
        "icons/motec.png: file name contains 'motec'",
    }
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        for rel, content in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(content, str):
                p.write_bytes(content.encode("utf-8"))
            else:
                p.write_bytes(content)
        found = set(scan_tree(root, allow))
    for item in sorted(expected - found):
        print(f"self-test (tree): missed {item}")
    for item in sorted(found - expected):
        print(f"self-test (tree): unexpected {item}")
    return found == expected


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
        "tools/packaging/windows/setup.iss": "AppName=Esses for i2\n",       # installer strings (AC-M4-08)
        "tools/packaging/linux/app.desktop": "Comment=no marks here\n",
        ".github/workflows/package.yml": "name: MoTeC package\n",
        ".github/workflows/other.yml": "name: MoTeC\n",                      # out of scope
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
        "tools/packaging/windows/setup.iss:1: word 'i2'",
        ".github/workflows/package.yml:1: word 'MoTeC'",
    }
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        for rel, content in cases.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(content.encode("utf-8"))
        found = set(scan(root, allow, sorted(cases)))
    ok = found == expected and self_test_tree(allow)
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
    parser.add_argument("--tree", type=pathlib.Path, help="scan a release tree (a staged package) instead")
    args = parser.parse_args()
    if args.self_test:
        return self_test(args.allowlist)
    if args.tree is not None:
        if not args.tree.is_dir():
            print(f"check_vendor_marks: {args.tree} is not a directory")
            return 2
        findings = scan_tree(args.tree.resolve(), Allowlist.load(args.allowlist))
    else:
        findings = scan(args.root.resolve(), Allowlist.load(args.allowlist))
    for f in findings:
        print(f)
    print(f"check_vendor_marks: {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
