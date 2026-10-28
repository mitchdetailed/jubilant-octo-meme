# Notices

Esses is licensed under the MIT licence (`LICENSE`). This file lists the third-party components that are in
this repository or in the distributed programs, with their licences (08_REPO_AND_BUILD.md sections 7.4, 9
and 10). Licence texts: `LICENSES/` in the source tree (the Qt licences in `tools/packaging/licenses/`), the
`licenses/` folder beside this file in every package. Each package also carries `THIRD_PARTY_LICENSES.md`,
written for it from the files it contains (see "Generated section" below).

The packages (07 section 7.1, D-212): the Windows installer, the Windows portable zip, the Linux AppImage and
the macOS disk image hold `esses`, `esses-cli` and `esses-render` with Qt, the docking library and the shipped
data; the standalone command-line archives hold `esses-cli` and the data only (no Qt, no docking library).

## Qt (LGPL-3.0-only)

**This application uses Qt 6.12.0 under the GNU LGPL v3** (the Qt of the 0.1 release packages, D-126; a
package built with another Qt says so in its About dialog, in `esses --version` and in its
`THIRD_PARTY_LICENSES.md`). Qt is Copyright (C) The Qt Company Ltd. and other contributors.

- **What is used.** The Qt Core, Qt Gui and Qt Widgets libraries and the plugins the deployment tools add for
  them (platform integration, including the offscreen platform behind `--offscreen`, widget styles, image
  formats, icon engines; Qt Svg for the SVG image format and icons), as unmodified official binaries, linked
  dynamically (D-006). No GPL-only Qt module is used, and neither Qt Network nor Qt Pdf is shipped. The list of
  files is in each package's `THIRD_PARTY_LICENSES.md`.
- **Licence texts.** `licenses/LGPL-3.0-only.txt` and, because the LGPL v3 builds on it,
  `licenses/GPL-3.0-only.txt`, in every package that contains Qt (in the source:
  `tools/packaging/licenses/`).
