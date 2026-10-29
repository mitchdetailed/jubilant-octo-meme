# Esses

Esses is a free, open-source desktop application for analysing motorsport data logs. Open the day's log files,
pick the laps to compare, look at them in graphs, reports and gauges, and add your own maths channels. It reads
`.ld` log files (with their `.ldx` companions) and CSV exports, and its maths expressions use the syntax of
MoTeC i2.

**Status: 0.1, the first public alpha.** 0.1 covers a day at the circuit: opening the logs, comparing laps, graphs,
reports and maths. It is young, so expect rough edges and please report them. The Windows packages are supported.
The Linux and macOS packages are built and launch-tested by the automated checks on every release but get little
or no testing by hand: they are labelled "builds green, not supported".

## Download

Get the latest release from the [Releases page](https://github.com/mitchdetailed/jubilant-octo-meme/releases):

| Download | For | Status |
|---|---|---|
| `esses-<version>-win64-setup.exe` | Windows 10 (22H2) and Windows 11, 64-bit | Supported |
| `esses-<version>-win64-portable.zip` | The same, without installing (a USB stick, a locked-down PC) | Supported |
| `esses-<version>-x86_64.AppImage` | x86-64 Linux at least as new as Ubuntu 24.04 (built and tested on it; an older system such as Ubuntu 22.04 may not start it) | Builds green, not supported |
| `esses-<version>-macos.dmg` | macOS 14.4 or later, Apple silicon or Intel (launch-tested on macOS 15) | Builds green, not supported |
| `esses-cli-<version>-<platform>` | The command-line tool on its own | As its platform |

The 0.1 packages are not code-signed. Windows SmartScreen may ask before the first start: choose **More info**,
then **Run anyway**. Windows 11's Smart App Control, when it is turned on, blocks unsigned programs without that
choice ([Installing Esses](docs/user/install.md#windows-installer) says what you can do). On macOS 14, Control-click
Esses in Finder, choose **Open**, then **Open** again; on macOS 15 or later, try to open Esses once, then choose
**Open Anyway** in System Settings > Privacy & Security. Each release has a `sha256sums.txt` with the checksums of
its packages. [Installing Esses](docs/user/install.md) has the details, including the portable mode and the
command line.

## What 0.1 does

- **Files:** open several `.ld` files (merged with their `.ldx`) and CSV files at once. An Import Log reports what
  was read, a Mapping panel fixes unknown units and channel names, and a file that changes on disk can be
  reloaded.
- **Laps:** the Data window, with a Main lap and overlays, filters, lap times, the difference to the fastest lap,
  offsets and trust flags.
- **Worksheets:** workbooks of worksheets with time and distance graphs, an outing graph, scatter plots,
  histograms, channel and time reports, track position, a values list and numeric and bar gauges. Cursor, zoom and
  datum are linked across components. A default circuit workbook is included.
- **Maths:** global and local maths channels, with live diagnostics in the Maths Editor and a Channel Status window
  that shows where each channel comes from.
- **Tracks:** corrected speed and distance, lap stretching, and a Track Editor for track maps and sections.
- **Projects:** a project folder that autosaves and reopens with the same laps selected.
- **Keyboard:** a complete key-binding table and a command palette (CTRL+SHIFT+P).
- **Command line:** `esses-cli` reads and checks files without the window: `info`, `channels`, `laps`, `eval`,
  `export`, `convert`, `validate` and more.

0.1 does not yet import workbooks, maths or settings files from other analysis software (you rebuild your
worksheets), and it has no indexed file browser, no printing, no GPS correction dialog and no Drag or Rally
profiles. The [user guide](docs/user/index.md) describes what is there; F1 opens it from the application.

## Building from source

You need CMake 3.28 or later, Ninja and a C++20 compiler (CI builds with Visual Studio 2022, GCC 13 and the
Apple Clang of macOS 15). The Qt-free core and `esses-cli` need nothing else:

```
cmake --preset linux-gcc-core          # or mac-clang-core; win-msvc-core from an x64 Native Tools prompt
cmake --build --preset linux-gcc-core
ctest --preset linux-gcc-core
```

The graphical application also needs Qt 6.7 or later with the Svg, PrintSupport and Test modules and Qt Gui's
private headers (part of the kits that the Qt online installer and aqtinstall install; a Linux distribution's Qt has
them in a separate package, such as `qt6-base-private-dev`). The 0.1 packages are built with Qt 6.12.0. On Linux
the build also needs the OpenGL, xkbcommon, fontconfig, FreeType and xcb development packages (on Ubuntu 24.04:
`libgl-dev libegl-dev libxkbcommon-dev libfontconfig1-dev libfreetype-dev libxcb1-dev`; the `qt-app` job of
`.github/workflows/ci.yml` lists what CI installs). Pass the Qt kit to the configure step:

```
cmake --preset linux-gcc-qt -DCMAKE_PREFIX_PATH=<path to the Qt kit>      # or mac-clang-qt
cmake --build --preset linux-gcc-qt
ctest --preset linux-gcc-qt
```

The programs are in `build/<preset>/bin`. The tests include a soak test of about five minutes (`-LE soak` leaves
it out), and on Linux they compare pixel images strictly against images made with Qt 6.12.0 on Ubuntu 24.04, so
with another Qt some image tests can fail. On Windows, `tools\dev\build-core.cmd` and `tools\dev\build-qt.cmd` set
up the Visual Studio environment and run all three steps (`ESSES_QT_DIR` selects the Qt kit).
[tools/dev/README.md](tools/dev/README.md) lists the presets and the main options, and
[08_REPO_AND_BUILD.md](docs/design/08_REPO_AND_BUILD.md) section 3 describes the build. The other libraries Esses
uses are vendored in `third_party/`, so the build downloads nothing.

## Documentation

- [User guide](docs/user/index.md): installing, opening files, choosing laps, graphs, maths, troubleshooting.
- [Changelog](CHANGELOG.md).
- Design: [docs/design/](docs/design/README.md) (start with `00_OVERVIEW.md`), the decision log
  [DECISIONS.md](docs/design/DECISIONS.md) and the milestone notes in [docs/milestones/](docs/milestones/).
- File formats as implemented: [docs/formats/](docs/formats/); maths compatibility:
  [docs/maths-compat.md](docs/maths-compat.md).

## Contributing and reporting problems

Problems and ideas go to the [issue tracker](https://github.com/mitchdetailed/jubilant-octo-meme/issues). Issues
are public, so keep private data out of them: [Reporting a problem](docs/user/troubleshooting.md#reporting-a-problem)
explains what to include and what to leave out. Report security problems privately (see [SECURITY.md](SECURITY.md)).

Esses is a clean-room project: where code and behaviour come from matters as much as whether they work. Read
[CONTRIBUTING.md](CONTRIBUTING.md) and [CLEANROOM.md](CLEANROOM.md) before your first change.
[GOVERNANCE.md](GOVERNANCE.md) explains how decisions are made.

## Licence

Esses is licensed under the [MIT licence](LICENSE). The application packages include Qt 6 (LGPL-3.0-only) and the
Qt Advanced Docking System (LGPL-2.1-or-later), both linked dynamically, so you can replace them with your own
builds: Qt with a build of the same version, the docking library with one of the same 5.x interface
([Replacing the Qt libraries](docs/user/relinking.md), [NOTICES.md](NOTICES.md)). NOTICES.md lists the third-party
components with their licences and the source offer for Qt and the docking library; each package's
`THIRD_PARTY_LICENSES.md` lists what that package contains.

"Esses" is the project's working name. Esses is an independent project: it is not affiliated with, endorsed by or
supported by MoTeC, and it contains none of MoTeC's software, help text or files. [TRADEMARK.md](TRADEMARK.md)
explains how the project uses names and marks.
