#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Layering lint (01_ARCHITECTURE.md sections 2.2-2.3; 08_REPO_AND_BUILD.md sections 2 and 5).

Reads <build-dir>/esses_targets.json (written at configure time by cmake/esses_targets.cmake) and
<build-dir>/compile_commands.json, then checks:
  1. every esses:: dependency of a library sits in the same or a lower layer, and no forbidden pair of
     01 section 2.2 rule 1 is linked (the configure-time check does the same; this is the lint-job copy);
  2. every `#include <esses/<other>/...>` in a target's sources (from compile_commands.json) and in a library's
     public and private headers names the target itself or one of its declared DEPS / PRIVATE_DEPS (test
     executables may also include whatever their library may include);
  3. the Qt-free set (01 section 2.3: base, units, store, model, formats, analysis, maths, project, plotcore and
     the CLI) is declared Qt-free whenever its targets exist;
  4. a library or executable links only the Qt modules of its layer (01 section 2.2 rule 4): esses_qtmodel Qt
     Core; esses_qtrender and esses-render Qt Gui; esses_widgets and esses Qt Widgets (plus Svg, PrintSupport,
     OpenGL); every other non-test target none; Qt Concurrent and Qt Sql nowhere (D-031).

Standard library only. `python -I tools/check_layering.py --build-dir build/<preset>`; `--self-test` runs the
negative and positive cases (01 section 11 item 3). Exit status: 0 clean, 1 findings, 2 usage error.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import tempfile

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([A-Za-z0-9_]+)/([A-Za-z0-9_]+)/')
QT_DEP_RE = re.compile(r"^Qt[0-9]*::([A-Za-z0-9_]+)$")
REQUIRED_QT_FREE = ("base", "units", "store", "model", "formats", "analysis", "maths", "project", "plotcore",
                    "cli_core", "cli")
_WIDGET_MODULES = frozenset({"Core", "Gui", "Widgets", "Svg", "SvgWidgets", "PrintSupport", "OpenGL",
                             "OpenGLWidgets"})
QT_MODULES_ALLOWED = {
    "qtmodel": frozenset({"Core"}),
    "qtrender": frozenset({"Core", "Gui"}),
    "render": frozenset({"Core", "Gui"}),
    "widgets": _WIDGET_MODULES,
    "app": _WIDGET_MODULES,
}
QT_MODULES_NEVER = frozenset({"Concurrent", "Sql"})
TARGET_DIR_RE = re.compile(r"CMakeFiles[\\/]+([^\\/]+)\.dir[\\/]")
HEADER_EXT = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}


def short_name(target: str, prefix: str) -> str:
    return target[len(prefix) + 1:] if target.startswith(prefix + "_") else target


