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
        programs, uninstall; with --runtime-test: an administrative install that requires a runtime no machine
        has is refused (exit code 7, nothing installed) and a per-user one installs the runtime DLLs beside
        esses.exe (08 section 7.1, the installer's runtime check)
  windows-portable --zip <portable.zip>     unzip, settings beside the program, nothing under %APPDATA%\\esses,
        associate-ld.cmd and unassociate-ld.cmd (--no-registry leaves them out, on a developer's machine)
  appimage --appimage <file>                --version and an offscreen start (APPIMAGE_EXTRACT_AND_RUN), the
        extracted tree
  dmg --dmg <file>                          mounted read-only (hdiutil), Info.plist, the bundle, the programs
  cli --archive <zip or tar.gz>             the standalone esses-cli
Common: --version <version> [--tag <vX.Y.Z>] [--fixture <a log file>]. --self-test checks the checks.
Standard library only. Exit status: 0 passed, 1 a check failed, 2 usage error.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import plistlib
import re
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


# ---- Windows ----------------------------------------------------------------------------------------------------

def reg_get(root, path: str, name: str = "") -> str | None:
    import winreg
    try:
        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            return str(winreg.QueryValueEx(key, name)[0])
    except OSError:
        return None


def wait_gone(path: pathlib.Path, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not path.exists():
            return True
        time.sleep(1.0)
    return not path.exists()


def appdata_settings() -> pathlib.Path:
    return pathlib.Path(os.environ["APPDATA"]) / "esses" / "settings.json"


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
            subprocess.run([str(user / "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=300)
            report.check(wait_gone(user / "esses.exe", 300), "the per-user uninstall removes esses.exe")


def windows_portable(report: Report, args) -> None:
    import winreg
    appdata = appdata_settings().parent
    existed = appdata.exists()
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(args.zip) as z:
            z.extractall(tmp)
        roots = [p for p in pathlib.Path(tmp).iterdir() if p.is_dir()]
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
        if not existed:
            report.check(not appdata.exists(), f"the portable run wrote nothing to {appdata}")
        if args.no_registry:
            print("skipped  the association scripts (--no-registry: this machine's associations stay as they are)")
            return
        r = subprocess.run(["cmd", "/c", str(root / "associate-ld.cmd")], capture_output=True, text=True, timeout=60)
        command = reg_get(winreg.HKEY_CURRENT_USER, r"Software\Classes\Esses.LogFile\shell\open\command")
        report.check(r.returncode == 0 and command == f'"{exe}" -l "%1"',
                     f"associate-ld.cmd associates .ld with this esses.exe (got {command})")
        subprocess.run(["cmd", "/c", str(root / "unassociate-ld.cmd")], capture_output=True, text=True, timeout=60)
        report.check(reg_get(winreg.HKEY_CURRENT_USER, r"Software\Classes\Esses.LogFile\shell\open\command") is None
                     and reg_get(winreg.HKEY_CURRENT_USER, r"Software\Classes\.ld") != "Esses.LogFile",
                     "unassociate-ld.cmd removes it")


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
            numeric = re.match(r"\d+\.\d+\.\d+", args.version).group(0)
            report.check(plist.get("CFBundleExecutable") == "esses", "Info.plist: CFBundleExecutable esses")
            report.check(plist.get("CFBundleShortVersionString") == numeric,
                         f"Info.plist: CFBundleShortVersionString {numeric}")
            types = plist.get("CFBundleDocumentTypes", [])
            report.check(any("ld" in d.get("CFBundleTypeExtensions", []) and d.get("LSHandlerRank") == "Alternate"
                             for d in types), "Info.plist: .ld as an alternate handler")
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
    ok = True
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
    parser.add_argument("--archive", type=pathlib.Path)
    parser.add_argument("--no-registry", action="store_true",
                        help="windows-portable: leave the association scripts out (a developer's machine)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.command or not args.version:
        parser.error("a command and --version are needed (or --self-test)")
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
