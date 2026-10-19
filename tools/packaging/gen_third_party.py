#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""The third-party manifest and SBOM of an Esses package (M4 packaging, D-212; 08 sections 7.4 and 9; AC-M4-06).

From tools/packaging/third_party.json (the components the packages ship), the licence files it names and the
files of a staged package, writes
  * THIRD_PARTY_LICENSES.md: every component in the package with version, licence, copyright, source and use;
    the Qt libraries and plugins actually deployed (and on Linux the system libraries linuxdeploy copied, with
    the copyright files package.py collected); the licence text of every vendored component verbatim, pointers
    to licenses/ for the long LGPL/GPL texts;
  * a CycloneDX 1.5 JSON SBOM of the same components (deterministic: the timestamp is the build date, the serial
    number a UUID derived from version, OS and package).
--check keeps third_party.json, NOTICES.md, third_party/README.md and third_party/ in step (every vendored
directory listed, its version in both tables, the docking library's version as its project declares it, every
licence text present). Standard library only.

  python -I gen_third_party.py --check [--root <repository>]
  python -I gen_third_party.py --root <repository> --stage <tree> --package app|cli --os windows|linux|macos
         --info <build>/packaging/package_info.json [--vc-runtime-version <v>] --out <THIRD_PARTY_LICENSES.md>
         [--sbom <file.cdx.json>]
  python -I gen_third_party.py --self-test
Exit status: 0 done or clean, 1 problems, 2 usage error.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import tempfile
import uuid

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
REPOSITORY_URL = "https://github.com/mitchdetailed/jubilant-octo-meme"
LONG_LICENCES = {"LGPL-3.0-only", "GPL-3.0-only", "LGPL-2.1-or-later"}  # pointed to in licenses/, not repeated

QT_WIN = re.compile(r"^Qt6([A-Za-z]+)\.dll$")
QT_LINUX = re.compile(r"^libQt6([A-Za-z]+)\.so(\.\d+)*$")
QT_MAC = re.compile(r"^Qt([A-Za-z]+)\.framework$")
ICU = re.compile(r"^libicu(data|i18n|uc)\.so\.(\d+)(\.\d+)*$")
ADS = re.compile(r"^(lib)?qtadvanceddocking-qt6d?(\.\d+)*\.(dll|so|dylib)(\.\d+)*$")
MSVC = re.compile(r"^(msvcp140(_\d+|_atomic_wait|_codecvt_ids)?|vcruntime140(_1|_threads)?|concrt140|vccorlib140)\.dll$",
                  re.IGNORECASE)
OURS = re.compile(r"^esses(-cli|-render)?(\.exe)?$")


# ---- the component list ------------------------------------------------------------------------------------

def load_components(root: pathlib.Path) -> dict:
    return json.loads((root / "tools" / "packaging" / "third_party.json").read_text(encoding="utf-8"))


def licence_ids(expression: str) -> list[str]:
    return [p.strip("() ") for p in re.split(r"\s+(?:OR|AND|WITH)\s+", expression) if p.strip("() ")]


def licence_text(root: pathlib.Path, entry: dict) -> str:
    path = root / entry["file"]
    text = path.read_text(encoding="utf-8", errors="replace")
    if entry.get("firstComment"):
        m = re.search(r"/\*(.*?)\*/", text, re.S)
        if not m:
            raise ValueError(f"{entry['file']}: no comment block")
        lines = [re.sub(r"^\s*\* ?", "", line) for line in m.group(1).splitlines()]
        text = "\n".join(lines).strip() + "\n"
    return text