def check(build_dir: pathlib.Path) -> list[str]:
    manifest_path = build_dir / "esses_targets.json"
    if not manifest_path.exists():
        raise FileNotFoundError(f"{manifest_path} not found; configure the build first")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    prefix, namespace = manifest["prefix"], manifest["namespace"]
    forbidden = set(manifest.get("forbiddenEdges", []))
    targets = {t["name"]: t for t in manifest["targets"]}
    libs = {short_name(n, prefix): t for n, t in targets.items() if t["kind"] == "library"}
    findings: list[str] = []

    def esses_deps(t: dict) -> list[str]:
        out = []
        for d in t.get("deps", []) + t.get("privateDeps", []):
            if d.startswith(namespace + "::"):
                out.append(d[len(namespace) + 2:])
        return out

    # 1. layer order and forbidden pairs
    for name, t in libs.items():
        for dep in esses_deps(t):
            target = libs.get(dep)
            if target is None:
                continue  # vendored third-party alias (esses::xxhash, ...)
            if t["layer"] is not None and target["layer"] is not None and target["layer"] > t["layer"]:
                findings.append(f"{t['name']} (layer {t['layer']}) depends on {namespace}::{dep} "
                                f"(layer {target['layer']})")
            if f"{name}>{dep}" in forbidden:
                findings.append(f"{t['name']} must never depend on {namespace}::{dep} (01 section 2.2 rule 1)")

    # allowed include namespaces per target
    allowed: dict[str, set[str]] = {}
    for name, t in libs.items():
        allowed[t["name"]] = {name, *esses_deps(t)}
    for n, t in targets.items():
        if t["kind"] == "library":
            continue
        own: set[str] = set(esses_deps(t))
        for dep in list(own):
            if dep in libs:
                own |= allowed[libs[dep]["name"]]
        allowed[n] = own
    lib_names = set(libs)

    def check_file(path: pathlib.Path, target: str) -> None:
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return
        for lineno, line in enumerate(lines, 1):
            m = INCLUDE_RE.match(line)
            if not m or m.group(1) != namespace:
                continue
            other = m.group(2)
            if other not in allowed.get(target, set()):
                known = "" if other in lib_names else " (unknown library)"
                findings.append(f"{path}:{lineno}: {target} includes <{namespace}/{other}/...> outside its "
                                f"declared DEPS{known}")

    # 2a. compiled sources, mapped to their target through the object path
    cc_path = build_dir / "compile_commands.json"
    if cc_path.exists():
        for entry in json.loads(cc_path.read_text(encoding="utf-8")):
            out = entry.get("output") or entry.get("command", "")
            m = TARGET_DIR_RE.search(out)
            if not m or m.group(1) not in targets:
                continue
            src = pathlib.Path(entry["file"])
            if not src.is_absolute():
                src = pathlib.Path(entry.get("directory", ".")) / src
            check_file(src, m.group(1))
    # 2b. public and private headers of each library
    for t in libs.values():
        root = pathlib.Path(t["sourceDir"])
        for sub in ("include", "src"):
            for hdr in sorted((root / sub).rglob("*")) if (root / sub).exists() else []:
                if hdr.suffix.lower() in HEADER_EXT:
                    check_file(hdr, t["name"])

    # 3. the Qt-free set
    for n, t in targets.items():
        if short_name(n, prefix) in REQUIRED_QT_FREE and t["kind"] != "test" and not t.get("qtFree", False):
            findings.append(f"{n} must be declared QT_FREE (01 section 2.3)")

    # 4. Qt modules per layer (direct link edges; cmake/qt-free-check.cmake checks the transitive closure)
    for n, t in targets.items():
        allowed = None if t["kind"] == "test" else QT_MODULES_ALLOWED.get(short_name(n, prefix), frozenset())
        for d in t.get("deps", []) + t.get("privateDeps", []):
            m = QT_DEP_RE.match(d)
            if not m:
                continue
            module = m.group(1)
            if module in QT_MODULES_NEVER:
                findings.append(f"{n} links {d}, which no target may use (D-031)")
            elif allowed is not None and module not in allowed:
                where = "a Qt-free target" if not allowed else "only " + ", ".join(sorted(allowed))
                findings.append(f"{n} links {d}; its layer allows {where} (01 section 2.2 rule 4)")
    return findings


