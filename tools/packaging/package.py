#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Makes the Esses packages from a configured and built tree (M4 packaging, D-212; 07 section 7.1, 08 section 7).

  python -I tools/packaging/package.py --build <build dir> --out <dist dir> [options]
  python -I tools/packaging/package.py --self-test

The build is a Release build with ESSES_BUILD_QT=ON (a release one with -DESSES_RELEASE_TAG=v<version>); its
<build>/packaging/package_info.json (tools/packaging/CMakeLists.txt) gives the version, the Qt kit and the
layout. Per OS (the host's unless --os):
  windows  cmake --install (component app) -> windeployqt (+ the offscreen platform plugin) -> notices and SBOM ->
           vendor-mark check of the tree -> Inno Setup installer (esses.iss, the build toolset's vc_redist.x64.exe
           bundled) -> portable zip (the tree + portable.marker + the C++ runtime DLLs + associate-ld.cmd and
           unassociate-ld.cmd) -> esses-cli zip (component cli + the runtime DLLs)
  linux    cmake --install into AppDir/usr -> linuxdeploy with its Qt plugin (+ the offscreen plugin) -> the
           copyright files of the system libraries it copied (dpkg) -> notices and SBOM -> vendor marks ->
           the AppImage -> esses-cli tarball
  macos    cmake --install -> Esses.app -> macdeployqt (+ the offscreen plugin) -> notices and SBOM -> vendor marks
           -> an ad hoc signature (arm64 runs nothing unsigned; not a developer signature, U-11) -> DMG (hdiutil)
           -> esses-cli tarball
and on every OS sha256sums-<platform>.txt over what it made. The trees stay under <out>/stage for the smoke
tests (smoke.py). Archives are deterministic: sorted entries, the time of SOURCE_DATE_EPOCH (else the build
date), no owner. --dry-run prints the commands and writes nothing. Standard library only.

Options: --os windows|linux|macos, --config <CMake config, default from the build>, --iscc <ISCC.exe>,
--vc-redist-dir <VC\\Redist\\MSVC\\<version> folder, default VCToolsRedistDir>, --no-installer,
--runtime-test-installer (also an installer that requires runtime 99.0.0, for smoke.py's refusal test; never
published), --linuxdeploy <linuxdeploy AppImage or binary> (its Qt plugin beside it or on PATH), --no-appimage,
--no-dmg. Exit status: 0 done, 1 a step failed, 2 usage error.
"""
from __future__ import annotations

import argparse
import datetime
import gzip
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
PLATFORM = {"windows": "win64", "linux": "linux-x86_64", "macos": "macos"}
CRT_PATTERNS = ("msvcp140", "vcruntime140", "concrt140")


class Failed(Exception):
    pass


def host_os() -> str:
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


# ---- names and numbers ---------------------------------------------------------------------------------------

def artefact_names(version: str, os_name: str) -> dict[str, str]:
    """The files a run makes in <out> (08 section 7: esses-<ver>-win64-setup.exe, ...)."""
    p = PLATFORM[os_name]
    names = {"sbom": f"esses-{version}-{p}.cdx.json", "cliSbom": f"esses-cli-{version}-{p}.cdx.json",
             "checksums": f"sha256sums-{p}.txt"}
    if os_name == "windows":
        names.update(installer=f"esses-{version}-win64-setup.exe",
                     runtimeTest=f"esses-{version}-win64-setup-runtime-test.exe",
                     portable=f"esses-{version}-win64-portable.zip", cli=f"esses-cli-{version}-win64.zip")
    elif os_name == "linux":
        names.update(appimage=f"esses-{version}-x86_64.AppImage", cli=f"esses-cli-{version}-linux-x86_64.tar.gz")
    else:
        names.update(dmg=f"esses-{version}-macos.dmg", cli=f"esses-cli-{version}-macos.tar.gz")
    return names


def file_version(info: dict) -> str:
    """a.b.c.n for the Windows version resources: n the commits since the tag ("0.1.0+3-g..."), 0 at a tag."""
    m = re.search(r"\+(\d+)-g", info["version"])
    return f"{info['versionMajor']}.{info['versionMinor']}.{info['versionPatch']}.{m.group(1) if m else 0}"


def redist_version(folder: pathlib.Path) -> str:
    """The version of a VC\\Redist\\MSVC\\<version> folder ("14.44.35112")."""
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)", folder.name)
    if not m:
        raise Failed(f"{folder}: not a VC\\Redist\\MSVC\\<version> folder")
    return ".".join(m.groups())


def archive_time() -> datetime.datetime:
    epoch = os.environ.get("SOURCE_DATE_EPOCH", "")
    if epoch.isdigit():
        return datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
    return datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)


# ---- archives and checksums ------------------------------------------------------------------------------------

def tree_files(src: pathlib.Path) -> list[pathlib.Path]:
    return sorted((p for p in src.rglob("*") if p.is_file() or p.is_symlink()), key=lambda p: p.as_posix())


def make_zip(src: pathlib.Path, dest: pathlib.Path, root: str) -> None:
    stamp = archive_time()
    date = (max(stamp.year, 1980), stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second)
    with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in tree_files(src):
            info = zipfile.ZipInfo(f"{root}/{p.relative_to(src).as_posix()}", date_time=date)
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if os.access(p, os.X_OK) and not p.suffix.lower() in (".dll", ".txt", ".md", ".json") else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            z.writestr(info, p.read_bytes())


def make_tar(src: pathlib.Path, dest: pathlib.Path, root: str) -> None:
    mtime = int(archive_time().timestamp())

    def clean(t: tarfile.TarInfo) -> tarfile.TarInfo:
        t.uid = t.gid = 0
        t.uname = t.gname = ""
        t.mtime = mtime
        if t.isfile():
            t.mode = 0o755 if t.mode & 0o111 else 0o644
        return t

    with open(dest, "wb") as raw:
        # The gzip header's time fixed too (tarfile's own gzip writer would stamp the current time).
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=mtime) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tar:
                for p in tree_files(src):
                    tar.add(p, arcname=f"{root}/{p.relative_to(src).as_posix()}", recursive=False, filter=clean)


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write_checksums(out: pathlib.Path, names: list[str], dest: pathlib.Path) -> None:
    lines = [f"{sha256(out / n)}  {n}\n" for n in sorted(names) if (out / n).is_file()]
    dest.write_text("".join(lines), encoding="utf-8", newline="\n")


# ---- running things ----------------------------------------------------------------------------------------------

class Runner:
    def __init__(self, dry: bool) -> None:
        self.dry = dry

    def run(self, cmd: list[str], env: dict[str, str] | None = None, cwd: pathlib.Path | None = None) -> None:
        print("+ " + " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd), flush=True)
        if self.dry:
            return
        full = dict(os.environ)
        full.update(env or {})
        r = subprocess.run([str(c) for c in cmd], env=full, cwd=cwd)
        if r.returncode != 0:
            raise Failed(f"{cmd[0]} exited with {r.returncode}")

    def output(self, cmd: list[str]) -> str:
        if self.dry:
            return ""
        r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
        return r.stdout if r.returncode == 0 else ""


def fresh(path: pathlib.Path, runner: Runner) -> pathlib.Path:
    if not runner.dry:
        if path.exists():
            shutil.rmtree(path)
        path.mkdir(parents=True)
    return path


def cmake_install(runner: Runner, build: pathlib.Path, prefix: pathlib.Path, component: str, config: str) -> None:
    cmd = ["cmake", "--install", str(build), "--prefix", str(prefix), "--component", component]
    if config:
        cmd += ["--config", config]
    runner.run(cmd)


def python_tool(runner: Runner, script: pathlib.Path, args: list[str]) -> None:
    runner.run([sys.executable, "-I", str(script)] + args)


def notices(runner: Runner, info_path: pathlib.Path, stage: pathlib.Path, package: str, os_name: str,
            md: pathlib.Path, sbom: pathlib.Path, vc_version: str = "", system: pathlib.Path | None = None) -> None:
    args = ["--root", str(REPO), "--stage", str(stage), "--package", package, "--os", os_name, "--info",
            str(info_path), "--out", str(md), "--sbom", str(sbom)]
    if vc_version:
        args += ["--vc-runtime-version", vc_version]
    if system is not None:
        args += ["--system-licences", str(system)]
    python_tool(runner, HERE / "gen_third_party.py", args)


def vendor_marks(runner: Runner, tree: pathlib.Path) -> None:
    python_tool(runner, REPO / "tools" / "check_vendor_marks.py", ["--tree", str(tree)])


def copy_file(runner: Runner, src: pathlib.Path, dest: pathlib.Path) -> None:
    print(f"+ copy {src} -> {dest}", flush=True)
    if not runner.dry:
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)


# ---- Windows -----------------------------------------------------------------------------------------------------

def find_crt(redist: pathlib.Path) -> pathlib.Path:
    candidates = sorted((redist / "x64").glob("Microsoft.VC14*.CRT"))
    if not candidates:
        raise Failed(f"{redist}: no x64\\Microsoft.VC14*.CRT folder")
    return candidates[-1]


def find_iscc(given: str) -> str:
    for c in ([given] if given else []) + [shutil.which("iscc") or "", shutil.which("ISCC") or "",
                                         r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"]:
        if c and pathlib.Path(c).is_file():
            return c
    raise Failed("Inno Setup's ISCC.exe was not found (--iscc)")


def iscc_command(iscc: str, info: dict, stage: pathlib.Path, out: pathlib.Path, vc_redist: pathlib.Path | None,
                 vc_version: str, crt: pathlib.Path | None, required: str = "", suffix: str = "") -> list[str]:
    cmd = [iscc, "/Qp", f"/DAppVersion={info['version']}", f"/DFileVersion={file_version(info)}",
           f"/DStageDir={stage}", f"/DRepoRoot={REPO}", f"/DOutputDir={out}"]
    if vc_redist is not None:
        cmd += [f"/DVcRedist={vc_redist}", f"/DVcRedistVersion={vc_version}"]
    if crt is not None:
        cmd.append(f"/DCrtDir={crt}")
    if required:
        cmd.append(f"/DVcRequired={required}")
    if suffix:
        cmd.append(f"/DOutputSuffix={suffix}")
    cmd.append(str(HERE / "windows" / "esses.iss"))
    return cmd


def package_windows(args, runner: Runner, info: dict, info_path: pathlib.Path, out: pathlib.Path) -> list[str]:
    names = artefact_names(info["version"], "windows")
    made: list[str] = []
    redist_dir = pathlib.Path(args.vc_redist_dir or os.environ.get("VCToolsRedistDir", "")).resolve() \
        if (args.vc_redist_dir or os.environ.get("VCToolsRedistDir")) else None
    if redist_dir is None or not redist_dir.is_dir():
        raise Failed("the build toolset's redistributable folder is unknown: run in a developer environment "
                     "(VCToolsRedistDir) or pass --vc-redist-dir")
    vc_version = redist_version(redist_dir)
    vc_redist = redist_dir / "vc_redist.x64.exe"
    crt = find_crt(redist_dir)
    stage = fresh(out / "stage" / "app" / f"esses-{info['version']}-win64", runner)
    cmake_install(runner, args.build, stage, "app", args.config)
    windeployqt = info.get("windeployqt") or str(pathlib.Path(info["qtBin"]) / "windeployqt.exe")
    # The PDF image-format plugin would bring Qt Pdf, and it and the TUIO touch plugin (generic/) Qt Network with
    # its TLS and network-information plugins; Esses uses none of them (D-212).
    deploy = [windeployqt, "--no-translations", "--no-system-d3d-compiler", "--no-system-dxc-compiler",
              "--no-compiler-runtime", "--no-opengl-sw", "--skip-plugin-types", "tls,networkinformation,generic",
              "--exclude-plugins", "qpdf,qpdfd",
              str(stage / "esses.exe"), str(stage / "esses-render.exe")]
    runner.run(deploy, env={"PATH": info["qtBin"] + os.pathsep + os.environ.get("PATH", "")})
    debug = info.get("config", "").lower() == "debug"
    copy_file(runner, pathlib.Path(info["qtPlugins"]) / "platforms" / ("qoffscreend.dll" if debug else "qoffscreen.dll"),
              stage / "platforms" / ("qoffscreend.dll" if debug else "qoffscreen.dll"))
    notices(runner, info_path, stage, "app", "windows", stage / "THIRD_PARTY_LICENSES.md", out / names["sbom"],
            vc_version)
    made.append(names["sbom"])
    vendor_marks(runner, stage)
    if not args.no_installer:
        iscc = find_iscc(args.iscc)
        runner.run(iscc_command(iscc, info, stage, out, vc_redist, vc_version, crt))
        made.append(names["installer"])
        if args.runtime_test_installer:
            runner.run(iscc_command(iscc, info, stage, out, vc_redist, vc_version, crt, "99.0.0", "-runtime-test"))
    # The portable zip: the same tree, the marker, the runtime beside the programs, the association scripts.
    portable_root = f"esses-{info['version']}-win64-portable"
    portable = fresh(out / "stage" / "portable" / portable_root, runner)
    print(f"+ copy {stage} -> {portable}", flush=True)
    if not runner.dry:
        shutil.copytree(stage, portable, dirs_exist_ok=True)
        (portable / "portable.marker").write_bytes(b"")
    for dll in sorted(crt.glob("*.dll")):
        copy_file(runner, dll, portable / dll.name)
    for script in ("associate-ld.cmd", "unassociate-ld.cmd"):
        copy_file(runner, HERE / "windows" / script, portable / script)
    print(f"+ zip {portable} -> {out / names['portable']}", flush=True)
    if not runner.dry:
        make_zip(portable, out / names["portable"], portable_root)
    made.append(names["portable"])
    # The command-line zip.
    cli_root = f"esses-cli-{info['version']}-win64"
    cli = fresh(out / "stage" / "cli" / cli_root, runner)
    cmake_install(runner, args.build, cli, "cli", args.config)
    for dll in sorted(crt.glob("*.dll")):
        copy_file(runner, dll, cli / dll.name)
    notices(runner, info_path, cli, "cli", "windows", cli / "THIRD_PARTY_LICENSES.md", out / names["cliSbom"],
            vc_version)
    made.append(names["cliSbom"])
    vendor_marks(runner, cli)
    print(f"+ zip {cli} -> {out / names['cli']}", flush=True)
    if not runner.dry:
        make_zip(cli, out / names["cli"], cli_root)
    made.append(names["cli"])
    return made


# ---- Linux -------------------------------------------------------------------------------------------------------

def system_library_licences(runner: Runner, appdir: pathlib.Path) -> dict[str, str]:
    """The Debian copyright file of every library linuxdeploy copied from the system, into
    usr/share/<dir>/licenses/system/<package>.copyright; returns {library: note}."""
    notes: dict[str, str] = {}
    lib = appdir / "usr" / "lib"
    target = appdir / "usr" / "share" / "esses" / "licenses" / "system"
    skip = re.compile(r"^(libQt6|libicu|libqtadvanceddocking)")
    for p in sorted(lib.glob("*.so*")) if lib.is_dir() else []:
        if skip.match(p.name) or p.is_symlink():
            continue
        owner = runner.output(["dpkg", "-S", p.name]).split(":")[0].strip()
        copyright_file = pathlib.Path("/usr/share/doc") / owner / "copyright" if owner else None
        if copyright_file is not None and copyright_file.is_file():
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(copyright_file, target / f"{owner}.copyright")
            notes[p.name] = f"Ubuntu package {owner}, licence in share/esses/licenses/system/{owner}.copyright"
        else:
            notes[p.name] = "from the build system (no package owner found)"
    return notes


def package_linux(args, runner: Runner, info: dict, info_path: pathlib.Path, out: pathlib.Path) -> list[str]:
    names = artefact_names(info["version"], "linux")
    made: list[str] = []
    stage_root = fresh(out / "stage" / "linux", runner)
    appdir = stage_root / "AppDir"
    cmake_install(runner, args.build, appdir / "usr", "app", args.config)
    linuxdeploy = args.linuxdeploy or shutil.which("linuxdeploy") or "linuxdeploy"
    if not args.no_appimage:
        env = {
            "QMAKE": info["qmake"],
            "EXTRA_PLATFORM_PLUGINS": "libqoffscreen.so",
            "LD_LIBRARY_PATH": os.pathsep.join(x for x in (info["binDir"], info["qtLib"],
                                                           os.environ.get("LD_LIBRARY_PATH", "")) if x),
            "APPIMAGE_EXTRACT_AND_RUN": "1",
        }
        bin_dir = appdir / "usr" / "bin"
        runner.run([linuxdeploy, "--appdir", str(appdir), "--plugin", "qt", "--executable", str(bin_dir / "esses"),
                    "--executable", str(bin_dir / "esses-render"), "--executable", str(bin_dir / "esses-cli")], env=env)
    system = stage_root / "system-licences.json"
    if not runner.dry:
        system.write_text(json.dumps(system_library_licences(runner, appdir), indent=2) + "\n", encoding="utf-8")
    notices(runner, info_path, appdir / "usr", "app", "linux",
            appdir / "usr" / "share" / info["dirName"] / "THIRD_PARTY_LICENSES.md", out / names["sbom"],
            system=system)
    made.append(names["sbom"])
    vendor_marks(runner, appdir)
    if not args.no_appimage:
        env = {"OUTPUT": str(out / names["appimage"]), "LINUXDEPLOY_OUTPUT_VERSION": info["version"],
               "APPIMAGE_EXTRACT_AND_RUN": "1", "QMAKE": info["qmake"]}
        runner.run([linuxdeploy, "--appdir", str(appdir), "--output", "appimage"], env=env)
        made.append(names["appimage"])
    made += package_cli_tar(args, runner, info, info_path, out, "linux", names)
    return made


def package_cli_tar(args, runner: Runner, info: dict, info_path: pathlib.Path, out: pathlib.Path, os_name: str,
                    names: dict[str, str]) -> list[str]:
    root = f"esses-cli-{info['version']}-{PLATFORM[os_name]}"
    cli = fresh(out / "stage" / "cli" / root, runner)
    cmake_install(runner, args.build, cli, "cli", args.config)
    if os_name == "macos":
        runner.run(["codesign", "--force", "--sign", "-", str(cli / "esses-cli")])
    notices(runner, info_path, cli, "cli", os_name, cli / "THIRD_PARTY_LICENSES.md", out / names["cliSbom"])
    vendor_marks(runner, cli)
    print(f"+ tar {cli} -> {out / names['cli']}", flush=True)
    if not runner.dry:
        make_tar(cli, out / names["cli"], root)
    return [names["cliSbom"], names["cli"]]


# ---- macOS -------------------------------------------------------------------------------------------------------

def package_macos(args, runner: Runner, info: dict, info_path: pathlib.Path, out: pathlib.Path) -> list[str]:
    names = artefact_names(info["version"], "macos")
    made: list[str] = []
    stage = fresh(out / "stage" / "macos", runner)
    cmake_install(runner, args.build, stage, "app", args.config)
    app = stage / "Esses.app"
    print(f"+ rename {stage / 'esses.app'} -> {app}", flush=True)
    if not runner.dry:
        (stage / "esses.app").rename(app)
    offscreen = app / "Contents" / "PlugIns" / "platforms" / "libqoffscreen.dylib"
    copy_file(runner, pathlib.Path(info["qtPlugins"]) / "platforms" / "libqoffscreen.dylib", offscreen)
    macos = app / "Contents" / "MacOS"
    macdeployqt = info.get("macdeployqt") or str(pathlib.Path(info["qtBin"]) / "macdeployqt")
    runner.run([macdeployqt, str(app), "-always-overwrite", f"-libpath={info['binDir']}",
                f"-executable={macos / 'esses-cli'}", f"-executable={macos / 'esses-render'}",
                f"-executable={offscreen}"])
    notices(runner, info_path, app, "app", "macos", app / "Contents" / "Resources" / "THIRD_PARTY_LICENSES.md",
            out / names["sbom"])
    made.append(names["sbom"])
    vendor_marks(runner, app)
    runner.run(["codesign", "--force", "--deep", "--sign", "-", str(app)])  # ad hoc, after the last change
    if not args.no_dmg:
        dmg_root = fresh(out / "stage" / "dmg", runner)
        print(f"+ copy {app} -> {dmg_root / 'Esses.app'}", flush=True)
        if not runner.dry:
            shutil.copytree(app, dmg_root / "Esses.app", symlinks=True)
            os.symlink("/Applications", dmg_root / "Applications")
        runner.run(["hdiutil", "create", "-volname", "Esses", "-srcfolder", str(dmg_root), "-ov", "-format", "UDZO",
                    str(out / names["dmg"])])
        made.append(names["dmg"])
    made += package_cli_tar(args, runner, info, info_path, out, "macos", names)
    return made


# ---- driver ------------------------------------------------------------------------------------------------------

def run_packaging(args) -> int:
    runner = Runner(args.dry_run)
    info_path = args.build / "packaging" / "package_info.json"
    if not info_path.is_file() and args.config:  # a multi-config generator writes one per configuration
        info_path = args.build / "packaging" / f"package_info-{args.config}.json"
    if not info_path.is_file():
        print(f"package.py: {info_path} not found (configure with ESSES_BUILD_QT=ON first)", file=sys.stderr)
        return 2
    info = json.loads(info_path.read_text(encoding="utf-8"))
    os_name = args.os or host_os()
    if info.get("os") != os_name:
        print(f"package.py: the build is for {info.get('os')}, not {os_name}", file=sys.stderr)
        return 2
    if not args.config:
        args.config = info.get("config", "")
    out = args.out.resolve()
    if not runner.dry:
        out.mkdir(parents=True, exist_ok=True)
    print(f"package.py: Esses {info['version']} (commit {info['commit']}, Qt {info['qtVersion']}, "
          f"{info.get('releaseTag') or 'development build'}) for {os_name} into {out}", flush=True)
    started = time.monotonic()
    try:
        made = {"windows": package_windows, "linux": package_linux, "macos": package_macos}[os_name](
            args, runner, info, info_path, out)
        names = artefact_names(info["version"], os_name)
        if not runner.dry:
            write_checksums(out, made, out / names["checksums"])
    except Failed as e:
        print(f"package.py: {e}", file=sys.stderr)
        return 1
    print(f"package.py: {', '.join(made)} in {time.monotonic() - started:.0f} s", flush=True)
    return 0


def self_test() -> int:
    ok = True
    n = artefact_names("0.1.0", "windows")
    ok &= n["installer"] == "esses-0.1.0-win64-setup.exe" and n["portable"] == "esses-0.1.0-win64-portable.zip"
    ok &= n["cli"] == "esses-cli-0.1.0-win64.zip" and n["checksums"] == "sha256sums-win64.txt"
    ok &= artefact_names("0.1.0", "linux")["appimage"] == "esses-0.1.0-x86_64.AppImage"
    ok &= artefact_names("0.1.0", "linux")["cli"] == "esses-cli-0.1.0-linux-x86_64.tar.gz"
    ok &= artefact_names("0.1.0", "macos")["dmg"] == "esses-0.1.0-macos.dmg"
    info = {"version": "0.1.0-m3+12-gabc1234", "versionMajor": 0, "versionMinor": 1, "versionPatch": 0}
    ok &= file_version(info) == "0.1.0.12"
    ok &= file_version({**info, "version": "0.1.0"}) == "0.1.0.0"
    ok &= redist_version(pathlib.Path("C:/VS/VC/Redist/MSVC/14.44.35112")) == "14.44.35112"
    try:
        redist_version(pathlib.Path("v145"))
        ok = False
    except Failed:
        pass
    cmd = iscc_command("ISCC.exe", {**info, "version": "0.1.0"}, pathlib.Path("S"), pathlib.Path("O"),
                       pathlib.Path("R/vc_redist.x64.exe"), "14.44.35112", pathlib.Path("CRT"), "99.0.0", "-runtime-test")
    ok &= "/DAppVersion=0.1.0" in cmd and "/DFileVersion=0.1.0.0" in cmd and "/DVcRequired=99.0.0" in cmd
    ok &= "/DOutputSuffix=-runtime-test" in cmd and cmd[-1].endswith("esses.iss")
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        src = t / "tree"
        (src / "data").mkdir(parents=True)
        (src / "esses.exe").write_bytes(b"MZ")
        (src / "data" / "units.json").write_text("{}\n", encoding="utf-8")
        (src / "portable.marker").write_bytes(b"")
        (src / "run.sh").write_text("#!/bin/sh\n", encoding="utf-8")
        os.chmod(src / "run.sh", 0o755)
        os.environ["SOURCE_DATE_EPOCH"] = "1760000000"
        make_zip(src, t / "a.zip", "root")
        make_zip(src, t / "b.zip", "root")
        ok &= (t / "a.zip").read_bytes() == (t / "b.zip").read_bytes()
        with zipfile.ZipFile(t / "a.zip") as z:
            ok &= z.namelist() == ["root/data/units.json", "root/esses.exe", "root/portable.marker", "root/run.sh"]
            ok &= z.read("root/portable.marker") == b""
        make_tar(src, t / "a.tar.gz", "root")
        make_tar(src, t / "b.tar.gz", "root")
        ok &= (t / "a.tar.gz").read_bytes() == (t / "b.tar.gz").read_bytes()
        with tarfile.open(t / "a.tar.gz") as tar:
            members = {m.name: m for m in tar.getmembers()}
            ok &= sorted(members) == ["root/data/units.json", "root/esses.exe", "root/portable.marker", "root/run.sh"]
            ok &= all(m.uid == 0 and m.mtime == 1760000000 for m in members.values())
            if os.name != "nt":
                ok &= members["root/run.sh"].mode == 0o755 and members["root/data/units.json"].mode == 0o644
        write_checksums(t, ["a.zip", "missing.zip"], t / "sums.txt")
        line = (t / "sums.txt").read_text(encoding="utf-8")
        ok &= re.fullmatch(r"[0-9a-f]{64}  a\.zip\n", line) is not None
        del os.environ["SOURCE_DATE_EPOCH"]
    print("package self-test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--os", choices=("windows", "linux", "macos"))
    parser.add_argument("--config", default="")
    parser.add_argument("--iscc", default="")
    parser.add_argument("--vc-redist-dir", default="")
    parser.add_argument("--no-installer", action="store_true")
    parser.add_argument("--runtime-test-installer", action="store_true")
    parser.add_argument("--linuxdeploy", default="")
    parser.add_argument("--no-appimage", action="store_true")
    parser.add_argument("--no-dmg", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.build or not args.out:
        parser.error("--build and --out are needed (or --self-test)")
    args.build = args.build.resolve()
    return run_packaging(args)


if __name__ == "__main__":
    sys.exit(main())