def check(root: pathlib.Path) -> list[str]:
    problems: list[str] = []
    try:
        data = load_components(root)
    except (OSError, ValueError) as e:
        return [f"third_party.json: {e}"]
    notices = (root / "NOTICES.md").read_text(encoding="utf-8")
    readme = (root / "third_party" / "README.md").read_text(encoding="utf-8")
    listed_dirs = set()
    for c in data.get("components", []):
        cid = c.get("id", "?")
        for key in ("id", "name", "noticesName", "version", "licence", "copyright", "homepage", "packages", "use"):
            if key not in c:
                problems.append(f"{cid}: '{key}' missing")
        name = c.get("noticesName", "")
        if name and name not in notices:
            problems.append(f"{cid}: NOTICES.md does not name '{name}'")
        for entry in c.get("licenceFiles", []):
            if not (root / entry["file"]).is_file():
                problems.append(f"{cid}: licence file {entry['file']} missing")
            else:
                try:
                    licence_text(root, entry)
                except ValueError as e:
                    problems.append(f"{cid}: {e}")
        for lid in licence_ids(c.get("licence", "")):
            if lid.startswith("LicenseRef-"):
                if "licenceUrl" not in c:
                    problems.append(f"{cid}: {lid} needs licenceUrl")
                continue
            if not ((root / "LICENSES" / f"{lid}.txt").is_file() or
                    (root / "tools" / "packaging" / "licenses" / f"LICENSE.{lid}.txt").is_file()):
                problems.append(f"{cid}: no licence text for {lid} (LICENSES/{lid}.txt)")
        vendored = c.get("vendored")
        if vendored:
            listed_dirs.add(vendored)
            if not (root / vendored).is_dir():
                problems.append(f"{cid}: {vendored} does not exist")
            version = c.get("version", "")
            row = re.compile(r"^\|\s*" + re.escape(name) + r"\s*\|\s*" + re.escape(version) + r"\b", re.M)
            if not row.search(notices):
                problems.append(f"{cid}: NOTICES.md has no table row '| {name} | {version} |'")
            readme_name = c.get("readmeName", name)
            row = re.compile(r"^\|\s*" + re.escape(readme_name) + r"\s*\|\s*" + re.escape(version) + r"\b", re.M)
            if not row.search(readme):
                problems.append(f"{cid}: third_party/README.md has no row '| {readme_name} | {version} ...'")
    for t in data.get("testOnly", []):
        listed_dirs.add(t["dir"])
    third_party = root / "third_party"
    for d in sorted(p for p in third_party.iterdir() if p.is_dir()):
        rel = d.relative_to(root).as_posix()
        if rel not in listed_dirs:
            problems.append(f"{rel}: not in tools/packaging/third_party.json (a component or testOnly)")
    ads = next((c for c in data.get("components", []) if c.get("id") == "qtads"), None)
    cmake = root / "third_party" / "qtads" / "CMakeLists.txt"
    if ads and cmake.is_file():
        m = re.search(r"project\(\s*QtADS\s+VERSION\s+([0-9.]+)", cmake.read_text(encoding="utf-8"))
        if not m or m.group(1) != ads["version"]:
            problems.append(f"qtads: third_party.json says {ads['version']}, its CMakeLists.txt "
                            f"{m.group(1) if m else 'nothing'}")
    return problems


# ---- what a staged package holds ------------------------------------------------------------------------------