- **Source.** The complete source of the Qt version shipped is its source archive,
  `https://download.qt.io/official_releases/qt/<major>.<minor>/<version>/single/qt-everywhere-src-<version>.tar.xz`;
  for 6.12.0: https://download.qt.io/official_releases/qt/6.12/6.12.0/single/qt-everywhere-src-6.12.0.tar.xz
  (when Qt moves a superseded version, the same path under https://download.qt.io/archive/qt/). The About dialog
  shows this URL for the Qt version it runs with. Each Esses release keeps a copy of that archive, with its
  SHA-256, as a release asset or on the project's storage, so this offer does not depend on Qt's servers; the
  offer is valid for at least three years after each release.
- **Replacing Qt.** You may use a modified or another build of the same Qt version: put your libraries in place
  of the shipped ones. Windows: the `Qt6*.dll` files and the plugin folders (`platforms\`, `styles\`, ...) beside
  `esses.exe`. Linux: extract the AppImage (`./esses-<version>-x86_64.AppImage --appimage-extract`), replace
  `squashfs-root/usr/lib/libQt6*.so.6` and `squashfs-root/usr/plugins/`, and start `squashfs-root/AppRun`.
  macOS: `Esses.app/Contents/Frameworks/Qt*.framework` and `Esses.app/Contents/PlugIns/`, then sign the bundle
  again for this Mac (`codesign --force --deep --sign - Esses.app`). Esses loads whatever compatible Qt it finds
  there. Or build Esses from its source against your Qt (`README.md`).
- **Qt's own third-party code.** Qt contains code of other projects under their own licences (for example
  PCRE2, HarfBuzz, FreeType, libpng, zlib); Qt lists them at https://doc.qt.io/qt-6/licenses-used-in-qt.html and
  in the `qt_attribution.json` files of the source archive. The Qt libraries for Linux also bring ICU
  (International Components for Unicode, Copyright Unicode, Inc. and others, under the Unicode License:
  https://github.com/unicode-org/icu/blob/main/LICENSE), which the AppImage contains.

### Qt Advanced Docking System (LGPL-2.1-or-later)

The graphical application `esses` uses the Qt Advanced Docking System (ADS) 5.1.1, Copyright (C) 2017-2026 Uwe
Kindler and the Qt Advanced Docking System contributors, for its dock windows (D-026, D-153). ADS is free
software under the GNU Lesser General Public License version 2.1 or (at your option) any later version; the
licence text is `third_party/qtads/LICENSE` and `LICENSES/LGPL-2.1-or-later.txt` (in the packages:
`licenses/LGPL-2.1-or-later.txt`). ADS comes with no warranty.

- **Source.** The complete source of the version used is in this repository under `third_party/qtads/`: the
  files of the upstream tag v5.1.1
  (https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/archive/refs/tags/v5.1.1.tar.gz, SHA-256
  `9ce947acbb1984946f9145c976e42e6984f99b98fb414dd7354a290af6d4ff04`; `third_party/README.md`) with the changes
  Esses made to them. `third_party/qtads/UPSTREAM.md` lists each change with its date, its reason and its
  upstream status, and each changed file says so where it was changed (LGPL-2.1 section 2). Upstream:
  https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System. Each Esses release keeps the same archive as a
  release asset (08 section 9 item 3), and the release's own source (its tag of this repository) holds the
  changed files; the About dialog links both.
- **Linking.** ADS is built from that source as a separate shared library and loaded by `esses` at run time:
  `qtadvanceddocking-qt6.dll` (Windows), `libqtadvanceddocking-qt6.so.5` (Linux),
  `libqtadvanceddocking-qt6.5.dylib` (macOS); Debug builds add a `d` (`qtadvanceddocking-qt6d.dll`). It is
  never linked statically; only `esses_widgets` uses its API (`libs/widgets/src/shell/ads_backend.cpp`; the About
  dialog reads its version header).
- **Replacing it.** You may use a modified or newer ADS. Either build ADS as a shared library from
  `third_party/qtads` or from upstream (its CMake option `BUILD_STATIC=OFF`, against the same Qt version and
  compiler, keeping the 5.x binary interface) and put your library in place of the one next to the `esses`
  executable (Windows: the DLL in the same folder; Linux: `usr/lib/` of the extracted AppImage; macOS:
  `Esses.app/Contents/Frameworks/`, then sign again as above; or a directory named in `LD_LIBRARY_PATH` /
  `DYLD_LIBRARY_PATH`); or change the files under `third_party/qtads` and rebuild Esses from source, which builds
  whatever is there (`cmake/qtads.cmake`). `ESSES_DOCK_BACKEND=qdockwidget` (environment) runs `esses` on its own
  `QDockWidget` dock backend instead.

## Microsoft Visual C++ Redistributable (Windows packages)

The Windows programs are built with Microsoft Visual C++ and need its runtime (`msvcp140.dll`,
`vcruntime140.dll`, `vcruntime140_1.dll` and their companions), Copyright (C) Microsoft Corporation, which
Microsoft lets developers redistribute under the "Distributable Code" terms of the Microsoft Software License
Terms for Visual Studio (https://visualstudio.microsoft.com/license-terms/; the files that may be redistributed:
https://learn.microsoft.com/visualstudio/releases/2022/redistribution). The installer carries Microsoft's
`vc_redist.x64.exe` and runs it when the installed runtime is older than the build's (08 section 7.1); a per-user
installation, the portable zip and the command-line zip carry the runtime's DLLs beside the programs (app-local
deployment). The runtime is not part of the source tree.

## Third-party code in this repository

Vendored under `third_party/`, unchanged except the two fixes to the Qt Advanced Docking System listed in
`third_party/qtads/UPSTREAM.md` (above); provenance (local copies, and the downloads the user approved on
2026-10-10 and, for ADS, on 2026-10-11) and file hashes are in `third_party/README.md`.

| Component | Version | Licence | Copyright | Used by | In distributed programs |
|---|---|---|---|---|---|
| xxHash | 0.8.3 | BSD-2-Clause | 2012-2023 Yann Collet | `esses_base` (`Hash128`, xxh3-128) | yes (compiled into every program) |
| pugixml | 1.13 | MIT | 2006-2024 Arseny Kapoulkine | `esses::pugixml`: `.ldx` parsing in `esses_formats` | yes (every program) |
| nlohmann/json | 3.11.3 | MIT | 2013-2022 Niels Lohmann | `esses::nlohmann_json`: `JsonDoc`, every JSON document and `--json` output | yes (every program) |
| json-schema-validator | 2.3.0 | MIT | 2016 Patrick Boettcher | `esses::json_schema_validator`: `esses-cli validate` (M0), `esses_project` (M1) | yes (every program) |
| CLI11 | 2.4.2 | BSD-3-Clause | 2017-2024 University of Cincinnati, developed by Henry Schreiner | `esses::cli11`: `esses-cli` | yes, in `esses-cli` |
| fast_float | 6.1.6 | MIT OR Apache-2.0 OR BSL-1.0 | 2021 The fast_float authors | `esses::fast_float`: number parsing in `esses_formats` | yes (every program; used under MIT) |
| Catch2 | 3.7.1 | BSL-1.0 | Catch2 Authors | `esses::catch2`: unit and property tests only | no |
| Qt Advanced Docking System | 5.1.1 | LGPL-2.1-or-later | 2017-2026 Uwe Kindler and the Qt Advanced Docking System contributors | `esses::qtads`: the default `DockHost` backend in `esses_widgets` (see above) | yes, as a separate shared library beside `esses` |

Catch2 2.13.10 (BSL-1.0, 2022 Two Blue Cubes Ltd) was used by the phase-A tests and removed from the
repository in phase B.

## Test-only assets

Used by the tests only and never part of a distributed program.

| Component | Version | Licence | Copyright | Used by | In distributed programs |
|---|---|---|---|---|---|
| Noto Sans (Regular, Bold) | 2.015 | OFL-1.1 | 2022 The Noto Project Authors | `tests/golden/fonts/`: the only fonts of the golden images and the `esses_qtrender` text tests (D-127) | no |

## Data derived from public standards

`libs/base/src/casefold.cpp` contains case-folding and compatibility mapping tables written from the Unicode
Character Database (character properties of the Unicode Standard); no UCD file is included.

## Linux packages: libraries of the build system

Besides Qt, linuxdeploy copies into the AppImage the few libraries of the build machine (Ubuntu 24.04) that a
target system may lack (for example parts of the X11 client libraries). Each one is listed in the AppImage's
`THIRD_PARTY_LICENSES.md` with the copyright file of the Ubuntu package it came from
(`usr/share/esses/licenses/system/`).

## Generated section

`tools/packaging/gen_third_party.py` writes each package's `THIRD_PARTY_LICENSES.md` (D-212) from
`tools/packaging/third_party.json`, the licence files of the source tree and the files actually in the package:
every component with version, licence, copyright and source, the Qt libraries and plugins deployed, the
libraries copied from the build system, and the licence text of each vendored library (with its copyright
lines). The same run writes the package's CycloneDX SBOM (`esses-<version>-<platform>.cdx.json`, published
beside the packages with their SHA-256 checksums). `gen_third_party.py --check` keeps that list, this file and
`third_party/` in step. No code ported from another project is in the tree (`CLEANROOM.md` lists no
`ported-from` row); when one is added, its upstream notice is listed here and in the generated file.