def self_test() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        files = {
            "libs/a/include/esses/a/a.hpp": "#include <esses/b/b.hpp>\n",       # a does not declare b
            "libs/a/src/a.cpp": "#include <esses/a/a.hpp>\n#include <esses/low/low.hpp>\n",
            "libs/b/include/esses/b/b.hpp": "#pragma once\n",
            "libs/low/include/esses/low/low.hpp": "#pragma once\n",
            "libs/a/tests/a_tests.cpp": "#include <esses/a/a.hpp>\n#include <esses/low/low.hpp>\n",
        }
        for rel, text in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")
        lib = lambda name, layer, deps: {"name": f"esses_{name}", "kind": "library", "layer": layer,  # noqa: E731
                                         "sourceDir": str(root / "libs" / name), "deps": deps,
                                         "privateDeps": [], "qtFree": True}
        manifest = {"prefix": "esses", "namespace": "esses", "forbiddenEdges": ["formats>maths"],
                    "targets": [lib("low", 0, []), lib("b", 2, []), lib("a", 1, ["esses::low", "esses::b"]),
                                {"name": "esses_a_tests", "kind": "test", "layer": None,
                                 "sourceDir": str(root / "libs" / "a"), "deps": ["esses::a"], "privateDeps": [],
                                 "qtFree": True},
                                lib("formats", 2, ["esses::maths"]), lib("maths", 2, [])]}
        build = root / "build"
        build.mkdir()
        (build / "esses_targets.json").write_text(json.dumps(manifest), encoding="utf-8")
        commands = [
            {"directory": str(build), "file": str(root / "libs/a/src/a.cpp"),
             "output": "libs/a/CMakeFiles/esses_a.dir/src/a.cpp.obj"},
            {"directory": str(build), "file": str(root / "libs/a/tests/a_tests.cpp"),
             "output": "libs/a/CMakeFiles/esses_a_tests.dir/tests/a_tests.cpp.obj"},
        ]
        (build / "compile_commands.json").write_text(json.dumps(commands), encoding="utf-8")

        # Negative: a (layer 1) depends on b (layer 2); formats -> maths is forbidden. Includes are allowed.
        findings = check(build)
        expected_neg = ["esses_a (layer 1) depends on esses::b (layer 2)",
                        "esses_formats must never depend on esses::maths (01 section 2.2 rule 1)"]
        ok = sorted(findings) == sorted(expected_neg)

        # Negative: drop b from a's DEPS (and the upward edge); the header's include of <esses/b/...> must fail.
        manifest["targets"][2]["deps"] = ["esses::low"]
        manifest["targets"] = [t for t in manifest["targets"] if t["name"] not in ("esses_formats", "esses_maths")]
        (build / "esses_targets.json").write_text(json.dumps(manifest), encoding="utf-8")
        findings = check(build)
        ok = ok and len(findings) == 1 and "includes <esses/b/...> outside its declared DEPS" in findings[0]

        # Positive: remove the offending include.
        (root / "libs/a/include/esses/a/a.hpp").write_text("#include <esses/low/low.hpp>\n", encoding="utf-8")
        ok = ok and check(build) == []

        # Negative: the Qt rules. qtmodel links Qt6::Gui, a Qt-free library links Qt6::Core, plotcore is not
        # declared Qt-free, and a test links Qt6::Concurrent.
        def qt_lib(name: str, layer: int, deps: list[str], qt_free: bool = False) -> dict:
            return dict(lib(name, layer, deps), qtFree=qt_free)

        base_targets = list(manifest["targets"])
        manifest["targets"] = base_targets + [
            qt_lib("qtmodel", 4, ["esses::low", "Qt6::Core", "Qt6::Gui"]),
            qt_lib("plotcore", 3, ["esses::low"]),
            qt_lib("b2", 2, ["Qt6::Core"], qt_free=True),
            {"name": "esses_qtmodel_tst_x", "kind": "test", "layer": None, "sourceDir": str(root / "libs" / "a"),
             "deps": ["Qt6::Concurrent"], "privateDeps": [], "qtFree": False}]
        (build / "esses_targets.json").write_text(json.dumps(manifest), encoding="utf-8")
        findings = sorted(check(build))
        expected_qt = sorted([
            "esses_qtmodel links Qt6::Gui; its layer allows only Core (01 section 2.2 rule 4)",
            "esses_plotcore must be declared QT_FREE (01 section 2.3)",
            "esses_b2 links Qt6::Core; its layer allows a Qt-free target (01 section 2.2 rule 4)",
            "esses_qtmodel_tst_x links Qt6::Concurrent, which no target may use (D-031)"])
        ok = ok and findings == expected_qt

        # Positive: the allowed modules per layer.
        manifest["targets"] = base_targets + [
            qt_lib("qtmodel", 4, ["esses::low", "Qt6::Core"]),
            qt_lib("qtrender", 4, ["Qt6::Gui"]),
            qt_lib("widgets", 5, ["Qt6::Widgets", "Qt6::Gui", "Qt6::Svg"]),
            qt_lib("plotcore", 3, ["esses::low"], qt_free=True)]
        (build / "esses_targets.json").write_text(json.dumps(manifest), encoding="utf-8")
        ok = ok and check(build) == []
    print("check_layering self-test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=pathlib.Path, help="a configured build directory")
    parser.add_argument("--self-test", action="store_true", help="run the built-in negative and positive cases")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.build_dir:
        parser.error("--build-dir is required")
    try:
        findings = check(args.build_dir)
    except (FileNotFoundError, json.JSONDecodeError, KeyError) as e:
        print(f"check_layering: {e}", file=sys.stderr)
        return 2
    for f in findings:
        print(f)
    print(f"check_layering: {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