def scan_stage(stage: pathlib.Path, os_name: str, debug: bool = False) -> dict:
    found = {"qtModules": set(), "qtFiles": [], "qtPlugins": [], "icu": {}, "msvc": [], "ads": [], "ours": [],
             "other": []}
    files = sorted(p for p in stage.rglob("*") if p.is_file() or p.is_symlink())
    for p in files:
        rel = p.relative_to(stage).as_posix()
        name = p.name
        parts = rel.split("/")
        if os_name == "macos":
            framework = next((x for x in parts if QT_MAC.match(x)), None)
            if framework:
                module = QT_MAC.match(framework).group(1)
                found["qtModules"].add(module)
                if framework not in found["qtFiles"]:
                    found["qtFiles"].append(framework)
                continue
            if "PlugIns" in parts and name.endswith(".dylib"):
                found["qtPlugins"].append("/".join(parts[parts.index("PlugIns") + 1:]))
                continue
        if os_name == "windows":
            m = QT_WIN.match(name)
            if m and len(parts) == 1:
                module = m.group(1)
                if debug and module.endswith("d"):
                    module = module[:-1]
                found["qtModules"].add(module)
                found["qtFiles"].append(name)
                continue
            if name.lower().endswith(".dll") and len(parts) > 1 and not MSVC.match(name):
                found["qtPlugins"].append(rel)
                continue
        if os_name == "linux":
            m = QT_LINUX.match(name)
            if m:
                found["qtModules"].add(m.group(1))
                found["qtFiles"].append(name)
                continue
            if "plugins" in parts and ".so" in name:
                found["qtPlugins"].append("/".join(parts[parts.index("plugins") + 1:]))
                continue
            m = ICU.match(name)
            if m:
                found["icu"].setdefault(m.group(2), []).append(name)
                continue
        if ADS.match(name):
            found["ads"].append(name)
        elif MSVC.match(name):
            found["msvc"].append(name)
        elif OURS.match(name):
            found["ours"].append(rel)
        elif re.search(r"\.(dll|dylib)$|\.so(\.\d+)*$", name):
            found["other"].append(rel)
    found["qtModules"] = sorted(found["qtModules"])
    return found


def components_for(data: dict, package: str, os_name: str) -> list[dict]:
    return [c for c in data["components"]
            if package in c["packages"] and os_name in c.get("os", [os_name])]


def resolved(c: dict, qt_version: str, vc_version: str) -> dict:
    out = dict(c)
    version = c["version"]
    if version == "from the build":
        version = qt_version if c["id"] == "qt" else vc_version
    out["version"] = version or "unknown"
    major_minor = ".".join(out["version"].split(".")[:2])
    for key in ("source", "purl"):
        if key in out:
            out[key] = out[key].replace("{version}", out["version"]).replace("{majorMinor}", major_minor)
    return out


# ---- THIRD_PARTY_LICENSES.md ---------------------------------------------------------------------------------

