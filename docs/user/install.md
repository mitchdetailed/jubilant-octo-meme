# Installing Esses

Every release on the project's GitHub Releases page has these downloads, with a `sha256sums.txt` that lists the
checksum of each package:

| Download | For | Status |
|---|---|---|
| `esses-<version>-win64-setup.exe` | Windows 10 (22H2) and Windows 11, 64-bit | Supported |
| `esses-<version>-win64-portable.zip` | The same, without installing (a USB stick, a locked-down PC) | Supported |
| The Linux AppImage (x86-64) | Linux at least as new as Ubuntu 24.04 (built and tested on it; an older system such as Ubuntu 22.04 may not start it) | Builds green, not supported |
| `esses-<version>-macos.dmg` | macOS 14.4 or later, Apple silicon or Intel (launch-tested on macOS 15) | Builds green, not supported |
| `esses-cli` on its own | The command-line tool for scripts, per operating system | As its platform |

"Builds green, not supported" means the package is built and launch-tested by the project's automated checks on
every release, but gets little or no testing by hand, and problems on it do not hold up a release. Reports are
still welcome.

The 0.1 packages are not code-signed, so each operating system warns the first time you run them (below).

## Windows installer

1. Run `esses-<version>-win64-setup.exe`. Windows SmartScreen may say it protected your PC because the file is
   unsigned: choose **More info**, then **Run anyway**. On Windows 11 with **Smart App Control** turned on, Windows
   blocks unsigned programs and offers no Run anyway, for the installer and the portable zip alike. Esses then
   runs only with Smart App Control turned off (Windows Security > App & browser control > Smart App Control
   settings). That setting applies to every program, and on a Windows 11 without the latest updates it cannot be
   turned on again without reinstalling Windows, so decide with that in mind.
2. The installer offers two tasks:
   - **Associate .ld log files with Esses**: double-clicking a `.ld` file then opens it in Esses. If another
     program already opens `.ld` files, the box starts unticked so that program keeps them unless you tick it.
     `.ldx` files are never associated; they are read together with their `.ld` file.
   - A desktop icon.
3. Esses is installed under `Program Files\Esses`, with `esses.exe`, the command-line tool `esses-cli.exe`, the
   licence notices (`NOTICES.md`, `THIRD_PARTY_LICENSES.md`) and this guide (`docs\user\`).

**The Microsoft Visual C++ runtime.** Esses needs the Visual C++ 2015-2022 runtime (x64) at least as new as the one
its installer carries (14.40 or later; the installer names the exact version it needs in its log). An older copy
that is already installed counts as present for many installers but makes Esses crash at start-up, so the Esses
installer checks the installed version against its own. Installed for all users (the default, which asks for
administrator rights), it installs the runtime that ships with it when the installed one is missing or older, and
stops without installing anything if the runtime is still too old afterwards. Installed for your user only ("Install
for me only", no administrator rights), it puts its own copy of the runtime files beside `esses.exe` instead.

To remove Esses, use Settings > Apps (or Add or Remove Programs). Your settings and projects stay (see
[Troubleshooting](troubleshooting.md#where-esses-keeps-its-files)).

## Windows portable zip

1. Extract `esses-<version>-win64-portable.zip` into any folder you can write to, for example on a USB stick.
2. Run `esses.exe` from that folder. SmartScreen may warn as above.

The zip contains a file named `portable.marker` next to `esses.exe`. While it is there, Esses keeps its settings,
schemes, key bindings and recently-used lists in the `config` folder beside `esses.exe` (and its cache in `cache`)
instead of in your user profile, so the folder can move from PC to PC. The Visual C++ runtime files are included
in the zip, so it starts on a PC without the runtime or with an old one. Nothing is written to the registry
unless you ask for it: `associate-ld.cmd` in the folder associates `.ld` files with this copy of Esses for your user,
and `unassociate-ld.cmd` removes that association again (run it before you delete or move the folder). To remove
Esses, delete the folder.

## Linux AppImage (builds green, not supported)

1. Make the downloaded file executable: `chmod +x <file>.AppImage`.
2. Run it. If it does not start, your system may lack FUSE 2, which AppImages use: install it (on Ubuntu 24.04
   the package is `libfuse2t64`), or run the AppImage with `--appimage-extract-and-run`.

The AppImage contains `esses-cli` as well; it is also offered as a separate `tar.gz`.

## macOS disk image (builds green, not supported)

1. Open the `.dmg` and drag Esses to Applications.
2. The app is not notarised, so the first start is refused. On macOS 14, Control-click (or right-click) Esses in
   Finder, choose **Open**, then **Open** again in the dialog. On macOS 15 or later, try to open Esses once, then
   open System Settings > Privacy & Security, choose **Open Anyway** next to the message about Esses, and confirm.
   Later starts work normally.

## Starting Esses from a command line

```
esses [-w <project folder>] [-l <log file>]... [<file>...] [--new-instance] [--scheme <name>] [--keymap <name|file>]
esses --version
```

- `-w <folder>` (or `--workspace`) opens a project folder (the folder that holds `project.json`).
- `-l <file>` (or `--log`) opens a log file; repeat it for several files.
- A file name given without an option is opened as a log, or as a project when it is a project folder or its
  `project.json`.
- When Esses is already running for your user (with the same settings folder), a second start hands its files
  to the running window, which opens them and comes to the front, and the second start ends; started without
  files, it just brings the running window forward. `--new-instance` opens a separate window instead. A portable
  copy and an installed Esses have different settings folders, so each opens its own window.
- `--scheme <name>` starts with that colour scheme (a scheme's name as the toolbar's scheme selector lists it, or
  `light` or `dark`); it stays the active scheme.
- `--keymap <name|file>` uses that key-binding file on top of the default key bindings for this run only (a file
  path, or the name of a file in the `keymaps` folder of your settings); your own settings are not changed.
- `--scheme` and `--keymap` apply to a window that starts: when Esses is already running, the files go to the
  running window, which keeps its own scheme and key bindings, and the terminal says so; add `--new-instance` to
  start a window with them.
- A problem with `--scheme` or `--keymap` is a warning in the Messages window; Esses starts anyway.
- If a second start cannot reach the running window (another program holds the channel), it opens a window of its
  own, and that window's Messages say so.
- `--version` prints the version and exits. On Windows `esses.exe` is a windowed program; the version still
  prints in the terminal you started it from. `esses-cli --version` does the same from the command-line tool.

The command-line tool `esses-cli` inspects files without the window: `esses-cli info <file>`, `channels`, `laps`,
`eval` (evaluate a maths expression over a log), `export` (write a CSV), `validate` and more. Run
`esses-cli --help` for the list, and `esses-cli <command> --help` for a command's options.

## Version check

Help > About Esses shows the version, the commit it was built from and the Qt version, with the Qt licence notice
and where to get the source. Quote the version when you report a problem.
