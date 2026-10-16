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
     Core and Qt Gui (D-161); esses_qtrender and esses-render Qt Gui; esses_widgets and esses Qt Widgets (plus
     Svg, PrintSupport, OpenGL); every other non-test target none; Qt Concurrent and Qt Sql nowhere (D-031);
  5. only esses_widgets and the esses application link the docking library (esses::qtads, the LGPL Qt Advanced
     Docking System as a shared library, D-026, D-153); tests may link what their library links;
  6. esses_qtmodel uses Qt Gui for the undo framework only (D-161): its sources and headers include no Qt Gui
     header except QUndoStack and QUndoCommand. The Qt Gui headers are the files of the directories the configure
     step records as qtGuiHeaderDirs (Qt6::Gui's include directory); the check is skipped without them.

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
    "qtmodel": frozenset({"Core", "Gui"}),  # Gui: QUndoStack/QUndoCommand only (D-161, rule 6)
    "qtrender": frozenset({"Core", "Gui"}),
    "render": frozenset({"Core", "Gui"}),
    "widgets": _WIDGET_MODULES,
    "app": _WIDGET_MODULES,
}
QT_MODULES_NEVER = frozenset({"Concurrent", "Sql"})
DOCKING_LIBRARY_USERS = frozenset({"widgets", "app"})
# Rule 6 (D-161): the only Qt Gui headers a library may include, per library; libraries not listed are not
# checked here (rule 4 covers which modules they link).
QT_GUI_HEADERS_ALLOWED = {"qtmodel": frozenset({"QUndoStack", "QUndoCommand", "qundostack.h"})}
QT_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]((QtGui)/)?([A-Za-z0-9_.]+)[>"]')


def is_docking_library(dep: str, namespace: str) -> bool:
    """The docking library under any name it can be linked by (our alias, ADS's aliases and target names)."""
    return (dep in (f"{namespace}::qtads", f"{namespace}::ads") or dep.startswith("ads::")
            or re.match(r"^qt[0-9]*advanceddocking", dep) is not None)
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

    # 5. the docking library (direct link edges)
    for n, t in targets.items():
        if t["kind"] == "test" or short_name(n, prefix) in DOCKING_LIBRARY_USERS:
            continue
        for d in t.get("deps", []) + t.get("privateDeps", []):
            if is_docking_library(d, namespace):
                findings.append(f"{n} links {d}; only esses_widgets and the esses application link the docking "
                                f"library (D-026)")

    # 6. Qt Gui headers of the libraries that may use part of Qt Gui only (D-161)
    gui_headers: set[str] = set()
    for d in manifest.get("qtGuiHeaderDirs", []):
        gdir = pathlib.Path(d)
        if gdir.is_dir():
            gui_headers.update(f.name for f in gdir.iterdir() if f.is_file())
    if gui_headers:
        sources_of: dict[str, list[pathlib.Path]] = {}
        if cc_path.exists():
            for entry in json.loads(cc_path.read_text(encoding="utf-8")):
                out = entry.get("output") or entry.get("command", "")
                m = TARGET_DIR_RE.search(out)
                if m:
                    src = pathlib.Path(entry["file"])
                    if not src.is_absolute():
                        src = pathlib.Path(entry.get("directory", ".")) / src
                    sources_of.setdefault(m.group(1), []).append(src)
        for name, allowed_gui in QT_GUI_HEADERS_ALLOWED.items():
            t = libs.get(name)
            if t is None:
                continue
            files = list(sources_of.get(t["name"], []))
            root = pathlib.Path(t["sourceDir"])
            for sub in ("include", "src"):
                if (root / sub).exists():
                    files += [h for h in sorted((root / sub).rglob("*")) if h.suffix.lower() in HEADER_EXT]
            for path in sorted(set(files)):
                try:
                    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
                except OSError:
                    continue
                for lineno, line in enumerate(lines, 1):
                    m = QT_INCLUDE_RE.match(line)
                    if not m:
                        continue
                    header = m.group(3)
                    is_gui = m.group(2) is not None or header == "QtGui" or header in gui_headers
                    if is_gui and header not in allowed_gui:
                        findings.append(f"{path}:{lineno}: {t['name']} includes Qt Gui header <{m.group(1) or ''}"
                                        f"{header}>; D-161 allows only " + ", ".join(sorted(allowed_gui)))
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

        # Negative: the Qt rules. qtmodel links Qt6::Widgets, a Qt-free library links Qt6::Core, plotcore is not
        # declared Qt-free, and a test links Qt6::Concurrent.
        def qt_lib(name: str, layer: int, deps: list[str], qt_free: bool = False) -> dict:
            return dict(lib(name, layer, deps), qtFree=qt_free)

        base_targets = list(manifest["targets"])
        manifest["targets"] = base_targets + [
            qt_lib("qtmodel", 4, ["esses::low", "Qt6::Core", "Qt6::Widgets"]),
            qt_lib("plotcore", 3, ["esses::low"]),
            qt_lib("b2", 2, ["Qt6::Core"], qt_free=True),
            {"name": "esses_qtmodel_tst_x", "kind": "test", "layer": None, "sourceDir": str(root / "libs" / "a"),
             "deps": ["Qt6::Concurrent"], "privateDeps": [], "qtFree": False}]
        (build / "esses_targets.json").write_text(json.dumps(manifest), encoding="utf-8")
        findings = sorted(check(build))
        expected_qt = sorted([
            "esses_qtmodel links Qt6::Widgets; its layer allows only Core, Gui (01 section 2.2 rule 4)",
            "esses_plotcore must be declared QT_FREE (01 section 2.3)",
            "esses_b2 links Qt6::Core; its layer allows a Qt-free target (01 section 2.2 rule 4)",
            "esses_qtmodel_tst_x links Qt6::Concurrent, which no target may use (D-031)"])
        ok = ok and findings == expected_qt

        # Positive: the allowed modules per layer, and the docking library in esses_widgets (privately) and its
        # tests.
        widgets = qt_lib("widgets", 5, ["Qt6::Widgets", "Qt6::Gui", "Qt6::Svg"])
        widgets["privateDeps"] = ["esses::qtads"]
        manifest["targets"] = base_targets + [
            qt_lib("qtmodel", 4, ["esses::low", "Qt6::Core", "Qt6::Gui"]),
            qt_lib("qtrender", 4, ["Qt6::Gui"]),
            widgets,
            {"name": "esses_widgets_tst_dock_host", "kind": "test", "layer": None,
             "sourceDir": str(root / "libs" / "a"), "deps": ["esses::widgets", "esses::qtads"], "privateDeps": [],
             "qtFree": False},
            qt_lib("plotcore", 3, ["esses::low"], qt_free=True)]
        (build / "esses_targets.json").write_text(json.dumps(manifest), encoding="utf-8")
        ok = ok and check(build) == []

        # Negative: the docking library linked by another library and, under ADS's own name, by an executable.
        manifest["targets"] = base_targets + [
            qt_lib("qtrender", 4, ["Qt6::Gui", "esses::qtads"]),
            {"name": "esses_plot_bench", "kind": "executable", "layer": 5, "sourceDir": str(root / "bench"),
             "deps": ["ads::qtadvanceddocking-qt6"], "privateDeps": [], "qtFree": False}]
        (build / "esses_targets.json").write_text(json.dumps(manifest), encoding="utf-8")
        findings = sorted(check(build))
        expected_ads = sorted([
            "esses_qtrender links esses::qtads; only esses_widgets and the esses application link the docking "
            "library (D-026)",
            "esses_plot_bench links ads::qtadvanceddocking-qt6; only esses_widgets and the esses application link "
            "the docking library (D-026)"])
        ok = ok and findings == expected_ads

        # Rule 6 (D-161): esses_qtmodel includes the undo framework (allowed) and QImage (refused); without the
        # recorded Qt Gui header directories the rule is skipped.
        gui_dir = root / "qt" / "include" / "QtGui"
        gui_dir.mkdir(parents=True)
        for header in ("QImage", "qimage.h", "QUndoStack", "QUndoCommand", "qundostack.h", "QtGui"):
            (gui_dir / header).write_text("", encoding="utf-8")
        qtmodel_dir = root / "libs" / "qtmodel"
        (qtmodel_dir / "include" / "esses" / "qtmodel").mkdir(parents=True)
        (qtmodel_dir / "src").mkdir(parents=True)
        (qtmodel_dir / "include" / "esses" / "qtmodel" / "undo.hpp").write_text(
            "#include <QUndoCommand>\n#include <QtGui/QUndoStack>\n#include <QObject>\n", encoding="utf-8")
        (qtmodel_dir / "src" / "image.hpp").write_text("#include <QString>\n#include <QImage>\n",
                                                       encoding="utf-8")
        qtmodel = qt_lib("qtmodel", 4, ["Qt6::Core", "Qt6::Gui"])
        qtmodel["sourceDir"] = str(qtmodel_dir)
        manifest["targets"] = base_targets + [qtmodel]
        manifest["qtGuiHeaderDirs"] = [str(gui_dir)]
        (build / "esses_targets.json").write_text(json.dumps(manifest), encoding="utf-8")
        findings = check(build)
        expected_gui = "image.hpp:2: esses_qtmodel includes Qt Gui header <QImage>"
        ok = ok and len(findings) == 1 and expected_gui in findings[0]
        manifest["qtGuiHeaderDirs"] = []
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