def render_markdown(root: pathlib.Path, comps: list[dict], info: dict, found: dict, package: str,
                    os_name: str, system_licences: dict[str, str] | None = None) -> str:
    version = info.get("version", "unknown")
    out = [
        "# Third-party licences",
        "",
        f"Esses {version}, {'the application' if package == 'app' else 'the command-line'} package for {os_name}"
        f" (commit {info.get('commit', 'unknown')}). Generated by `tools/packaging/gen_third_party.py` from"
        " `tools/packaging/third_party.json`, the licence files of the source tree and the files of this package."
        " `NOTICES.md` beside this file says how each component is used, where the source of the LGPL libraries is"
        " and how to replace them. Esses itself is MIT-licensed (`LICENSE`).",
        "",
        "| Component | Version | Licence | Copyright | Source |",
        "|---|---|---|---|---|",
    ]
    for c in comps:
        out.append(f"| {c['name']} | {c['version']} | {c['licence']} | {c['copyright']} | {c.get('source', '')} |")
    out += ["", "## How each is used", ""]
    for c in comps:
        out.append(f"- **{c['name']}**: {c['use']}")
    if package == "app" and found["qtFiles"]:
        qt = next((c for c in comps if c["id"] == "qt"), None)
        out += ["", "## Qt in this package", "",
                f"This application uses Qt {qt['version'] if qt else ''} under the GNU LGPL v3 (licenses/LGPL-3.0-only.txt,"
                " with the GNU GPL v3 it builds on, licenses/GPL-3.0-only.txt). The source of exactly this version:"
                f" {qt['source'] if qt else ''}.",
                "",
                "- Modules: " + ", ".join(found["qtModules"]),
                "- Libraries: " + ", ".join(sorted(found["qtFiles"])),
                "- Plugins: " + (", ".join(sorted(found["qtPlugins"])) or "none")]
    if found["icu"]:
        out += ["", "## ICU", "",
                "The Qt libraries for Linux come with ICU (International Components for Unicode), version "
                + ", ".join(sorted(found["icu"])) + " (" + ", ".join(sorted(sum(found["icu"].values(), []))) + "),"
                " Copyright (C) Unicode, Inc. and others, under the Unicode License (Unicode-3.0; ICU 73 and"
                " earlier: Unicode-DFS-2016 with the ICU licence): https://github.com/unicode-org/icu/blob/main/LICENSE."]
    others = [o for o in found["other"]]
    if others:
        out += ["", "## Other libraries in this package", "",
                "Copied by the deployment tool from the build machine's system; each one's licence is in the file"
                " named beside it (from its distribution package) or stated by its upstream project."]
        for o in sorted(others):
            note = (system_licences or {}).get(pathlib.PurePosixPath(o).name, "")
            out.append(f"- {o}" + (f": {note}" if note else ""))
    out += ["", "## Licence texts", ""]
    for c in comps:
        files = c.get("licenceFiles", [])
        long_ids = [lid for lid in licence_ids(c["licence"]) if lid in LONG_LICENCES]
        if long_ids:
            shipped = ", ".join(f.get("shippedAs", f["file"]) for f in files)
            out += [f"### {c['name']} ({c['licence']})", "", f"The licence texts are in this package: {shipped}.", ""]
            continue
        if not files:
            out += [f"### {c['name']} ({c['licence']})", "",
                    f"{c.get('licenceName', c['licence'])}: {c.get('licenceUrl', c.get('homepage', ''))}", ""]
            continue
        for f in files:
            out += [f"### {c['name']} ({c['licence']})", "", "```text", licence_text(root, f).rstrip("\n"), "```", ""]
    return "\n".join(out).rstrip("\n") + "\n"


# ---- CycloneDX ------------------------------------------------------------------------------------------------

def licence_entry(c: dict) -> list[dict]:
    ids = licence_ids(c["licence"])
    if len(ids) > 1:
        return [{"expression": c["licence"]}]
    if ids and ids[0].startswith("LicenseRef-"):
        return [{"license": {"name": c.get("licenceName", ids[0]), "url": c.get("licenceUrl", "")}}]
    return [{"license": {"id": ids[0]}}] if ids else []


def render_sbom(comps: list[dict], info: dict, found: dict, package: str, os_name: str) -> dict:
    version = info.get("version", "unknown")
    serial = uuid.uuid5(uuid.NAMESPACE_URL, f"{REPOSITORY_URL}/{version}/{os_name}/{package}")
    date = info.get("buildDate") or "1970-01-01"
    components = []
    for c in comps:
        entry = {"type": "library", "bom-ref": c["id"], "name": c["name"], "version": c["version"],
                 "licenses": licence_entry(c), "copyright": c["copyright"]}
        if c.get("purl"):
            entry["purl"] = c["purl"]
        refs = [{"type": "website", "url": c["homepage"]}]
        if c.get("source"):
            refs.append({"type": "distribution", "url": c["source"]})
        entry["externalReferences"] = refs
        if c["id"] == "qt" and found["qtFiles"]:
            entry["properties"] = [{"name": "esses:qt-modules", "value": ", ".join(found["qtModules"])},
                                   {"name": "esses:qt-plugins", "value": ", ".join(sorted(found["qtPlugins"]))}]
        components.append(entry)
    if found["icu"]:
        icu_version = sorted(found["icu"])[-1]
        components.append({"type": "library", "bom-ref": "icu", "name": "ICU", "version": icu_version,
                           "licenses": [{"license": {"name": "Unicode License (ICU)",
                                                     "url": "https://github.com/unicode-org/icu/blob/main/LICENSE"}}],
                           "externalReferences": [{"type": "website", "url": "https://icu.unicode.org/"}]})
    for o in sorted(found["other"]):
        name = pathlib.PurePosixPath(o).name
        components.append({"type": "library", "bom-ref": "file:" + o, "name": name,
                           "properties": [{"name": "esses:path", "value": o}]})
    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": {
            "timestamp": f"{date}T00:00:00Z",
            "tools": {"components": [{"type": "application", "name": "gen_third_party.py",
                                      "description": "Esses tools/packaging/gen_third_party.py"}]},
            "component": {
                "type": "application", "bom-ref": "esses", "name": "esses" if package == "app" else "esses-cli",
                "version": version, "licenses": [{"license": {"id": "MIT"}}],
                "externalReferences": [{"type": "vcs", "url": REPOSITORY_URL}],
                "properties": [{"name": "esses:commit", "value": info.get("commit", "unknown")},
                               {"name": "esses:package", "value": package},
                               {"name": "esses:os", "value": os_name},
                               {"name": "esses:release-tag", "value": info.get("releaseTag", "")}],
            },
        },
        "components": components,
        "dependencies": [{"ref": "esses", "dependsOn": [c["bom-ref"] for c in components]}],
    }
    return bom


