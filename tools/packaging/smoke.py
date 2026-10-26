#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Smoke tests of the Esses packages (M4 packaging, D-212; AC-M4-04's runner part, AC-M4-09; 08 section 7).

Each command takes a package that package.py made, starts what is inside the way a user would and checks:
  * `esses --version`: the first line "esses <version>", then a commit (a hexadecimal sha), "Qt: ...", an ISO
    build date and the source URL (the tag's, `<repository>/tree/<tag>`, when --tag is given; the version must
    then be the tag without its "v": the installer's version equals the tag, AC-M4-09);
  * an offscreen start with a log on its command line: the program runs and records the log in the recent files
    of its settings.json, in the folder the package promises (the per-user folder; <exe dir>/config for the
    portable zip), and is then ended;
  * the files every package must carry (NOTICES.md, THIRD_PARTY_LICENSES.md, the licence texts, the CLI, the
    offscreen platform plugin) and, per package, what makes it that package.
Commands:
  windows-installer --installer <setup.exe> [--runtime-test <setup-runtime-test.exe>]  silent install into a
        temporary folder with the .ld association task, the uninstall entry's DisplayVersion, the association, the
        programs; an upgrade over it (files an earlier package had and this one has not, app-local runtime DLLs
        included, are gone; a .ld association another program took meanwhile stays; D-217), uninstall; with
        --runtime-test: an administrative install that requires a runtime no machine has is refused (exit code 7,
        nothing installed) and a per-user one installs the runtime DLLs beside esses.exe (08 section 7.1, the
        installer's runtime check), and its uninstall leaves a .ld association another program owns
  windows-portable --zip <portable.zip>     unzip into a folder whose name holds & and %, settings beside the
        program, nothing written under %APPDATA%\\esses (compared file by file when the folder exists already;
        D-217), associate-ld.cmd and unassociate-ld.cmd for real (HKCU), with decoys of reg, find and the like in
        their folder and first on PATH, and .ld kept by a program that took it (D-218 (b); --no-registry leaves
        them out, on a developer's machine); a failure prints the script's exit code and output and the registry
        values read
  appimage --appimage <file>                --version and an offscreen start (APPIMAGE_EXTRACT_AND_RUN), the
        extracted tree
  dmg --dmg <file> [--macos-minimum <x.y>]  mounted read-only (hdiutil), the bundle, the programs, Info.plist: the
        executable, the version, LSMinimumSystemVersion = --macos-minimum (the deployment target package.yml
        builds for, 14.0 by default), .ld as an alternate handler of the log type the bundle exports (not
        imports; D-217)
  cli --archive <zip or tar.gz>             the standalone esses-cli
Common: --version <version> [--tag <vX.Y.Z>] [--fixture <a log file>]. --self-test checks the checks.
--script-test --reg-stub <esses_reg_stub.exe> [--scripts <folder>] (ctest packaging.association_scripts, Windows;
        elsewhere exit 77, skipped): associate-ld.cmd and unassociate-ld.cmd from awkward folder names, against the
        stand-in reg.exe (windows/reg_stub.cpp) as %SystemRoot%\\System32\\reg.exe of a temporary SystemRoot, with
        decoys in their folder and first on PATH; the registry is never touched (D-218 (b)).
Standard library only. Exit status: 0 passed, 1 a check failed, 2 usage error, 77 skipped (--script-test).
"""
from __future__ import annotations

import argparse
import itertools
import os
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
REPOSITORY_URL = "https://github.com/mitchdetailed/jubilant-octo-meme"
APP_ID = "{66CC7557-E26B-4E57-9DB2-CA0B18ADA06E}"  # tools/packaging/windows/esses.iss
DEFAULT_FIXTURE = REPO / "fixtures" / "generic.csv" / "sparse-gps" / "input.csv"


class Report:
    def __init__(self) -> None:
        self.problems: list[str] = []

    def check(self, condition: bool, what: str) -> bool:
        print(("ok      " if condition else "FAILED  ") + what, flush=True)
        if not condition:
            self.problems.append(what)
        return condition


# ---- checks shared by every package ---------------------------------------------------------------------------

def version_problems(text: str, version: str, tag: str = "") -> list[str]:
    """What is wrong with an `esses --version` report (about_dialog.hpp versionReport, AC-M4-09)."""
    lines = text.replace("\r\n", "\n").strip("\n").split("\n")
    problems = []
    if not lines or lines[0] != f"esses {version}":
        problems.append(f"first line '{lines[0] if lines else ''}' is not 'esses {version}'")
    fields = dict(line.split(": ", 1) for line in lines[1:] if ": " in line)
    if not re.fullmatch(r"[0-9a-f]{7,40}(-dirty)?", fields.get("commit", "")):
        problems.append(f"commit '{fields.get('commit', '')}' is not a git sha (cmake/version.cmake stamps "
                        "'unknown' only for a build without a git checkout or commit; D-216 (a))")
    if not re.fullmatch(r"\d+\.\d+\.\d+\S* \(built with \d+\.\d+\.\d+\S*\)", fields.get("Qt", "")):
        problems.append(f"Qt '{fields.get('Qt', '')}' is not '<version> (built with <version>)'")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", fields.get("build date", "")):
        problems.append(f"build date '{fields.get('build date', '')}' is not an ISO date")
    if not fields.get("dock library"):
        problems.append("no dock library line")
    source = fields.get("source", "")
    if tag:
        if version != tag[1:]:
            problems.append(f"the version {version} is not the tag {tag}")
        if source != f"{REPOSITORY_URL}/tree/{tag}":
            problems.append(f"source '{source}' is not the tag's URL")
    elif not source.startswith(REPOSITORY_URL):
        problems.append(f"source '{source}' is not the repository")
    return problems


def child_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["ESSES_INSTANCE_NAME"] = "esses-smoke-" + uuid.uuid4().hex  # never forwards to another run
    for v in ("ESSES_CONFIG_DIR", "ESSES_CACHE_DIR", "ESSES_LOG_DIR", "ESSES_DATA_DIR"):
        env.pop(v, None)
    env.update(extra or {})
    return env


def run_version(report: Report, exe: list[str], version: str, tag: str, env: dict[str, str] | None = None) -> None:
    try:
        r = subprocess.run(exe + ["--version"], capture_output=True, text=True, timeout=180, env=child_env(env))
    except (OSError, subprocess.TimeoutExpired) as e:
        report.check(False, f"{exe[-1]} --version runs ({e})")
        return
    report.check(r.returncode == 0, f"{exe[-1]} --version exits 0 (got {r.returncode}; {r.stderr.strip()[:200]})")
    print(r.stdout.strip(), flush=True)
    for p in version_problems(r.stdout, version, tag):
        report.check(False, f"--version: {p}")
    if not version_problems(r.stdout, version, tag):
        report.check(True, "--version reports version, commit, Qt, build date, dock library and source")


def settings_mention(settings: pathlib.Path, needle: str) -> bool:
    try:
        return needle in settings.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def offscreen_start(report: Report, exe: list[str], fixture: pathlib.Path, settings: pathlib.Path,
                    env: dict[str, str] | None = None, cwd: pathlib.Path | None = None, timeout: float = 240.0) -> None:
    """Starts the program offscreen with a log; passes once settings.json names the log while it still runs."""
    needle = fixture.name
    proc = subprocess.Popen(exe + ["-l", str(fixture)], env=child_env(env), cwd=cwd,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + timeout
    seen = False
    try:
        while time.monotonic() < deadline:
            if settings_mention(settings, needle):
                seen = True
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        running = proc.poll() is None
        report.check(seen and running, f"an offscreen start opens {needle} and records it in {settings}"
                     + ("" if running else f" (the program ended with {proc.returncode})"))
    finally:
        if proc.poll() is None:
            proc.kill()
        try:
            out, _ = proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            out = b""
        if not seen:
            print(out.decode("utf-8", errors="replace")[-4000:], flush=True)


def has_files(report: Report, root: pathlib.Path, rels: list[str], what: str) -> None:
    """Every entry exists; an entry "a|b" is satisfied by either (a Debug build's Qt plugins end in "d")."""
    missing = [r for r in rels if not any((root / alt).exists() for alt in r.split("|"))]
    report.check(not missing, f"{what} holds {', '.join(rels)}" + (f" (missing: {', '.join(missing)})" if missing else ""))


def notices_files(share: str) -> list[str]:
    return [f"{share}NOTICES.md", f"{share}THIRD_PARTY_LICENSES.md", f"{share}LICENSE",
            f"{share}licenses/LGPL-3.0-only.txt", f"{share}licenses/GPL-3.0-only.txt",
            f"{share}licenses/LGPL-2.1-or-later.txt", f"{share}licenses/MIT.txt", f"{share}data/keymaps/default.json"]


# ---- macOS -------------------------------------------------------------------------------------------------------

def info_plist_problems(plist: dict, version: str, minimum: str) -> list[str]:
    """What is wrong with Esses.app's Info.plist (tools/packaging/macos/Info.plist.in, D-212, D-217)."""
    problems = []
    if plist.get("CFBundleExecutable") != "esses":
        problems.append(f"CFBundleExecutable '{plist.get('CFBundleExecutable')}' is not 'esses'")
    m = re.match(r"\d+\.\d+\.\d+", version)
    numeric = m.group(0) if m else version
    if plist.get("CFBundleShortVersionString") != numeric:
        problems.append(f"CFBundleShortVersionString '{plist.get('CFBundleShortVersionString')}' is not '{numeric}'")
    if plist.get("LSMinimumSystemVersion") != minimum:
        problems.append(f"LSMinimumSystemVersion '{plist.get('LSMinimumSystemVersion')}' is not the deployment "
                        f"target '{minimum}'")
    handlers = [d for d in plist.get("CFBundleDocumentTypes", []) if "ld" in d.get("CFBundleTypeExtensions", [])]
    if not handlers or any(d.get("LSHandlerRank") != "Alternate" for d in handlers):
        problems.append(".ld is not opened as an alternate handler (LSHandlerRank Alternate)")

    def declares_ld(declarations: list) -> list[str]:
        return [d.get("UTTypeIdentifier", "") for d in declarations
                if "ld" in d.get("UTTypeTagSpecification", {}).get("public.filename-extension", [])]

    exported = declares_ld(plist.get("UTExportedTypeDeclarations", []))
    if len(exported) != 1:
        problems.append(f"{len(exported)} exported type declarations of .ld instead of 1 (the log type is Esses's own)")
    if declares_ld(plist.get("UTImportedTypeDeclarations", [])):
        problems.append("the .ld type is imported (it is Esses's own, so it is exported)")
    for d in handlers:
        if exported and exported[0] not in d.get("LSItemContentTypes", []):
            problems.append(f"the .ld handler's LSItemContentTypes {d.get('LSItemContentTypes', [])} do not name the "
                            f"exported type '{exported[0]}'")
    return problems


# ---- Windows ----------------------------------------------------------------------------------------------------

def reg_get(root, path: str, name: str = "") -> str | None:
    import winreg
    try:
        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            return str(winreg.QueryValueEx(key, name)[0])
    except OSError:
        return None


def reg_describe(root, path: str, name: str = "") -> str:
    """A registry value for a report: its data and type, "no value" or "no key"."""
    import winreg
    try:
        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            try:
                data, kind = winreg.QueryValueEx(key, name)
            except OSError:
                return "no value"
            return f"{data!r} (type {kind})"
    except OSError:
        return "no key"


def wait_gone(path: pathlib.Path, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not path.exists():
            return True
        time.sleep(1.0)
    return not path.exists()


def appdata_settings() -> pathlib.Path:
    return pathlib.Path(os.environ["APPDATA"]) / "esses" / "settings.json"


def folder_snapshot(folder: pathlib.Path) -> dict[str, tuple[int, int]] | None:
    """Every file under `folder` with its size and modification time (ns); None when there is no folder."""
    if not folder.is_dir():
        return None
    out: dict[str, tuple[int, int]] = {}
    for f in folder.rglob("*"):
        try:
            if f.is_file():
                st = f.stat()
                out[f.relative_to(folder).as_posix()] = (st.st_size, st.st_mtime_ns)
        except OSError:
            continue
    return out


def snapshot_changes(before: dict[str, tuple[int, int]] | None,
                     after: dict[str, tuple[int, int]] | None) -> list[str]:
    """What a run wrote into a folder: the folder created, files added, changed or removed (D-217)."""
    if after is None:
        return [] if before is None else ["the folder was removed"]
    if before is None:
        return ["the folder was created"] + sorted(after)
    return sorted([f"added {k}" for k in after.keys() - before.keys()] +
                  [f"changed {k}" for k in after.keys() & before.keys() if after[k] != before[k]] +
                  [f"removed {k}" for k in before.keys() - after.keys()])


def reg_set_default(root, path: str, value: str | None) -> None:
    """Sets (or, with None, deletes) the default value of a key (the smoke test's stand-in .ld owner)."""
    import winreg
    if value is None:
        try:
            with winreg.OpenKey(root, path, 0, winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
                winreg.DeleteValue(key, "")
        except OSError:
            pass
        return
    with winreg.CreateKeyEx(root, path, 0, winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
        winreg.SetValueEx(key, "", 0, winreg.REG_SZ, value)


OTHER_OWNER = "Esses.SmokeOtherProgram"  # a ProgId that stands for another program owning .ld


def windows_installer(report: Report, args) -> None:
    import winreg
    uninstall_key = rf"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{APP_ID}_is1"
    with tempfile.TemporaryDirectory() as tmp:
        target = pathlib.Path(tmp) / "Esses"
        log = pathlib.Path(tmp) / "setup.log"
        r = subprocess.run([str(args.installer), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/DIR={target}",
                            "/MERGETASKS=assocld", f"/LOG={log}"], timeout=900)
        report.check(r.returncode == 0, f"silent install exits 0 (got {r.returncode})")
        shown = reg_get(winreg.HKEY_LOCAL_MACHINE, uninstall_key, "DisplayVersion")
        report.check(shown == args.version, f"the uninstall entry's DisplayVersion is {args.version} (got {shown})")
        if args.tag:
            report.check(shown == args.tag[1:], f"the installer's version equals the tag {args.tag} (AC-M4-09)")
        has_files(report, target, ["esses.exe", "esses-cli.exe", "esses-render.exe", "Qt6Core.dll|Qt6Cored.dll",
                                   "platforms/qwindows.dll|platforms/qwindowsd.dll",
                                   "platforms/qoffscreen.dll|platforms/qoffscreend.dll", "unins000.exe"]
                  + notices_files(""), "the installation")
        report.check(any(target.glob("qtadvanceddocking-qt6*.dll")), "the docking library is installed")
        exe = target / "esses.exe"
        report.check(reg_get(winreg.HKEY_LOCAL_MACHINE, r"Software\Classes\.ld") == "Esses.LogFile",
                     ".ld is associated with Esses.LogFile")
        command = reg_get(winreg.HKEY_LOCAL_MACHINE, r"Software\Classes\Esses.LogFile\shell\open\command")
        report.check(command == f'"{exe}" -l "%1"', f"the open command is \"esses.exe\" -l \"%1\" (got {command})")
        run_version(report, [str(exe)], args.version, args.tag)
        r = subprocess.run([str(target / "esses-cli.exe"), "--version"], capture_output=True, text=True, timeout=120)
        report.check(r.returncode == 0 and args.version in r.stdout, "esses-cli --version names the version")
        offscreen_start(report, [str(exe)], args.fixture, appdata_settings())
        # An upgrade over it (D-217): what an earlier package had and this one has not goes (app-local runtime DLLs
        # of an earlier per-user install on an old runtime, a Qt plugin or module no longer deployed), and a .ld
        # association that another program took after the first install is not taken back (no /MERGETASKS).
        stale = [target / "msvcp140.dll", target / "vcruntime140_1.dll", target / "platforms" / "qstale.dll",
                 target / "Qt6Stale.dll"]
        for f in stale:
            f.write_bytes(b"stale")
        ld_key = r"Software\Classes\.ld"
        reg_set_default(winreg.HKEY_LOCAL_MACHINE, ld_key, OTHER_OWNER)
        r = subprocess.run([str(args.installer), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/DIR={target}",
                            f"/LOG={pathlib.Path(tmp) / 'upgrade.log'}"], timeout=900)
        report.check(r.returncode == 0, f"a silent upgrade over it exits 0 (got {r.returncode})")
        left = [f.relative_to(target).as_posix() for f in stale if f.exists()]
        report.check(not left, "the upgrade removes what this package does not install" +
                     (f" (left: {', '.join(left)})" if left else ""))
        report.check((target / "platforms").is_dir() and exe.exists(), "the upgrade installs the package again")
        report.check(reg_get(winreg.HKEY_LOCAL_MACHINE, ld_key) == OTHER_OWNER,
                     "the upgrade leaves .ld with the program that took it")
        reg_set_default(winreg.HKEY_LOCAL_MACHINE, ld_key, "Esses.LogFile")
        subprocess.run([str(target / "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=300)
        report.check(wait_gone(exe, 300), "uninstall removes esses.exe")
        report.check(reg_get(winreg.HKEY_LOCAL_MACHINE, uninstall_key, "DisplayVersion") is None,
                     "uninstall removes the uninstall entry")
        report.check(reg_get(winreg.HKEY_LOCAL_MACHINE, r"Software\Classes\.ld") != "Esses.LogFile",
                     "uninstall removes the .ld association")
    if args.runtime_test:
        runtime_test(report, args)


def runtime_test(report: Report, args) -> None:
    """The installer built with VcRequired=99.0.0: no machine's runtime is recent enough."""
    import winreg
    with tempfile.TemporaryDirectory() as tmp:
        admin = pathlib.Path(tmp) / "admin"
        log = pathlib.Path(tmp) / "admin.log"
        r = subprocess.run([str(args.runtime_test), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/DIR={admin}",
                            f"/LOG={log}"], timeout=900)
        text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
        report.check(r.returncode == 7, f"an administrative install whose runtime stays too old is refused "
                                        f"(exit code 7, got {r.returncode})")
        report.check("vc_redist.x64.exe exit code" in text, "it ran the bundled vc_redist.x64.exe first")
        report.check(not (admin / "esses.exe").exists(), "the refused install copied nothing")
        user = pathlib.Path(tmp) / "user"
        r = subprocess.run([str(args.runtime_test), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER",
                            f"/DIR={user}", f"/LOG={pathlib.Path(tmp) / 'user.log'}"], timeout=900)
        report.check(r.returncode == 0, f"a per-user install exits 0 (got {r.returncode})")
        has_files(report, user, ["esses.exe", "msvcp140.dll", "vcruntime140.dll"],
                  "the per-user installation (with the runtime beside esses.exe)")
        run_version(report, [str(user / "esses.exe")], args.version, args.tag)
        key = rf"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{APP_ID}_is1"
        report.check(reg_get(winreg.HKEY_CURRENT_USER, key, "DisplayVersion") == args.version,
                     "the per-user uninstall entry names the version")
        if (user / "unins000.exe").exists():
            # Another program owns .ld when Esses is uninstalled: its association stays (D-217).
            ld_key = r"Software\Classes\.ld"
            previous = reg_get(winreg.HKEY_CURRENT_USER, ld_key)
            reg_set_default(winreg.HKEY_CURRENT_USER, ld_key, OTHER_OWNER)
            subprocess.run([str(user / "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=300)
            report.check(wait_gone(user / "esses.exe", 300), "the per-user uninstall removes esses.exe")
            report.check(reg_get(winreg.HKEY_CURRENT_USER, ld_key) == OTHER_OWNER,
                         "the uninstall leaves a .ld association another program owns")
            reg_set_default(winreg.HKEY_CURRENT_USER, ld_key, previous)


# ---- Windows: the portable zip's association scripts (D-218 (b)) --------------------------------------------------

LD_KEY = r"Software\Classes\.ld"
OPEN_WITH_KEY = r"Software\Classes\.ld\OpenWithProgids"
PROGID = "Esses.LogFile"
PROGID_KEY = r"Software\Classes\Esses.LogFile"
COMMAND_KEY = r"Software\Classes\Esses.LogFile\shell\open\command"
# Programs of Windows that a script could call by a bare name, which cmd looks up in the current folder and then
# along PATH before it reaches System32 (Git for Windows' usr\bin has find, sort and timeout of its own).
DECOY_NAMES = ("reg", "find", "findstr", "sort", "more", "where", "timeout", "choice", "icacls")


def windows_program(name: str) -> str:
    """One of Windows's programs by its full path (%SystemRoot%\\System32), never looked up along PATH."""
    return os.path.join(os.environ.get("SYSTEMROOT", r"C:\Windows"), "System32", name)


def plant_decoys(here: pathlib.Path, path_first: pathlib.Path) -> None:
    """A decoy of each of DECOY_NAMES in the folder a script runs in and in a folder that goes first on PATH: each
    one records that it ran in the file ESSES_DECOY_LOG names and fails. A script that takes Windows's programs from
    System32 by their full path never starts one."""
    for folder, where in ((here, "the current folder"), (path_first, "the first folder on PATH")):
        folder.mkdir(parents=True, exist_ok=True)
        for name in DECOY_NAMES:
            (folder / f"{name}.bat").write_bytes(
                f'@>>"%ESSES_DECOY_LOG%" echo {name}.bat from {where}\r\n@exit /b 1\r\n'.encode("ascii"))


def script_env(path_first: pathlib.Path, decoy_log: pathlib.Path,
               extra: dict[str, str] | None = None) -> dict[str, str]:
    """The environment of an association script under test: the decoys first on PATH and, as cmd searches by default,
    the current folder before PATH (NoDefaultCurrentDirectoryInExePath removed)."""
    env = {k: v for k, v in os.environ.items() if k.upper() != "NODEFAULTCURRENTDIRECTORYINEXEPATH"}
    env["PATH"] = str(path_first) + os.pathsep + env.get("PATH", "")
    env["ESSES_DECOY_LOG"] = str(decoy_log)
    env.update(extra or {})
    return env


def run_script(script: pathlib.Path, env: dict[str, str]) -> subprocess.CompletedProcess:
    """As Explorer runs a script (cmd /c ""<path>""; quoted whole, whatever the folder's name), in its folder."""
    return subprocess.run(f'"{windows_program("cmd.exe")}" /d /c ""{script}""', cwd=script.parent, env=env,
                          capture_output=True, timeout=60)


def script_output(r: subprocess.CompletedProcess) -> str:
    def text(b: bytes) -> str:
        return b.decode("oem" if os.name == "nt" else "latin-1", errors="replace").strip()
    return f"exit code {r.returncode}, stdout {text(r.stdout)!r}, stderr {text(r.stderr)!r}"


def decoys_started(decoy_log: pathlib.Path) -> list[str]:
    try:
        return [line.strip() for line in decoy_log.read_text(encoding="ascii", errors="replace").splitlines()
                if line.strip()]
    except OSError:
        return []


def association_state() -> list[str]:
    """What the association scripts read and write under HKCU, for a failure's report."""
    import winreg
    hkcu = winreg.HKEY_CURRENT_USER
    return [f"HKCU\\{LD_KEY} (default): {reg_describe(hkcu, LD_KEY)}",
            f"HKCU\\{OPEN_WITH_KEY} {PROGID}: {reg_describe(hkcu, OPEN_WITH_KEY, PROGID)}",
            f"HKCU\\{PROGID_KEY} (default): {reg_describe(hkcu, PROGID_KEY)}",
            f"HKCU\\{COMMAND_KEY} (default): {reg_describe(hkcu, COMMAND_KEY)}"]


def portable_associations(report: Report, root: pathlib.Path, exe: pathlib.Path, work: pathlib.Path) -> None:
    """associate-ld.cmd and unassociate-ld.cmd for real (HKCU) as Explorer runs them, with decoys of reg, find and
    the other programs of DECOY_NAMES in the folder and first on PATH (D-218 (b): a GNU find first on PATH kept .ld
    associated). A failed check prints what the script printed, its exit code, the registry values read and the
    decoys that ran. .ld's default is put back as it was."""
    import winreg
    hkcu = winreg.HKEY_CURRENT_USER
    decoy_log = work / "decoys.log"
    plant_decoys(root, work / "decoys")
    env = script_env(work / "decoys", decoy_log)
    previous = reg_get(hkcu, LD_KEY)

    def has_open_with() -> bool:
        return reg_describe(hkcu, OPEN_WITH_KEY, PROGID) not in ("no key", "no value")

    def step(script: str, expect, what: str) -> None:
        r = run_script(root / script, env)
        if not report.check(r.returncode == 0 and expect(), what):
            print(f"        {script}: {script_output(r)}", flush=True)
            for line in association_state():
                print(f"        {line}", flush=True)
            for line in decoys_started(decoy_log):
                print(f"        decoy started: {line}", flush=True)

    def associated() -> bool:
        return (reg_get(hkcu, LD_KEY) == PROGID and has_open_with()
                and reg_get(hkcu, COMMAND_KEY) == f'"{exe}" -l "%1"')

    try:
        step("associate-ld.cmd", associated, "associate-ld.cmd associates .ld with this esses.exe (ProgId, open "
             "command of the whole path, OpenWithProgids, .ld's default)")
        # Another program took .ld meanwhile: it keeps it, and the rest of Esses's association goes (D-218 (b)).
        reg_set_default(hkcu, LD_KEY, OTHER_OWNER)
        step("unassociate-ld.cmd",
             lambda: reg_get(hkcu, LD_KEY) == OTHER_OWNER and reg_get(hkcu, COMMAND_KEY) is None
             and not has_open_with(),
             "unassociate-ld.cmd leaves .ld with the program that took it and removes the rest")
        step("associate-ld.cmd", associated, "associate-ld.cmd associates .ld again")
        step("unassociate-ld.cmd",
             lambda: reg_get(hkcu, LD_KEY) is None and reg_get(hkcu, COMMAND_KEY) is None and not has_open_with(),
             "unassociate-ld.cmd removes it (.ld's default, OpenWithProgids, the ProgId)")
        started = decoys_started(decoy_log)
        report.check(not started, "the scripts take reg from System32, never a program of that name from their "
                                  "folder or PATH" + (f" (started: {'; '.join(started)})" if started else ""))
    finally:
        reg_set_default(hkcu, LD_KEY, previous)


def portable_associations_self_test() -> bool:
    """portable_associations() over a registry held in a dict, with Python stand-ins for the two scripts that act
    out their effects on it: no script runs and nothing reaches the registry, so it passes with any version of the
    scripts (they are --script-test's). It passes stand-ins that do the scripts' job, and fails one that acts out
    fe6cf07's unassociate-ld.cmd under a GNU find first on PATH (.ld's default left, the decoy find started),
    printing the registry values it read."""
    import contextlib
    import io
    store: dict[tuple[str, str], str] = {}

    def fake_get(root, path: str, name: str = "") -> str | None:
        return store.get((path.lower(), name))

    def fake_describe(root, path: str, name: str = "") -> str:
        if (path.lower(), name) in store:
            return f"{store[(path.lower(), name)]!r} (type 1)"
        return "no value" if any(k[0] == path.lower() for k in store) else "no key"

    def fake_set_default(root, path: str, value: str | None) -> None:
        if value is None:
            store.pop((path.lower(), ""), None)
        else:
            store[(path.lower(), "")] = value

    def scripts(keep_default: bool):
        def run(script: pathlib.Path, env: dict[str, str]) -> subprocess.CompletedProcess:
            if script.name == "associate-ld.cmd":
                store.update({(PROGID_KEY.lower(), ""): "Esses log file", (OPEN_WITH_KEY.lower(), PROGID): "",
                              (COMMAND_KEY.lower(), ""): f'"{script.parent / "esses.exe"}" -l "%1"',
                              (LD_KEY.lower(), ""): PROGID})
                return subprocess.CompletedProcess([], 0, b".ld files now open with ...\r\n", b"")
            if keep_default:  # `find` is GNU find: "find: 'Esses.LogFile': No such file or directory"
                with open(env["ESSES_DECOY_LOG"], "a", encoding="ascii") as f:
                    f.write("find.bat from the first folder on PATH\n")
            elif store.get((LD_KEY.lower(), "")) == PROGID:
                del store[(LD_KEY.lower(), "")]
            store.pop((OPEN_WITH_KEY.lower(), PROGID), None)
            for key in [k for k in store if k[0].startswith(PROGID_KEY.lower())]:
                del store[key]
            return subprocess.CompletedProcess([], 0, b"the .ld association of Esses is removed\r\n", b"")
        return run

    ok = True
    saved = {n: globals()[n] for n in ("reg_get", "reg_describe", "reg_set_default", "run_script")}
    try:
        globals().update(reg_get=fake_get, reg_describe=fake_describe, reg_set_default=fake_set_default)
        for keep_default in (False, True):
            store.clear()
            globals()["run_script"] = scripts(keep_default)
            report = Report()
            out = io.StringIO()
            with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(out):
                root = pathlib.Path(tmp) / "R&D 100% tools" / "esses-portable"
                root.mkdir(parents=True)
                portable_associations(report, root, root / "esses.exe", pathlib.Path(tmp))
            text = out.getvalue()
            if not keep_default:
                ok &= report.problems == [] and store == {}
            else:
                ok &= len(report.problems) == 2 and report.problems[0].startswith("unassociate-ld.cmd removes it")
                ok &= f"HKCU\\{LD_KEY} (default): 'Esses.LogFile' (type 1)" in text and "exit code 0" in text
                ok &= "decoy started: find.bat from the first folder on PATH" in text and store == {}
            if not ok:
                print(f"self-test: portable_associations (keep_default={keep_default}): {report.problems}\n{text}")
    finally:
        globals().update(saved)
    return ok


def stub_query_listing(data: str | None, name: str = "(Default)") -> bytes | None:
    """What `reg query HKCU\\Software\\Classes\\.ld /ve` prints for a REG_SZ default value (None: no output, the key
    is absent). The default value's name is in the language of Windows: "(Default)", "(Standard)", a French
    one with a space in it."""
    if data is None:
        return None
    return f"\r\nHKEY_CURRENT_USER\\{LD_KEY}\r\n    {name}    REG_SZ    {data}\r\n\r\n".encode("cp850")


def association_script_problems(reg_stub: pathlib.Path, scripts: pathlib.Path) -> list[str]:
    """associate-ld.cmd and unassociate-ld.cmd under cmd, as Explorer runs them, from awkward folder names, against
    reg_stub (windows/reg_stub.cpp) as <a temporary SystemRoot>\\System32\\reg.exe, so nothing reaches the registry,
    and with decoys of reg, find and the rest of DECOY_NAMES in the script's folder and first on PATH (D-218 (b)):
    what is wrong with the reg commands each one runs."""
    import json
    problems: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp).resolve()
        system_root = t / "SystemRoot"
        (system_root / "System32").mkdir(parents=True)
        reg = system_root / "System32" / "reg.exe"
        shutil.copy2(reg_stub, reg)
        empty_root = t / "NoReg"
        (empty_root / "System32").mkdir(parents=True)
        decoy_log = t / "decoys.log"
        reg_log = t / "reg.log"
        query = t / "query.txt"
        counter = itertools.count()

        def run(folder: pathlib.Path, script: str, listing: bytes | None = None, fail: str = "",
                root: pathlib.Path = system_root) -> tuple[subprocess.CompletedProcess, list[list[str]]]:
            for f in (reg_log, query):
                f.unlink(missing_ok=True)
            if listing is not None:
                query.write_bytes(listing)
            env = script_env(t / "decoys", decoy_log, {"SYSTEMROOT": str(root), "ESSES_REG_STUB_LOG": str(reg_log),
                                                       "ESSES_REG_STUB_QUERY": str(query)})
            env.pop("ESSES_REG_STUB_FAIL", None)
            if fail:
                env["ESSES_REG_STUB_FAIL"] = fail
            r = run_script(folder / script, env)
            calls = [json.loads(line) for line in reg_log.read_text(encoding="utf-8").splitlines()] \
                if reg_log.exists() else []
            for call in calls:
                if not os.path.samefile(call[0], reg):
                    problems.append(f"{script} in '{folder.name}' ran {call[0]}, not %SystemRoot%\\System32\\reg.exe")
            return r, [call[1:] for call in calls]

        def folder_named(name: str) -> pathlib.Path:
            folder = t / f"{next(counter)}" / name
            folder.mkdir(parents=True)
            (folder / "esses.exe").write_bytes(b"MZ")
            for s in ("associate-ld.cmd", "unassociate-ld.cmd"):
                shutil.copy2(scripts / s, folder / s)
            plant_decoys(folder, t / "decoys")
            return folder

        def expect(what: str, r: subprocess.CompletedProcess, ok: bool) -> None:
            if not ok:
                problems.append(f"{what}: {script_output(r)}")

        ours = [["delete", f"HKCU\\{OPEN_WITH_KEY}", "/v", PROGID, "/f"], ["delete", f"HKCU\\{PROGID_KEY}", "/f"]]
        query_call = ["query", f"HKCU\\{LD_KEY}", "/ve"]
        drop_default = ["delete", f"HKCU\\{LD_KEY}", "/ve", "/f"]
        # "50% of 100%": a pair of % that names no variable (a defined one, as in "%PATH%", is expanded by the cmd /c
        # that starts any script, Explorer's included, before the script runs).
        for name in ("Esses portable", "R&D 100% tools!", "R&D 100% (tools)", "a^b (c) !d", "50% of 100%",
                     "Donn\u00e9es \u00fc"):
            folder = folder_named(name)
            exe = str(folder / "esses.exe")
            r, calls = run(folder, "associate-ld.cmd")
            wanted = [["add", f"HKCU\\{PROGID_KEY}", "/ve", "/d", "Esses log file", "/f"],
                      ["add", f"HKCU\\{PROGID_KEY}\\DefaultIcon", "/ve", "/d", f'"{exe}",0', "/f"],
                      ["add", f"HKCU\\{COMMAND_KEY}", "/ve", "/d", f'"{exe}" -l "%1"', "/f"],
                      ["add", f"HKCU\\{OPEN_WITH_KEY}", "/v", PROGID, "/t", "REG_NONE", "/f"],
                      ["add", f"HKCU\\{LD_KEY}", "/ve", "/d", PROGID, "/f"]]
            expect(f"associate-ld.cmd from '{name}'", r, r.returncode == 0 and not r.stderr.strip())
            if calls != wanted:
                problems.append(f"associate-ld.cmd from '{name}' ran reg {calls} instead of {wanted}")
            r, calls = run(folder, "unassociate-ld.cmd", stub_query_listing(PROGID))
            expect(f"unassociate-ld.cmd from '{name}'", r, r.returncode == 0 and not r.stderr.strip())
            if calls != [query_call, drop_default] + ours:
                problems.append(f"unassociate-ld.cmd from '{name}' ran reg {calls}")
        folder = folder_named("R&D 100% tools!")
        # .ld's default is removed only while it is exactly Esses.LogFile, in any language of Windows.
        for listing, owned in ((stub_query_listing(PROGID, "(Standard)"), True),
                               (stub_query_listing(PROGID, "(par d\u00e9faut)"), True),
                               (stub_query_listing("Other.Program"), False),
                               (stub_query_listing("Esses.LogFileOld"), False),  # `find` took these two for Esses's
                               (stub_query_listing("MyEsses.LogFile"), False),
                               (stub_query_listing("Esses.LogFile!"), False),  # a lost ! would make it Esses's
                               (stub_query_listing('A&B|C<D>E^F%PATH%"G'), False),
                               (stub_query_listing("(value not set)"), False),
                               (None, False)):
            r, calls = run(folder, "unassociate-ld.cmd", listing)
            what = f"unassociate-ld.cmd over {listing!r}"
            expect(what, r, r.returncode == 0 and not r.stderr.strip())
            if calls != [query_call] + ([drop_default] if owned else []) + ours:
                problems.append(f"{what} ran reg {calls}")
        # Failures: reported, with the exit code.
        r, calls = run(folder, "unassociate-ld.cmd", stub_query_listing(PROGID), fail="delete")
        expect("unassociate-ld.cmd when the removal of .ld's default fails", r,
               r.returncode == 1 and b"could not be removed" in r.stderr and calls == [query_call, drop_default] + ours)
        r, calls = run(folder, "associate-ld.cmd", fail="add")
        expect("associate-ld.cmd when reg add fails", r, r.returncode == 1 and len(calls) == 1)
        (folder / "esses.exe").unlink()
        r, calls = run(folder, "associate-ld.cmd")
        expect("associate-ld.cmd without esses.exe beside it", r, r.returncode == 1 and b"esses.exe" in r.stderr
               and not calls)
        for script in ("associate-ld.cmd", "unassociate-ld.cmd"):
            r, calls = run(folder, script, stub_query_listing(PROGID), root=empty_root)
            expect(f"{script} where System32 has no reg.exe", r, r.returncode == 1 and b"reg.exe" in r.stderr
                   and not calls)
        problems += [f"a decoy ran: {line}" for line in decoys_started(decoy_log)]
    return problems


def script_test(args) -> int:
    """ctest packaging.association_scripts: association_script_problems() with the build's stand-in reg.exe."""
    if os.name != "nt":
        print("skipped: associate-ld.cmd and unassociate-ld.cmd are Windows cmd scripts (they run on Windows only)")
        return 77
    if args.reg_stub is None:
        print("skipped: --script-test needs --reg-stub <the build's esses_reg_stub.exe> (ctest passes it)")
        return 77
    problems = association_script_problems(args.reg_stub.resolve(), (args.scripts or HERE / "windows").resolve())
    for problem in problems:
        print(f"script-test: {problem}")
    print("smoke script-test: " + ("passed" if not problems else f"FAILED ({len(problems)} problem(s))"))
    return 1 if problems else 0


def windows_portable(report: Report, args) -> None:
    appdata = appdata_settings().parent
    before = folder_snapshot(appdata)  # compared file by file afterwards, also when the folder exists (D-217)
    with tempfile.TemporaryDirectory() as tmp:
        # A folder name with & and % (a user's "R&D 100% tools"): the association scripts quote the path (D-217).
        unzip = pathlib.Path(tmp) / "R&D 100% tools"
        unzip.mkdir()
        with zipfile.ZipFile(args.zip) as z:
            z.extractall(unzip)
        roots = [p for p in unzip.iterdir() if p.is_dir()]
        report.check(len(roots) == 1, "the zip holds one folder")
        root = roots[0]
        report.check(root.name == f"esses-{args.version}-win64-portable", f"the folder is named for the version")
        has_files(report, root, ["portable.marker", "esses.exe", "esses-cli.exe", "msvcp140.dll", "vcruntime140.dll",
                                 "vcruntime140_1.dll", "associate-ld.cmd", "unassociate-ld.cmd",
                                 "platforms/qoffscreen.dll|platforms/qoffscreend.dll"] + notices_files(""),
                  "the portable folder")
        exe = root / "esses.exe"
        run_version(report, [str(exe)], args.version, args.tag)
        offscreen_start(report, [str(exe)], args.fixture, root / "config" / "settings.json", cwd=root)
        changes = snapshot_changes(before, folder_snapshot(appdata))
        report.check(not changes, f"the portable run wrote nothing to {appdata}" +
                     (f" ({'; '.join(changes[:5])})" if changes else
                      "" if before is None else f" (compared with the {len(before)} file(s) there before)"))
        if args.no_registry:
            print("skipped  the association scripts (--no-registry: this machine's associations stay as they are; "
                  "ctest packaging.association_scripts runs them against a stand-in reg.exe)")
            return
        portable_associations(report, root, exe, pathlib.Path(tmp))


# ---- Linux and macOS -----------------------------------------------------------------------------------------------

def appimage(report: Report, args) -> None:
    image = args.appimage.resolve()
    image.chmod(image.stat().st_mode | 0o111)
    env = {"APPIMAGE_EXTRACT_AND_RUN": "1"}
    run_version(report, [str(image)], args.version, args.tag, env)
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        r = subprocess.run([str(image), "--appimage-extract"], cwd=t, capture_output=True, timeout=600)
        root = t / "squashfs-root"
        report.check(r.returncode == 0 and root.is_dir(), "the AppImage extracts")
        has_files(report, root, ["AppRun", "esses.desktop", "usr/bin/esses", "usr/bin/esses-cli", "usr/bin/esses-render",
                                 "usr/share/applications/esses.desktop", "usr/share/mime/packages/esses.xml",
                                 "usr/plugins/platforms/libqoffscreen.so", "usr/plugins/platforms/libqxcb.so"]
                  + notices_files("usr/share/esses/"), "the AppImage")
        cli = subprocess.run([str(root / "usr" / "bin" / "esses-cli"), "--version"], capture_output=True, text=True,
                             timeout=120)
        report.check(cli.returncode == 0 and args.version in cli.stdout, "the AppImage's esses-cli runs")
        config = t / "config"
        offscreen_start(report, [str(image)], args.fixture, config / "esses" / "settings.json",
                        {**env, "XDG_CONFIG_HOME": str(config), "XDG_CACHE_HOME": str(t / "cache"),
                         "XDG_STATE_HOME": str(t / "state")})


def dmg(report: Report, args) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        mount = t / "mnt"
        mount.mkdir()
        r = subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", str(mount), str(args.dmg)],
                           capture_output=True, text=True, timeout=300)
        if not report.check(r.returncode == 0, f"the DMG mounts ({r.stderr.strip()[:200]})"):
            return
        try:
            app = mount / "Esses.app"
            has_files(report, app, ["Contents/Info.plist", "Contents/MacOS/esses", "Contents/MacOS/esses-cli",
                                    "Contents/MacOS/esses-render", "Contents/Resources/esses.icns",
                                    "Contents/PlugIns/platforms/libqcocoa.dylib",
                                    "Contents/PlugIns/platforms/libqoffscreen.dylib"]
                      + notices_files("Contents/Resources/"), "Esses.app")
            report.check((mount / "Applications").is_symlink(), "the DMG links /Applications")
            with open(app / "Contents" / "Info.plist", "rb") as f:
                plist = plistlib.load(f)
            for p in info_plist_problems(plist, args.version, args.macos_minimum):
                report.check(False, f"Info.plist: {p}")
            if not info_plist_problems(plist, args.version, args.macos_minimum):
                report.check(True, f"Info.plist: the executable, the version, LSMinimumSystemVersion "
                                   f"{args.macos_minimum}, .ld as an alternate handler of the exported log type")
            v = subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], capture_output=True, text=True,
                               timeout=300)
            report.check(v.returncode == 0, f"the bundle's (ad hoc) signature verifies ({v.stderr.strip()[:200]})")
            exe = app / "Contents" / "MacOS" / "esses"
            run_version(report, [str(exe)], args.version, args.tag)
            home = t / "home"
            offscreen_start(report, [str(exe)], args.fixture,
                            home / "Library" / "Application Support" / "esses" / "settings.json", {"HOME": str(home)})
        finally:
            subprocess.run(["hdiutil", "detach", str(mount), "-force"], capture_output=True, timeout=300)


def cli_archive(report: Report, args) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        if args.archive.name.endswith(".zip"):
            with zipfile.ZipFile(args.archive) as z:
                z.extractall(t)
        else:
            with tarfile.open(args.archive) as tar:
                tar.extractall(t, filter="data")
        roots = [p for p in t.iterdir() if p.is_dir()]
        report.check(len(roots) == 1 and roots[0].name.startswith(f"esses-cli-{args.version}-"),
                     "the archive holds one esses-cli-<version>-<platform> folder")
        root = roots[0]
        exe = root / ("esses-cli.exe" if os.name == "nt" else "esses-cli")
        has_files(report, root, [exe.name, "NOTICES.md", "THIRD_PARTY_LICENSES.md", "LICENSE", "licenses/MIT.txt",
                                 "data/units.json", "data/keymaps/default.json"], "the command-line archive")
        report.check(not (root / "licenses" / "LGPL-3.0-only.txt").exists(), "it carries no Qt licence (no Qt)")
        r = subprocess.run([str(exe), "--version"], capture_output=True, text=True, timeout=120)
        report.check(r.returncode == 0 and args.version in r.stdout, f"esses-cli --version names {args.version}")
        r = subprocess.run([str(exe), "validate", str(root / "data" / "keymaps" / "default.json")],
                           capture_output=True, text=True, timeout=120)
        report.check(r.returncode == 0, "esses-cli validates a shipped document with the shipped schemas")


# ---- driver -------------------------------------------------------------------------------------------------------

def self_test() -> int:
    # The association scripts themselves, from awkward folder names (D-217), are --script-test's (ctest
    # packaging.association_scripts), which needs the build's stand-in reg.exe.
    ok = True
    if os.name == "nt":  # windows-portable's checks of the scripts' work and its report of a failure (D-218 (b))
        ok &= portable_associations_self_test()
    good = ("esses 0.1.0\ncommit: 56d8065\nQt: 6.12.0 (built with 6.12.0)\nbuild date: 2026-10-19\n"
            "dock library: ADS 5.1.1\nsource: https://github.com/mitchdetailed/jubilant-octo-meme/tree/v0.1.0\n")
    ok &= version_problems(good, "0.1.0", "v0.1.0") == []
    ok &= version_problems(good.replace("\n", "\r\n"), "0.1.0", "v0.1.0") == []
    ok &= len(version_problems(good, "0.1.0", "v0.1.1")) == 2  # the version and the source differ from the tag
    ok &= any("git sha" in p for p in version_problems(good.replace("56d8065", "unknown"), "0.1.0", "v0.1.0"))
    ok &= any("ISO" in p for p in version_problems(good.replace("2026-10-19", "today"), "0.1.0"))
    dev = good.replace("/tree/v0.1.0", "/commit/56d8065").replace("esses 0.1.0", "esses 0.1.0+3-g56d8065")
    ok &= version_problems(dev, "0.1.0+3-g56d8065") == []
    ok &= any("first line" in p for p in version_problems(dev, "0.1.0"))
    # The Info.plist checks of dmg (D-217) over the template as CMake configures it, then broken copies.
    values = {"MACOSX_BUNDLE_EXECUTABLE_NAME": "esses", "MACOSX_BUNDLE_ICON_FILE": "esses.icns",
              "MACOSX_BUNDLE_GUI_IDENTIFIER": "com.github.mitchdetailed.esses", "MACOSX_BUNDLE_BUNDLE_NAME": "Esses",
              "MACOSX_BUNDLE_SHORT_VERSION_STRING": "0.1.0", "MACOSX_BUNDLE_BUNDLE_VERSION": "0.1.0.0",
              "MACOSX_BUNDLE_LONG_VERSION_STRING": "0.1.0", "MACOSX_BUNDLE_COPYRIGHT": "the Esses contributors",
              "ESSES_MACOS_MINIMUM": "14.0"}
    template = (HERE / "macos" / "Info.plist.in").read_text(encoding="utf-8")
    configured = re.sub(r"\$\{(\w+)\}", lambda m: values.get(m.group(1), "UNSET-" + m.group(1)), template)
    ok &= "UNSET-" not in configured
    plist = plistlib.loads(configured.encode("utf-8"))
    problems = info_plist_problems(plist, "0.1.0", "14.0")
    for problem in problems:
        print(f"self-test: Info.plist.in: {problem}")
    ok &= problems == []
    ok &= info_plist_problems(plist, "0.1.0+3-g56d8065", "14.0") == []
    ok &= any("CFBundleShortVersionString" in p for p in info_plist_problems(plist, "0.2.0", "14.0"))
    ok &= any("LSMinimumSystemVersion" in p for p in info_plist_problems(plist, "0.1.0", "15.0"))
    ok &= any("LSMinimumSystemVersion" in p
              for p in info_plist_problems({k: v for k, v in plist.items() if k != "LSMinimumSystemVersion"},
                                           "0.1.0", "14.0"))
    imported = {k: v for k, v in plist.items() if k != "UTExportedTypeDeclarations"}
    imported["UTImportedTypeDeclarations"] = plist["UTExportedTypeDeclarations"]  # the type before D-217
    ok &= len(info_plist_problems(imported, "0.1.0", "14.0")) == 2  # not exported, and imported
    owner = plistlib.loads(configured.replace("<string>Alternate</string>", "<string>Owner</string>").encode("utf-8"))
    ok &= any("alternate" in p for p in info_plist_problems(owner, "0.1.0", "14.0"))
    other = plistlib.loads(configured.encode("utf-8"))
    other["CFBundleDocumentTypes"][0]["LSItemContentTypes"] = ["public.data"]
    ok &= any("LSItemContentTypes" in p for p in info_plist_problems(other, "0.1.0", "14.0"))
    with tempfile.TemporaryDirectory() as tmp:  # the %APPDATA% comparison of windows-portable (D-217)
        folder = pathlib.Path(tmp) / "esses"
        ok &= snapshot_changes(None, folder_snapshot(folder)) == []
        folder.mkdir()
        (folder / "settings.json").write_text("{}", encoding="utf-8")
        (folder / "layout.json").write_text("{}", encoding="utf-8")
        before = folder_snapshot(folder)
        ok &= snapshot_changes(before, folder_snapshot(folder)) == []
        (folder / "settings.json").write_text('{"recent": []}', encoding="utf-8")
        (folder / "new.json").write_text("{}", encoding="utf-8")
        (folder / "layout.json").unlink()
        ok &= snapshot_changes(before, folder_snapshot(folder)) == [
            "added new.json", "changed settings.json", "removed layout.json"]
        ok &= snapshot_changes(None, folder_snapshot(folder))[0] == "the folder was created"
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        settings = t / "settings.json"
        # A stand-in program: records the log it was given after a moment, then keeps running.
        script = t / "fake.py"
        script.write_text("import pathlib, sys, time\n"
                          "time.sleep(1)\n"
                          f"pathlib.Path({str(settings)!r}).write_text('{{\"recent\": [' + repr(sys.argv[-1]) + ']}}')\n"
                          "time.sleep(60)\n", encoding="utf-8")
        log = t / "a session.csv"
        report = Report()
        offscreen_start(report, [sys.executable, "-I", str(script)], log, settings, timeout=60)
        ok &= report.problems == []
        # A program that ends without recording anything fails the check.
        quitter = t / "quit.py"
        quitter.write_text("raise SystemExit(3)\n", encoding="utf-8")
        report = Report()
        offscreen_start(report, [sys.executable, "-I", str(quitter)], log, t / "none.json", timeout=60)
        ok &= len(report.problems) == 1
    print("smoke self-test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", nargs="?",
                        choices=("windows-installer", "windows-portable", "appimage", "dmg", "cli"))
    parser.add_argument("--version")
    parser.add_argument("--tag", default="")
    parser.add_argument("--fixture", type=pathlib.Path, default=DEFAULT_FIXTURE)
    parser.add_argument("--installer", type=pathlib.Path)
    parser.add_argument("--runtime-test", type=pathlib.Path)
    parser.add_argument("--zip", type=pathlib.Path)
    parser.add_argument("--appimage", type=pathlib.Path)
    parser.add_argument("--dmg", type=pathlib.Path)
    parser.add_argument("--macos-minimum", default="14.0",
                        help="dmg: the LSMinimumSystemVersion expected (the build's CMAKE_OSX_DEPLOYMENT_TARGET)")
    parser.add_argument("--archive", type=pathlib.Path)
    parser.add_argument("--no-registry", action="store_true",
                        help="windows-portable: leave the association scripts out (a developer's machine)")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--script-test", action="store_true",
                        help="the portable zip's association scripts against a stand-in reg.exe (Windows; exit 77: "
                             "skipped)")
    parser.add_argument("--reg-stub", type=pathlib.Path, help="--script-test: the build's esses_reg_stub.exe")
    parser.add_argument("--scripts", type=pathlib.Path,
                        help="--script-test: the folder of the scripts (default: tools/packaging/windows)")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.script_test:
        return script_test(args)
    if not args.command or not args.version:
        parser.error("a command and --version are needed (or --self-test, --script-test)")
    needed = {"windows-installer": "installer", "windows-portable": "zip", "appimage": "appimage", "dmg": "dmg",
              "cli": "archive"}[args.command]
    if getattr(args, needed) is None:
        parser.error(f"{args.command} needs --{needed}")
    args.fixture = args.fixture.resolve()
    report = Report()
    {"windows-installer": windows_installer, "windows-portable": windows_portable, "appimage": appimage, "dmg": dmg,
     "cli": cli_archive}[args.command](report, args)
    print(f"smoke {args.command}: {len(report.problems)} failure(s)")
    return 1 if report.problems else 0


if __name__ == "__main__":
    sys.exit(main())
