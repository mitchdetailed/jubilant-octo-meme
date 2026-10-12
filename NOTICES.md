# Notices

Esses is licensed under the MIT licence (`LICENSE`). This file lists the third-party components that are in
this repository or in the distributed programs, with their licences (08_REPO_AND_BUILD.md sections 7.4, 9
and 10). Full licence texts are in `LICENSES/`.

## Qt and other LGPL components (from M1)

M0 contains no Qt code and no Qt binaries are distributed. When the graphical application ships, this section
will carry, per 08_REPO_AND_BUILD.md section 9: the statement "This application uses Qt <version> under the
GNU LGPL v3", the LGPLv3 and GPLv3 texts, the exact URL and sha256 of the matching Qt source archive (written
offer valid for at least three years after each release), the relinking instructions, and the LGPL-2.1 notice
and source tag of the Qt Advanced Docking System. Qt is linked dynamically and never modified.

## Third-party code in this repository

Vendored under `third_party/`, unchanged; provenance (local copies, and the downloads the user approved on
2026-10-10) and file hashes are in `third_party/README.md`.

| Component | Version | Licence | Copyright | Used by | In distributed programs |
|---|---|---|---|---|---|
| xxHash | 0.8.3 | BSD-2-Clause | 2012-2023 Yann Collet | `esses_base` (`Hash128`, xxh3-128) | yes (compiled into every program) |
| pugixml | 1.13 | MIT | 2006-2024 Arseny Kapoulkine | `esses::pugixml`: `.ldx` parsing in `esses_formats` | yes, with `esses_formats` |
| nlohmann/json | 3.11.3 | MIT | 2013-2022 Niels Lohmann | `esses::nlohmann_json`: `JsonDoc`, every JSON document and `--json` output | yes, with the first JSON code |
| json-schema-validator | 2.3.0 | MIT | 2016 Patrick Boettcher | `esses::json_schema_validator`: `esses-cli validate` (M0), `esses_project` (M1) | yes, with `esses-cli validate` |
| CLI11 | 2.4.2 | BSD-3-Clause | 2017-2024 University of Cincinnati, developed by Henry Schreiner | `esses::cli11`: `esses-cli` | yes, in `esses-cli` |
| fast_float | 6.1.6 | MIT OR Apache-2.0 OR BSL-1.0 | 2021 The fast_float authors | `esses::fast_float`: number parsing in `esses_formats` | yes, with its first user |
| Catch2 | 3.7.1 | BSL-1.0 | Catch2 Authors | `esses::catch2`: unit and property tests only | no |

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