def validate_sbom(bom: dict) -> list[str]:
    """The structure CycloneDX 1.5 requires of what we write (no schema download)."""
    problems = []
    if bom.get("bomFormat") != "CycloneDX" or bom.get("specVersion") != "1.5":
        problems.append("bomFormat/specVersion")
    if not re.match(r"^urn:uuid:[0-9a-f-]{36}$", bom.get("serialNumber", "")):
        problems.append("serialNumber")
    if not isinstance(bom.get("version"), int) or bom["version"] < 1:
        problems.append("version")
    meta = bom.get("metadata", {})
    if not re.match(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$", meta.get("timestamp", "")):
        problems.append("metadata.timestamp")
    refs = [meta.get("component", {}).get("bom-ref")]
    for c in bom.get("components", []):
        if c.get("type") not in ("library", "application", "framework", "file") or not c.get("name"):
            problems.append(f"component {c.get('name')}: type/name")
        for lic in c.get("licenses", []):
            if not (("license" in lic and ("id" in lic["license"] or "name" in lic["license"])) or "expression" in lic):
                problems.append(f"component {c.get('name')}: licence entry")
        refs.append(c.get("bom-ref"))
    if len(refs) != len(set(refs)) or None in refs:
        problems.append("bom-refs not unique")
    known = set(refs)
    for d in bom.get("dependencies", []):
        if d.get("ref") not in known or any(r not in known for r in d.get("dependsOn", [])):
            problems.append("dependencies name an unknown bom-ref")
    return problems


# ---- driver -----------------------------------------------------------------------------------------------------

def generate(root: pathlib.Path, stage: pathlib.Path, package: str, os_name: str, info: dict, vc_version: str,
             out: pathlib.Path, sbom: pathlib.Path | None, system_licences: dict[str, str] | None = None) -> list[str]:
    data = load_components(root)
    debug = str(info.get("config", "")).lower() == "debug"
    found = scan_stage(stage, os_name, debug)
    comps = [resolved(c, info.get("qtVersion", ""), vc_version) for c in components_for(data, package, os_name)]
    if package == "app" and not found["qtFiles"]:
        return [f"{stage}: no Qt library in the package (was the deployment tool run?)"]
    out.write_text(render_markdown(root, comps, info, found, package, os_name, system_licences), encoding="utf-8",
                   newline="\n")
    if sbom is not None:
        bom = render_sbom(comps, info, found, package, os_name)
        problems = validate_sbom(bom)
        if problems:
            return [f"SBOM: {p}" for p in problems]
        sbom.write_text(json.dumps(bom, indent=2, sort_keys=False) + "\n", encoding="utf-8", newline="\n")
    return []


def self_test() -> int:
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        # A Windows tree as windeployqt leaves it.
        win = t / "win"
        for rel in ("esses.exe", "esses-cli.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
                    "qtadvanceddocking-qt6.dll", "platforms/qwindows.dll", "platforms/qoffscreen.dll",
                    "styles/qmodernwindowsstyle.dll", "msvcp140.dll", "vcruntime140_1.dll", "data/units.json"):
            (win / rel).parent.mkdir(parents=True, exist_ok=True)
            (win / rel).write_bytes(b"x")
        f = scan_stage(win, "windows")
        ok &= f["qtModules"] == ["Core", "Gui", "Widgets"]
        ok &= sorted(f["qtPlugins"]) == ["platforms/qoffscreen.dll", "platforms/qwindows.dll",
                                         "styles/qmodernwindowsstyle.dll"]
        ok &= f["ads"] == ["qtadvanceddocking-qt6.dll"] and sorted(f["msvc"]) == ["msvcp140.dll", "vcruntime140_1.dll"]
        ok &= sorted(f["ours"]) == ["esses-cli.exe", "esses.exe"] and f["other"] == []
        dbg = t / "dbg"
        (dbg).mkdir()
        (dbg / "Qt6Cored.dll").write_bytes(b"x")
        ok &= scan_stage(dbg, "windows", debug=True)["qtModules"] == ["Core"]
        # A Linux AppDir/usr.
        lin = t / "usr"
        for rel in ("bin/esses", "lib/libQt6Core.so.6", "lib/libQt6XcbQpa.so.6", "lib/libicui18n.so.73",
                    "lib/libicuuc.so.73", "lib/libqtadvanceddocking-qt6.so.5", "lib/libxcb-cursor.so.0",
                    "plugins/platforms/libqxcb.so", "share/esses/data/units.json"):
            (lin / rel).parent.mkdir(parents=True, exist_ok=True)
            (lin / rel).write_bytes(b"x")
        f = scan_stage(lin, "linux")
        ok &= f["qtModules"] == ["Core", "XcbQpa"] and f["qtPlugins"] == ["platforms/libqxcb.so"]
        ok &= sorted(f["icu"]) == ["73"] and f["other"] == ["lib/libxcb-cursor.so.0"]
        # A macOS bundle.
        mac = t / "Esses.app" / "Contents"
        for rel in ("MacOS/esses", "Frameworks/QtCore.framework/Versions/A/QtCore", "Frameworks/QtGui.framework/QtGui",
                    "Frameworks/libqtadvanceddocking-qt6.5.dylib", "PlugIns/platforms/libqcocoa.dylib"):
            (mac / rel).parent.mkdir(parents=True, exist_ok=True)
            (mac / rel).write_bytes(b"x")
        f = scan_stage(mac, "macos")
        ok &= f["qtModules"] == ["Core", "Gui"] and f["qtPlugins"] == ["platforms/libqcocoa.dylib"]
        ok &= f["ads"] == ["libqtadvanceddocking-qt6.5.dylib"]
        # The manifest and the SBOM of the Windows tree, from the real component list.
        info = {"version": "0.1.0", "commit": "abc1234", "qtVersion": "6.12.0", "buildDate": "2026-10-19",
                "releaseTag": "v0.1.0"}
        out = t / "THIRD_PARTY_LICENSES.md"
        sbom = t / "sbom.cdx.json"
        problems = generate(REPO, win, "app", "windows", info, "14.44.35112", out, sbom)
        ok &= problems == []
        text = out.read_text(encoding="utf-8")
        for needle in ("This application uses Qt 6.12.0 under the GNU LGPL v3",
                       "https://download.qt.io/official_releases/qt/6.12/6.12.0/single/qt-everywhere-src-6.12.0.tar.xz",
                       "| Qt Advanced Docking System | 5.1.1 |", "Copyright (C) 2012-2023 Yann Collet",
                       "Copyright (c) 2006-2024 Arseny Kapoulkine", "Microsoft Visual C++ Redistributable | 14.44.35112",
                       "Modules: Core, Gui, Widgets"):
            if needle not in text:
                print(f"self-test: the manifest lacks '{needle}'")
                ok = False
        bom = json.loads(sbom.read_text(encoding="utf-8"))
        ok &= validate_sbom(bom) == []
        ok &= {c["bom-ref"] for c in bom["components"]} >= {"qt", "qtads", "pugixml", "msvc-runtime"}
        ok &= bom["serialNumber"] == render_sbom([], info, scan_stage(win, "windows"), "app", "windows")["serialNumber"]
        # The command-line package: no Qt, no docking library.
        problems = generate(REPO, win, "cli", "linux", info, "", out, None)
        text = out.read_text(encoding="utf-8")
        ok &= problems == [] and "| Qt |" not in text and "Qt Advanced Docking System" not in text and "CLI11" in text
        # A broken SBOM is caught.
        ok &= validate_sbom({"bomFormat": "CycloneDX", "specVersion": "1.4"}) != []
        # --check finds a missing row, a version that differs and an unlisted directory.
        fake = t / "repo"
        (fake / "tools" / "packaging").mkdir(parents=True)
        (fake / "LICENSES").mkdir()
        (fake / "LICENSES" / "MIT.txt").write_text("MIT\n", encoding="utf-8")
        (fake / "third_party" / "lib" ).mkdir(parents=True)
        (fake / "third_party" / "stray").mkdir()
        (fake / "third_party" / "lib" / "LICENSE").write_text("Copyright x\n", encoding="utf-8")
        (fake / "third_party" / "README.md").write_text("| lib | 2.0 | MIT |\n", encoding="utf-8")
        (fake / "NOTICES.md").write_text("lib is used\n", encoding="utf-8")
        (fake / "tools" / "packaging" / "third_party.json").write_text(json.dumps({"components": [{
            "id": "lib", "name": "lib", "noticesName": "lib", "version": "1.0", "licence": "MIT", "copyright": "x",
            "homepage": "h", "packages": ["app"], "use": "u", "vendored": "third_party/lib",
            "licenceFiles": [{"file": "third_party/lib/LICENSE"}]}], "testOnly": []}), encoding="utf-8")
        found = set(check(fake))
        expected = {"lib: NOTICES.md has no table row '| lib | 1.0 |'",
                    "lib: third_party/README.md has no row '| lib | 1.0 ...'",
                    "third_party/stray: not in tools/packaging/third_party.json (a component or testOnly)"}
        if found != expected:
            print(f"self-test: check() found {sorted(found)}")
            ok = False
    print("gen_third_party self-test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=REPO)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--stage", type=pathlib.Path)
    parser.add_argument("--package", choices=("app", "cli"))
    parser.add_argument("--os", dest="os_name", choices=("windows", "linux", "macos"))
    parser.add_argument("--info", type=pathlib.Path)
    parser.add_argument("--vc-runtime-version", default="")
    parser.add_argument("--system-licences", type=pathlib.Path,
                        help="JSON {library file name: where its licence is} (Linux, written by package.py)")
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--sbom", type=pathlib.Path)
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.check:
        problems = check(args.root.resolve())
        for p in problems:
            print(p)
        print(f"gen_third_party --check: {len(problems)} problem(s)")
        return 1 if problems else 0
    if not (args.stage and args.package and args.os_name and args.info and args.out):
        parser.error("--stage, --package, --os, --info and --out are needed (or --check / --self-test)")
    info = json.loads(args.info.read_text(encoding="utf-8"))
    system = json.loads(args.system_licences.read_text(encoding="utf-8")) if args.system_licences else None
    problems = generate(args.root.resolve(), args.stage, args.package, args.os_name, info, args.vc_runtime_version,
                        args.out, args.sbom, system)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
