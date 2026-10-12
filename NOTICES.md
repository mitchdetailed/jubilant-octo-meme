# Notices

Esses is licensed under the MIT licence (`LICENSE`). This file lists the third-party components that are in
this repository or in the distributed programs, with their licences (08_REPO_AND_BUILD.md sections 7.4, 9
and 10). Full licence texts are in `LICENSES/`.

## Qt and other LGPL components (from M1)

No Qt binaries are distributed yet. When the graphical application ships, this section will also carry, per
08_REPO_AND_BUILD.md section 9: the statement "This application uses Qt <version> under the GNU LGPL v3", the
LGPLv3 and GPLv3 texts, the exact URL and sha256 of the matching Qt source archive (written offer valid for at
least three years after each release) and the relinking instructions for Qt. Qt is linked dynamically and never
modified.

### Qt Advanced Docking System (LGPL-2.1-or-later)

The graphical application `esses` uses the Qt Advanced Docking System (ADS) 5.1.1, Copyright (C) 2017-2026 Uwe
Kindler and the Qt Advanced Docking System contributors, for its dock windows (D-026, D-153). ADS is free
software under the GNU Lesser General Public License version 2.1 or (at your option) any later version; the
licence text is `third_party/qtads/LICENSE` and `LICENSES/LGPL-2.1-or-later.txt`. ADS comes with no warranty.

- **Source.** The complete source of the version used is in this repository under `third_party/qtads/`,
  unmodified, as downloaded from the upstream tag v5.1.1
  (https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/archive/refs/tags/v5.1.1.tar.gz, SHA-256
  `9ce947acbb1984946f9145c976e42e6984f99b98fb414dd7354a290af6d4ff04`; `third_party/README.md`). Upstream:
  https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System. A packaged release will carry the same
  archive as a release asset (08 section 9 item 3).
- **Linking.** ADS is built from that source as a separate shared library and loaded by `esses` at run time:
  `qtadvanceddocking-qt6.dll` (Windows), `libqtadvanceddocking-qt6.so.5` (Linux),
  `libqtadvanceddocking-qt6.5.dylib` (macOS); Debug builds add a `d` (`qtadvanceddocking-qt6d.dll`). It is
  never linked statically; only `esses_widgets` uses its API (`libs/widgets/src/shell/ads_backend.cpp`).
- **Replacing it.** You may use a modified or newer ADS. Either build ADS as a shared library from
  `third_party/qtads` or from upstream (its CMake option `BUILD_STATIC=OFF`, against the same Qt version and
  compiler, keeping the 5.x binary interface) and put your library in place of the one next to the `esses`
  executable (Windows: the DLL in the same folder; Linux and macOS: the file the executable's run-time search
  path points to, or a directory named in `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`); or change the files under
  `third_party/qtads` and rebuild Esses from source, which builds whatever is there (`cmake/qtads.cmake`).
  `ESSES_DOCK_BACKEND=qdockwidget` (environment) runs `esses` on its own `QDockWidget` dock backend instead.

## Third-party code in this repository

Vendored under `third_party/`, unchanged; provenance (local copies, and the downloads the user approved on
2026-10-10 and, for ADS, on 2026-10-11) and file hashes are in `third_party/README.md`.

| Component | Version | Licence | Copyright | Used by | In distributed programs |
|---|---|---|---|---|---|
| xxHash | 0.8.3 | BSD-2-Clause | 2012-2023 Yann Collet | `esses_base` (`Hash128`, xxh3-128) | yes (compiled into every program) |
| pugixml | 1.13 | MIT | 2006-2024 Arseny Kapoulkine | `esses::pugixml`: `.ldx` parsing in `esses_formats` | yes, with `esses_formats` |
| nlohmann/json | 3.11.3 | MIT | 2013-2022 Niels Lohmann | `esses::nlohmann_json`: `JsonDoc`, every JSON document and `--json` output | yes, with the first JSON code |
| json-schema-validator | 2.3.0 | MIT | 2016 Patrick Boettcher | `esses::json_schema_validator`: `esses-cli validate` (M0), `esses_project` (M1) | yes, with `esses-cli validate` |
| CLI11 | 2.4.2 | BSD-3-Clause | 2017-2024 University of Cincinnati, developed by Henry Schreiner | `esses::cli11`: `esses-cli` | yes, in `esses-cli` |
| fast_float | 6.1.6 | MIT OR Apache-2.0 OR BSL-1.0 | 2021 The fast_float authors | `esses::fast_float`: number parsing in `esses_formats` | yes, with its first user |
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

## Generated section

From phase B, `tools/gen_notices.py` generates `THIRD_PARTY_LICENSES.md` for the packages from this file, the
`ported-from` rows of `CLEANROOM.md` and the licence texts in `LICENSES/`.
