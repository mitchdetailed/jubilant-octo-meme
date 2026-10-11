# 08 — Repository layout, build system, CI, testing and packaging

**Scope.** This document specifies the repository tree, the CMake design (presets, options, the `ESSES_BUILD_QT` switch, target↔module mapping, dependency acquisition, warnings, clang-tidy/clang-format, the configure-time Qt-free check), the GitHub Actions matrix, the per-layer testing strategy, packaging for Windows/macOS/Linux, versioning and release, LGPL compliance, REUSE/SPDX, the governance file set, and a concrete first-week bootstrap sequence for the developer's machine (Qt 6.7.2 `msvc2019_64`, Visual Studio 2022 with its bundled Ninja, CMake 4.4; library dependencies vendored under `third_party/`, no vcpkg, D-147). It implements D-034 and D-033 using the target names of `ARCHITECTURE_OUTLINE.md` §1. Milestone placement of each item is in 07_ROADMAP.md.

**Related decisions.** D-002, D-003, D-004, D-005, D-006, D-007, D-011, D-019, D-025, D-026, D-028, D-029, D-030, D-031, D-033, D-034, D-035; as built in M0: D-147 to D-151.

---

## 1. Repository layout

```
esses/
├── CMakeLists.txt                 # project(), options, add_subdirectory per layer, qt-free check hook
├── CMakePresets.json              # shared presets (§3.6); CMakeUserPresets.json is git-ignored
├── LICENSE                        # MIT (D-002, confirmed 2026-10-10)
├── NOTICES.md                     # Qt LGPL notice, source offer, third-party list (generated section)
├── README.md  CHANGELOG.md  CONTRIBUTING.md  DCO  GOVERNANCE.md  CLEANROOM.md  TRADEMARK.md  SECURITY.md
├── REUSE.toml  .clang-format  .clang-tidy  .editorconfig  .gitattributes  .gitignore  .gitmodules
├── CLAUDE.md  AGENTS.md           # architecture summary for AI-assisted contributors (D-004); written in M1 (D-151)
├── .github/
│   ├── workflows/{ci.yml, bench.yml, fuzz.yml, release.yml, abi-compat.yml(v1.0)}
│   ├── PULL_REQUEST_TEMPLATE.md   # provenance checkbox, AC id, DCO reminder
│   └── CODEOWNERS                 # generated from OWNERS files (M6)
├── cmake/
│   ├── naming.cmake               # the only place the codename appears as a prefix (D-003)
│   ├── esses_targets.cmake        # esses_add_library(), esses_add_executable(), esses_add_test()
│   ├── qt-free-check.cmake        # §3.9
│   ├── warnings.cmake  sanitizers.cmake  version.cmake  notices.cmake  deploy.cmake
│   ├── third_party.cmake          # builds the vendored libraries (§3.7.2)
│   ├── catch_discover.cmake  catch_discover_tests.cmake   # one ctest entry per TEST_CASE (§6.2, D-148)
│   ├── FindOrFetchADS.cmake       # ADS via FetchContent as a shared lib (§3.7.3)
│   └── EssesSDKConfig.cmake.in    # v1.0 (D-028)
├── libs/                          # one directory per esses_<lib> target (§2)
│   ├── base/  units/  store/  model/  formats/  analysis/  maths/  project/  plotcore/
│   ├── qtmodel/  qtrender/  widgets/              # Qt layers 4–5
│   ├── capi/ (v1.1)  script/ (v1.1)  video/ (later)
│   └── CMakeLists.txt             # adds layers 0–3 always, 4–5 only if ESSES_BUILD_QT
├── apps/
│   ├── cli/                       # esses-cli  (Qt-free)
│   ├── render/                    # esses-render (Qt Gui, offscreen)
│   └── app/                       # esses (Qt Widgets): MainWindow, resources, icons (our own)
├── plugins/                       # v1.0+: C-ABI plugins; private non-Qt deps, Qt only per the 05 §10.3 qt modes (D-028, D-131)
│   ├── formats/{csv/ (moved out of tree at v1.0), <vendor>/ (harvested)}
│   ├── mathfn/
│   └── examples/{c/, mingw/, rust/}
├── sdk/                           # esses_abi.h (C11), CHANGELOG.md, cmake config, examples (v1.0)
├── schemas/                       # *.schema.json: sidecar-1, probe-1, markers-1, fixture-1, project-1,
│                                  #   workbook-1, overrides-1, aliases-1, mappings-1, unitmap-1, maths-1,
│                                  #   scheme-1, keymap-1, profile-1, track-1, layout-1, journal-1, settings-1,
│                                  #   importer-profile-1, dictionary-1, units-1, rule-1, maths-case-1, plugin-1,
│                                  #   common-1, mathfn-manifest-1, laps-1, builtin-<id>-<version>,
│                                  #   components/<type>-<version>
├── data/                          # shipped data, all our own (D-004)
│   ├── units.json
│   ├── profiles/circuit.profile.json  (drag/rally at v1.0)
│   ├── rules/{beacon-pair,whole-outing,track-sections,detail-groups}.rule.json
│   ├── dictionaries/{motec.ld.json, motec.csv.json, generic.csv.json}
│   ├── profiles-text/{motec.csv.json, generic.csv.json}
│   ├── keymaps/default.json
│   ├── schemes/{light,dark}.scheme.json
│   └── workbooks/circuit-default.workbook.json
├── fixtures/                      # D-033 corpus (§6.7; layout and fixture-1 defined in 05 §9.1)
│   ├── <formatId>/<name>/{input.<ext> | fixture.json remote, fixture.json, expected.*.json, expected.channels/, README.md}
│   ├── manifest.json              # index of all fixtures incl. remote hashes
│   └── private/                   # git submodule → private repository (user's own files, never redistributed)
├── tests/
│   ├── unit/<lib>/                # Catch2, mirrors libs/ (also referenced from libs/<lib>/tests/)
│   ├── golden/{json/, drawlist/, images/, fonts/}
│   ├── maths/*.case.json          # schema maths-case-1, owned by 03 §14.1 (D-033 path)
│   ├── ui/{keymap/, walkthrough/, ...}  # Qt Test, offscreen
│   ├── fuzz/                      # libFuzzer targets + seed corpora + dictionaries
│   └── soak/                      # M4: saturday session (30 min nightly, 2 h before tags), lsan.supp
├── bench/                         # Google Benchmark sources, workloads.md (RW-1..RW-5), baselines/<runner-class>.json
├── third_party/                   # every library dependency, vendored with its licence file (§3.7.2, D-147)
│   ├── README.md                  # provenance: version, route, source, SHA-256 of every vendored file
│   ├── catch2v3/  cli11/  fast_float/  json-schema-validator/  nlohmann_json/  pugixml/  xxhash/   # M0
│   ├── pocketfft/  iir1/  …       # vendored when their milestone first needs them
├── docs/
│   ├── adr/ (DECISIONS.md split per ADR)  architecture.md  maths-compat.md  formats/ld.md
│   ├── milestones/Mn.md  spikes/  user/  sdk/ (v1.0)
├── tools/
│   ├── check_vendor_marks.py  vendor_marks_allowlist.txt  check_keymap_coverage.py  check_layering.py
│   ├── check_text_overlap.py      # local/private only: n-gram overlap with the help corpus and GPL sources (§5, §11)
│   ├── ld_ref_decode.py           # independent .ld reference decoder: synthetic-fixture oracle (D-041)
│   ├── check_importlog_codes.py   # every Import Log code emitted by fixtures is documented in docs/formats/ (AC-M0-12)
│   ├── bench_compare.py  golden_update.py  gen_notices.py  rename.py  release.py
│   ├── gen_ld_fixtures.cpp (built as esses-gen-fixtures)
│   └── packaging/{windows/esses.iss, macos/, linux/esses.desktop, linux/AppRun}
└── build/                         # git-ignored; binaryDir = build/<presetName>
```

Additions beyond the directory list in the task statement: `schemas/` (named by D-019 and outline §1.2), `bench/` (outline §8 names `bench/workloads.md`) and `third_party/` (D-034's vendored libraries need a home with per-library licence files; since D-147 it holds every library dependency). See §13.

**Never in the repository.** The clean-room working material stays outside the tree: `research/`, `help_text/`, `conv_samples/`, extracted or displayed i2 help pages, i2's shipped XML and sample files, i2 exports of the user's logs (they go only to `fixtures/private`), and GPL reference sources (gotzl/ldparser and the other read-for-understanding projects). `.gitignore` lists these directory names, and the `lint` job fails when a tracked path matches them or when a tracked file has the extension `.chm`, `.i2wkb`, `.i2wsp`, `.mcs`, `.mfs` or `.mt2` outside `fixtures/private` (§11).

## 2. Per-library directory convention

```
libs/<name>/
├── CMakeLists.txt            # esses_add_library(<name> LAYER n [QT_FREE] DEPS ... )
├── README.md                 # one paragraph: responsibility, what it must never depend on
├── include/esses/<name>/     # public headers; namespace esses::<name>
├── src/                      # private sources and private headers
└── tests/                    # Catch2 (or Qt Test for layers 4–5); esses_add_test(<name>_tests ...)
```

Rules enforced by the layer check in `esses_add_library()` (configure, §3.4), `tools/check_layering.py` (lint job) and `cmake/qt-free-check.cmake` (configure):

1. `#include <esses/<other>/…>` is allowed only for `<other>` in the target's declared `DEPS`; the script parses `compile_commands.json` and include lines.
2. Layers 0–3 (`base, units, store, model, formats, analysis, maths, project, plotcore, cli, capi, script`) carry the `ESSES_QT_FREE` target property; `qtmodel` may use `Qt6::Core` only; `qtrender` `Qt6::Gui`; `widgets`/`app` `Qt6::Widgets` + ADS (D-007).
3. `formats` never depends on `maths`, `analysis` or `project`; `analysis` never depends on `maths`; `maths` never depends on `project` or `formats`; `qtrender` never depends on `qtmodel` (outline §1.2). Same-layer edges allowed: `maths → analysis`, `project → analysis`, `project → maths` (01 §2.2).
4. Strings below layer 4 are UTF-8 `std::string`; `QString` only in `qtmodel` and above (checked by the same grep as the Qt-free check).

## 3. CMake design

### 3.1 Requirements and global settings

```cmake
cmake_minimum_required(VERSION 3.28...4.4)   # presets v8 need 3.28; the developer has 4.4
project(esses VERSION 0.1.0 LANGUAGES C CXX) # version overridden by cmake/version.cmake from git describe
include(cmake/naming.cmake)                  # ESSES_PREFIX "esses", ESSES_NAMESPACE "esses", ESSES_APP_NAME "Esses"
set(CMAKE_CXX_STANDARD 20)  set(CMAKE_CXX_STANDARD_REQUIRED ON)  set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")   # /MD, matches Qt kits
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)  # every exe, test exe and DLL (incl. ADS) in one directory,
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)  #   set before FetchContent_MakeAvailable(ads) (§3.7.5)
if(MSVC) add_compile_options(/utf-8 /Zc:__cplusplus /Zc:preprocessor /permissive- /EHsc /bigobj) endif()
```

The 3.28 minimum makes policy CMP0141 `NEW`, so `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` works; the `ci` presets set it to `Embedded` (`/Z7`) because sccache cannot cache objects compiled with `/Zi` (§4.3). All executables land in `build/<preset>/bin/`.

### 3.2 Options

| Option | Default | Meaning |
|---|---|---|
| `ESSES_BUILD_QT` | ON (OFF in M0, where no Qt code exists, D-150) | Build layers 4–5, `esses`, `esses-render`, UI tests. OFF = no `find_package(Qt6)` at all (D-007) |
| `ESSES_BUILD_TESTS` | ON | Catch2 / Qt Test targets, `enable_testing()` |
| `ESSES_BUILD_BENCH` | OFF (ON in `bench` preset) | Google Benchmark targets (Google Benchmark is vendored when `bench/` first needs it, D-147) |
| `ESSES_BUILD_FUZZ` | OFF | libFuzzer targets (Clang only) |
| `ESSES_BUILD_PLUGINS` | OFF (v1.0) | `plugins/` and `sdk/` |
| `ESSES_WARNINGS_AS_ERRORS` | OFF (ON in `ci` presets) | `/WX` or `-Werror` |
| `ESSES_SANITIZE` | "" | `address,undefined` or `thread` (Clang/GCC) |
| `ESSES_ENABLE_CLANG_TIDY` | OFF (ON in `lint`) | `CMAKE_CXX_CLANG_TIDY` on Qt-free targets only |
| `ESSES_WITH_CATALOGUE` | OFF (v1.0) | sqlite3 amalgamation catalogue (D-019) |
| `ESSES_WITH_MAT` | OFF (v1.0) | matio export |
| `ESSES_WITH_SCRIPT` | OFF (v1.1) | Lua + sol2 (`esses_script`) |
| `ESSES_WITH_UNITS`, `ESSES_WITH_MODEL`, `ESSES_WITH_FORMATS`, `ESSES_WITH_CLI` | ON | Build `libs/units`, `libs/model`, `libs/formats`, `apps/cli` (the developer tools and the fixture corpus tests follow the CLI). OFF leaves an unfinished component out; a component that DEPS on it fails the layer check, which names the switch (D-149) |
| `ESSES_LTO` | OFF (ON in release presets) | `INTERPROCEDURAL_OPTIMIZATION` |
| `ESSES_FORCE_ASSERTS` | OFF (ON in `ci` presets) | Keep `ESSES_ASSERT` active in optimised builds |
| `ESSES_TEST_FONT_DIR` | `${CMAKE_SOURCE_DIR}/tests/golden/fonts` | Pinned fonts for goldens |

### 3.3 `cmake/naming.cmake`

The codename appears as an identifier only here and in documentation (D-003), so `tools/rename.py` can swap it:

```cmake
set(ESSES_PREFIX      "esses")      # CMake target prefix: ${ESSES_PREFIX}_base
set(ESSES_NAMESPACE   "esses")      # C++ namespace and alias prefix esses::base
set(ESSES_CAPI_PREFIX "esses_")     # C ABI symbol prefix
set(ESSES_APP_NAME    "Esses")      # window title, installer, bundle name
set(ESSES_EXE_NAME    "esses")      # esses, esses-cli, esses-render
set(ESSES_ORG_DOMAIN  "esses.invalid")  # replaced when the domain exists (schema ids, QSettings)
```

### 3.4 Target helper

```cmake
# cmake/esses_targets.cmake
function(esses_add_library name)
  cmake_parse_arguments(A "QT_FREE;HEADER_ONLY" "LAYER" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
  set(t ${ESSES_PREFIX}_${name})
  if(A_HEADER_ONLY) add_library(${t} INTERFACE) else() add_library(${t} STATIC ${A_SOURCES}) endif()
  add_library(${ESSES_NAMESPACE}::${name} ALIAS ${t})
  target_include_directories(${t} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                         $<INSTALL_INTERFACE:include>)
  target_link_libraries(${t} PUBLIC ${A_DEPS} PRIVATE ${A_PRIVATE_DEPS})
  target_compile_features(${t} PUBLIC cxx_std_20)
  esses_apply_warnings(${t})
  set_target_properties(${t} PROPERTIES ESSES_LAYER ${A_LAYER} FOLDER "libs/${name}")
  # Layer check (01 §2.3): no esses:: dependency may sit in a higher layer. libs/CMakeLists.txt adds the libraries
  # in layer order, so every esses:: dependency already exists; same-layer edges (maths → analysis, project → maths) pass.
  foreach(d IN LISTS A_DEPS A_PRIVATE_DEPS)
    if(NOT d MATCHES "^${ESSES_NAMESPACE}::") continue() endif()
    if(NOT TARGET ${d})
      message(FATAL_ERROR "layer check: ${t} depends on ${d}, which is not defined yet (add libraries in layer order)")
    endif()
    get_target_property(real ${d} ALIASED_TARGET)
    if(NOT real) set(real ${d}) endif()
    get_target_property(dl ${real} ESSES_LAYER)          # NOTFOUND for esses::ads and other non-esses targets
    if(dl AND dl GREATER A_LAYER)
      message(FATAL_ERROR "layer check: ${t} (layer ${A_LAYER}) depends on ${d} (layer ${dl})")
    endif()
  endforeach()
  if(A_QT_FREE) esses_mark_qt_free(${t}) endif()
  if(ESSES_ENABLE_CLANG_TIDY AND A_QT_FREE)
    set_target_properties(${t} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--warnings-as-errors=*")
  endif()
endfunction()
```

Every `libs/<name>/CMakeLists.txt` is then one call, e.g. `esses_add_library(store LAYER 1 QT_FREE SOURCES ... DEPS esses::base)`. Executables use `esses_add_executable(... LAYER n ...)` with the same check. The `lint` job's negative test configures a scratch library `esses_add_library(probe LAYER 1 DEPS esses::project)` and asserts that configure fails (01 §11 item 3).

### 3.5 Targets ↔ modules

| Dir | Target | Alias | Kind | Layer | Qt | Public deps | Private deps | Tests target | Built when |
|---|---|---|---|---|---|---|---|---|---|
| `libs/base` | `esses_base` | `esses::base` | static | 0 | free | — | nlohmann_json, xxHash::xxhash | `esses_base_tests` | always |
| `libs/units` | `esses_units` | `esses::units` | static | 0 | free | base | — | `esses_units_tests` | always |
| `libs/store` | `esses_store` | `esses::store` | static | 1 | free | base | — | `esses_store_tests` | always |
| `libs/model` | `esses_model` | `esses::model` | static | 1 | free | base, units, store | — | `esses_model_tests` | always |
| `libs/formats` | `esses_formats` | `esses::formats` | static | 2 | free | base, units, store, model | pugixml, ZLIB, FastFloat | `esses_formats_tests` | always |
| `libs/analysis` | `esses_analysis` | `esses::analysis` | static | 2 | free | base, units, store, model | Eigen3, pocketfft | `esses_analysis_tests` | always |
| `libs/maths` | `esses_maths` | `esses::maths` | static | 2 | free | base, units, store, model, analysis | `third_party::iir1` | `esses_maths_tests` | always |
| `libs/project` | `esses_project` | `esses::project` | static | 2 | free | base, units, model, analysis, maths (same-layer edge, 01 §2.2) | `third_party::json_schema_validator`; pugixml (v1.0 `src/cfgimport/` importers) | `esses_project_tests` | always |
| `libs/plotcore` | `esses_plotcore` | `esses::plotcore` | static | 3 | free | base, store, model, analysis, project | — | `esses_plotcore_tests` | always |
| `apps/cli` | `esses_cli` → `esses-cli` | — | exe | 3 | free | all Qt-free libs | CLI11 | golden JSON via ctest | always |
| `libs/capi` | `esses_capi` | `esses::capi` | static+shared | 3 | free | all Qt-free libs | — | `esses_capi_tests` | v1.1 |
| `libs/script` | `esses_script` | `esses::script` | static | 3 | free | all Qt-free libs | lua, sol2 | `esses_script_tests` | `ESSES_WITH_SCRIPT` |
| `libs/qtmodel` | `esses_qtmodel` | `esses::qtmodel` | static | 4 | Core | all Qt-free libs, Qt6::Core | — | `esses_qtmodel_tests` (Qt Test) | `ESSES_BUILD_QT` |
| `libs/qtrender` | `esses_qtrender` | `esses::qtrender` | static | 4 | Gui | plotcore, Qt6::Gui | — | `esses_qtrender_tests` (goldens) | `ESSES_BUILD_QT` |
| `apps/render` | `esses_render` → `esses-render` | — | exe | 4 | Gui | qtmodel, qtrender | — | render parity test | `ESSES_BUILD_QT` |
| `libs/widgets` | `esses_widgets` | `esses::widgets` | static | 5 | Widgets | qtmodel, qtrender, Qt6::Widgets, `esses::ads` (alias of ADS's `qt6advanceddocking`, §3.7.3) | — | `esses_widgets_tests` (Qt Test) | `ESSES_BUILD_QT` |
| `apps/app` | `esses_app` → `esses` | — | exe (WIN32/MACOSX_BUNDLE) | 5 | Widgets | widgets | Qt6::Svg, Qt6::PrintSupport | walkthroughs | `ESSES_BUILD_QT` |
| `libs/video` | `esses_video` | `esses::video` | static | 5 | Multimedia | qtmodel, Qt6::Multimedia | — | — | later |
| `plugins/*` | `esses_plugin_<id>` | — | shared | — | any (private) | `sdk/esses_abi.h` only | anything | `abi-compat` | `ESSES_BUILD_PLUGINS` |

Qt modules ever linked: `Core, Gui, Widgets, OpenGL, OpenGLWidgets (GlBackend only), Svg, PrintSupport, Test, Concurrent (tests only), Multimedia (later)`; never `Sql`, never a GPL-only module (D-006).

### 3.6 Presets

`CMakePresets.json` (version 8). Hidden bases compose into named presets; every named preset has a build, test and (for CI) workflow preset of the same name.

```json
{
  "version": 8,
  "cmakeMinimumRequired": { "major": 3, "minor": 28, "patch": 0 },
  "configurePresets": [
    { "name": "base", "hidden": true, "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": { "CMAKE_EXPORT_COMPILE_COMMANDS": "ON", "ESSES_BUILD_TESTS": "ON" } },
    { "name": "qt",   "hidden": true, "cacheVariables": { "ESSES_BUILD_QT": "ON" } },
    { "name": "core", "hidden": true, "cacheVariables": { "ESSES_BUILD_QT": "OFF" } },
    { "name": "ci",   "hidden": true, "cacheVariables": { "ESSES_WARNINGS_AS_ERRORS": "ON",
                          "CMAKE_MSVC_DEBUG_INFORMATION_FORMAT": "Embedded",
                          "CMAKE_C_COMPILER_LAUNCHER": "sccache", "CMAKE_CXX_COMPILER_LAUNCHER": "sccache" } },
    { "name": "win-msvc", "hidden": true, "inherits": "base",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "architecture": { "value": "x64", "strategy": "external" },
      "cacheVariables": { "CMAKE_C_COMPILER": "cl", "CMAKE_CXX_COMPILER": "cl" } },
    { "name": "win-msvc-debug",   "inherits": ["win-msvc", "qt"],  "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "win-msvc-release", "inherits": ["win-msvc", "qt"],  "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo", "ESSES_LTO": "ON" } },
    { "name": "win-msvc-core",    "inherits": ["win-msvc", "core"], "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "linux-gcc",        "inherits": ["base", "qt"],  "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo", "CMAKE_C_COMPILER": "gcc", "CMAKE_CXX_COMPILER": "g++" } },
    { "name": "linux-gcc-core",   "inherits": ["base", "core"], "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "linux-clang-asan", "inherits": ["base", "core"], "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++", "ESSES_SANITIZE": "address,undefined" } },
    { "name": "linux-clang-tsan", "inherits": ["base", "core"], "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++", "ESSES_SANITIZE": "thread" } },
    { "name": "linux-clang-fuzz", "inherits": ["base", "core"], "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo", "CMAKE_C_COMPILER": "clang", "CMAKE_CXX_COMPILER": "clang++", "ESSES_BUILD_FUZZ": "ON", "ESSES_SANITIZE": "address,undefined" } },
    { "name": "mac-clang",        "inherits": ["base", "qt"],  "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" } },
    { "name": "mac-clang-core",   "inherits": ["base", "core"], "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "bench",            "inherits": ["base", "core"], "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "ESSES_BUILD_BENCH": "ON", "ESSES_BUILD_TESTS": "OFF" } },
    { "name": "ci-linux-gcc-core",   "inherits": ["linux-gcc-core", "ci"] },
    { "name": "ci-win-msvc-core",    "inherits": ["win-msvc-core", "ci"] },
    { "name": "ci-mac-clang-core",   "inherits": ["mac-clang-core", "ci"] },
    { "name": "ci-linux-gcc",        "inherits": ["linux-gcc", "ci"] },
    { "name": "ci-win-msvc-release", "inherits": ["win-msvc-release", "ci"] },
    { "name": "ci-mac-clang",        "inherits": ["mac-clang", "ci"] }
  ],
  "buildPresets": [ { "name": "win-msvc-debug", "configurePreset": "win-msvc-debug" }, "…one per configure preset, ci-* included…" ],
  "testPresets":  [ { "name": "win-msvc-debug", "configurePreset": "win-msvc-debug",
                      "output": { "outputOnFailure": true }, "environment": { "QT_QPA_PLATFORM": "offscreen" } }, "…one per configure preset, ci-* included…" ],
  "workflowPresets": [ { "name": "ci-core-linux", "steps": [ { "type": "configure", "name": "ci-linux-gcc-core" }, { "type": "build", "name": "ci-linux-gcc-core" }, { "type": "test", "name": "ci-linux-gcc-core" } ] }, "…" ]
}
```

`core` presets set no `CMAKE_PREFIX_PATH` for Qt, so no Qt is found and none is needed. `qt` presets additionally expect `CMAKE_PREFIX_PATH` (from `CMakeUserPresets.json` locally, from `install-qt-action` in CI). The hidden `ci` base is inherited only by the `ci-*` configure presets, which `ci.yml` and the workflow presets use (workflow presets cannot carry cache variables themselves); the developer's local presets do not inherit it, so a local build warns without failing (D-034). There is no vcpkg toolchain or triplet (D-147): the vendored libraries are built in the tree as static or header-only targets with the project's dynamic CRT (`/MD`, matching Qt), so no third-party DLL has to be found at run time (§3.7.5). The repository's `CMakePresets.json` is the as-built source of truth; in M0 it also has `win-msvc-core-rel`, sets `ESSES_FORCE_ASSERTS` in the `ci` base and has no sccache launcher yet.

Developer's `CMakeUserPresets.json` (git-ignored). Build and test presets must be declared here too, or `cmake --build --preset dev` fails with "No such build preset"; the configure preset's `environment` is inherited by its build and test presets (`inheritConfigureEnvironment` defaults to true), which puts the Qt kit's `bin` on `PATH` for tests and for running `esses.exe` from the build tree:

```json
{ "version": 8,
  "configurePresets": [
    { "name": "dev", "inherits": "win-msvc-debug",
      "cacheVariables": { "CMAKE_PREFIX_PATH": "C:/Qt/6.7.2/msvc2019_64" },
      "environment": { "PATH": "C:/Qt/6.7.2/msvc2019_64/bin;$penv{PATH}" } },
    { "name": "dev-core", "inherits": "win-msvc-core" } ],
  "buildPresets": [ { "name": "dev", "configurePreset": "dev" }, { "name": "dev-core", "configurePreset": "dev-core" } ],
  "testPresets": [
    { "name": "dev", "configurePreset": "dev", "output": { "outputOnFailure": true },
      "environment": { "QT_QPA_PLATFORM": "offscreen" } },
    { "name": "dev-core", "configurePreset": "dev-core", "output": { "outputOnFailure": true } } ] }
```

### 3.7 Dependencies

#### 3.7.1 vcpkg manifest

**Superseded by D-147 (M0 as built): there is no vcpkg manifest.** Qt comes from the official installer or aqtinstall (D-006) and ADS through FetchContent (§3.7.3, D-119). Every other library dependency is vendored under `third_party/` (§3.7.2): the M0 libraries were copied from local sources or downloaded with the user's approval on 2026-10-10, and each later one is vendored the same way, with the user's approval for the download, when its milestone first needs it. The manifest this section first specified (`nlohmann-json`, `pugixml`, `zlib`, `fast-float`, `xxhash`, `cli11` and `eigen3`, the features `tests` (catch2, benchmark), `catalogue` (sqlite3), `mat` (matio) and `script` (lua, sol2), a pinned `builtin-baseline`, the `x64-windows-static-md` triplet and a file-based binary cache in CI) is not created. The table gives the as-built source and version of each dependency.

| Dependency | Source | Version policy | Licence (SPDX) | Used by | Notes |
|---|---|---|---|---|---|
| nlohmann-json | `third_party/nlohmann_json` | 3.11.3 | MIT | base (`JsonDoc`), formats (`probe_json`), cli, project | header-only, `esses::nlohmann_json` |
| pugixml | `third_party/pugixml` | 1.13 | MIT | formats (`.ldx`, i2 XML at v1.0) | static library `esses::pugixml` |
| zlib | `third_party/` when first needed | pinned at vendoring | Zlib | formats (fixtures `.csv.gz`, XRZ/MDF later) | |
| fast-float | `third_party/fast_float` | 6.1.6 | Apache-2.0 OR MIT OR BSL-1.0 | formats (every floating-point parse from M0; `TextEngine`) | header-only, `esses::fast_float`; D-029, D-147 |
| xxhash | `third_party/xxhash` | 0.8.3 | BSD-2-Clause | base (`Hash128`) | xxh3-128; header-only, `esses::xxhash` |
| cli11 | `third_party/cli11` | 2.4.2 | BSD-3-Clause | cli | header-only, `esses::cli11` |
| eigen3 | `third_party/` when first needed | pinned at vendoring | MPL-2.0 | analysis (`TrendFit`, track fitting) | `EIGEN_MPL2_ONLY` defined globally |
| catch2 | `third_party/catch2v3` | 3.7.1 (amalgamated) | BSL-1.0 | all tests | `esses::catch2`, `esses::catch2_nomain`; own test discovery (§6.2, D-148) |
| benchmark | `third_party/` when `bench/` first needs it | pinned at vendoring | Apache-2.0 | bench | |
| sqlite3 | `third_party/` (v1.0) | pinned at vendoring | Public Domain (blessing) | project (v1.0) | amalgamation, never QtSql (D-019) |
| matio | `third_party/` (v1.0) | pinned at vendoring | BSD-2-Clause | formats (v1.0) | v5 without HDF5 |
| lua 5.4 + sol2 | `third_party/` (v1.1) | 5.4.x | MIT / MIT | script (v1.1) | sandbox per D-030 |
| pocketfft | `third_party/` when first needed | pinned commit | BSD-3-Clause | analysis (`Spectrum`, v1.0) | header-only (`pocketfft_hdronly.hpp`), `INTERFACE` target `esses::pocketfft` |
| Iir1 | `third_party/` | pinned tag | MIT | maths (IIR kernels) | **compiled** (`Biquad.cpp`, `Butterworth.cpp`, `Cascade.cpp`, `ChebyshevI.cpp`, `ChebyshevII.cpp`, `PoleFilter.cpp`, `RBJ.cpp`, `State.cpp`): static library `esses::iir1` |
| json-schema-validator | `third_party/json-schema-validator` | 2.3.0 | MIT | project (`SchemaRegistry`), cli | **compiled** (the six sources of `json-schema-validator/src`): static library `esses::json_schema_validator`, links `esses::nlohmann_json` |
| Qt 6 (Core, Gui, Widgets, Svg, PrintSupport, Test; OpenGL/OpenGLWidgets only with `GlBackend`; Multimedia later) | official installer / aqtinstall | min 6.7, CI 6.12 | LGPL-3.0-only (with GPL-2.0/3.0 alternatives) | layers 4–5 | dynamic only (D-006) |
| Qt Advanced Docking System | FetchContent | pinned release tag ≥ 4.3, verified against Qt 6.7.2 and 6.12 | LGPL-2.1-or-later | widgets | shared library `qt6advanceddocking` (`qt6advanceddocking.dll`), aliased `esses::ads` (D-026) |
| FFmpeg (inside Qt Multimedia) | Qt installer | as shipped | LGPL-2.1-or-later | video (later) | notice in `NOTICES.md` when added |

Host tools (not shipped): CMake (BSD-3-Clause), Ninja (Apache-2.0), LLVM clang-format/clang-tidy (Apache-2.0 WITH LLVM-exception), reuse-tool (GPL-3.0-or-later, dev-time only), Inno Setup (Inno Setup licence), linuxdeploy (MIT), create-dmg (MIT), aqtinstall (MIT).

#### 3.7.2 Vendored libraries

`third_party/<lib>/` contains the upstream files unchanged and the upstream licence file. One file, `third_party/README.md`, records for every library its version, licence file, route (local copy, or download approved by the user), the source of the copy, the upstream URL and the SHA-256 of every vendored file (D-147); it replaces a per-library `UPSTREAM.md`. `REUSE.toml` annotates each directory with its SPDX id, and `NOTICES.md` carries the copyright lines. M0 vendors pugixml, xxHash, nlohmann/json, json-schema-validator, CLI11, fast_float and Catch2 v3; pocketfft and Iir1 follow when their milestones need them. Of the three D-034 named, only pocketfft is header-only; Iir1 and json-schema-validator are compiled libraries (D-034's "header-only" is corrected here, §13). `cmake/third_party.cmake` never calls an upstream `CMakeLists.txt` (Iir1's `cmake_minimum_required(VERSION 3.1)` is a hard error under CMake 4.x), builds every target itself with SYSTEM include directories and without the project's warning flags, and names it `esses::<lib>`:

```cmake
# cmake/third_party.cmake (excerpt of the M0 pattern; pocketfft and Iir1 follow it when they are vendored)
set(ESSES_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH "Vendored third-party sources")

add_library(${ESSES_PREFIX}_tp_xxhash INTERFACE)                                   # header-only
target_include_directories(${ESSES_PREFIX}_tp_xxhash SYSTEM INTERFACE "${ESSES_THIRD_PARTY_DIR}/xxhash")
add_library(${ESSES_NAMESPACE}::xxhash ALIAS ${ESSES_PREFIX}_tp_xxhash)

set(_jsv "${ESSES_THIRD_PARTY_DIR}/json-schema-validator/src")                     # compiled: the six upstream sources
add_library(${ESSES_PREFIX}_tp_json_schema_validator STATIC
            ${_jsv}/json-patch.cpp ${_jsv}/json-schema-draft7.json.cpp ${_jsv}/json-uri.cpp
            ${_jsv}/json-validator.cpp ${_jsv}/smtp-address-validator.cpp ${_jsv}/string-format-check.cpp)
target_include_directories(${ESSES_PREFIX}_tp_json_schema_validator SYSTEM PUBLIC "${_jsv}")
target_link_libraries(${ESSES_PREFIX}_tp_json_schema_validator PUBLIC ${ESSES_NAMESPACE}::nlohmann_json)
esses_silence_third_party(${ESSES_PREFIX}_tp_json_schema_validator)   # not our code: no warnings, never warnings-as-errors
add_library(${ESSES_NAMESPACE}::json_schema_validator ALIAS ${ESSES_PREFIX}_tp_json_schema_validator)
```

Updating a vendored library is a PR that touches only `third_party/<lib>/`, `third_party/README.md` (version, route, source, SHA-256), `NOTICES.md` and `REUSE.toml`; a new download needs the user's approval (D-147). Adopting a package manager later would be a new decision amending D-147, and the alias names would stay unchanged.

#### 3.7.3 ADS acquisition

`cmake/FindOrFetchADS.cmake` uses `FetchContent_Declare(ads GIT_REPOSITORY … GIT_TAG <pinned>)` with `BUILD_STATIC OFF`, `BUILD_EXAMPLES OFF`, `ADS_VERSION` from the tag. ADS 4.x names its library `qt${QT_VERSION_MAJOR}advanceddocking`, so against Qt 6 the target is `qt6advanceddocking` (its own alias `ads::qt6advanceddocking`) and the DLL is `qt6advanceddocking.dll`; there is no `ads::qtadvanceddocking`. After `FetchContent_MakeAvailable(ads)`:

```cmake
add_library(esses::ads ALIAS qt6advanceddocking)              # the only name the tree links
set_target_properties(qt6advanceddocking PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # ADS sets its own dirs otherwise
```

ADS is built in the same preset, from source, against the same Qt it links; on Linux it links `Qt6::GuiPrivate`, which ties the binary to that exact Qt minor version, so it is never reused across the 6.7.2 (local) and 6.12 (CI) kits. The pinned tag is one verified to build against both. `install()` copies `$<TARGET_FILE:qt6advanceddocking>` next to the executable (§3.11). vcpkg's `qtadvanceddocking` port is not used because it depends on vcpkg's `qtbase`, which would build Qt through vcpkg against D-006 (§13).

#### 3.7.4 Qt acquisition

- Local: the existing `C:/Qt/6.7.2/msvc2019_64` kit (MSVC 2019 ABI is binary-compatible with the VS 2022 v143 toolset); `CMAKE_PREFIX_PATH` in `CMakeUserPresets.json`.
- CI and releases: `jurplel/install-qt-action@v4` with `version: '6.12.0'`, `arch: win64_msvc2022_64 | linux_gcc_64 | clang_64`, `cache: true`; modules none beyond qtbase/qtsvg until `qtmultimedia` is needed.
- Local 6.12 (U-08): **recommended in M0 week 1**, not optional at M4: `aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 -O C:/Qt` (about an hour) and a second user preset `dev-612`. It removes CI-only failures and lets the developer reproduce the golden-image platform's Qt version; the 6.7.2 kit stays for the `find_package(Qt6 6.7)` minimum check. Proposed as U-08's new default in DECISIONS.md Addendum A (07 §14 to be aligned).
- `find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport Test)` only inside `if(ESSES_BUILD_QT)`; `qt_standard_project_setup()`; `CMAKE_AUTOMOC ON` on Qt targets only.

#### 3.7.5 Finding DLLs on Windows

A Windows executable starts only if every DLL it imports is next to it or on `PATH`; otherwise it fails with `0xC0000135` (this includes test executables run by `ctest`).

1. **Vendored dependencies:** none at run time, because every vendored library is header-only or a static library built in the tree (§3.7.2, D-147).
2. **Our targets and ADS:** `CMAKE_RUNTIME_OUTPUT_DIRECTORY`/`CMAKE_LIBRARY_OUTPUT_DIRECTORY` = `${CMAKE_BINARY_DIR}/bin`, set before `FetchContent_MakeAvailable(ads)` and forced on the ADS target (§3.7.3), so `qt6advanceddocking.dll` sits beside every executable and test.
3. **Qt:** found through `PATH`, not copied. Locally the user presets put `<kit>/bin` on `PATH` (§3.6); in CI `install-qt-action` does. `esses_add_test()` also sets `ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"` on every Qt-linked test, so a bare `ctest` works too. Qt DLLs are not copied with `$<TARGET_RUNTIME_DLLS>`, because the platform plugins (`platforms/qoffscreen.dll`, `qwindows.dll`) would not come with them and Qt would fail to initialise.
4. **Starting `esses.exe` from Explorer** in a build tree: `cmake --build --preset dev --target deploy-dev` runs `windeployqt --no-translations --dir build/dev/bin` once.

### 3.8 Warnings, warnings-as-errors, clang-tidy, clang-format

`cmake/warnings.cmake`:

```cmake
function(esses_apply_warnings t)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W4 /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296
                                        /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14619 /w14640
                                        /w14826 /w14905 /w14906 /w14928 /wd4251)
    if(ESSES_WARNINGS_AS_ERRORS) target_compile_options(${t} PRIVATE /WX) endif()
  else()
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
                                        -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference
                                        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
    if(ESSES_WARNINGS_AS_ERRORS) target_compile_options(${t} PRIVATE -Werror) endif()
  endif()
endfunction()
```

`-Werror`/`/WX` is on only in CI presets so a compiler upgrade never blocks local work; CI is the gate (D-034). Third-party and generated sources (moc, vendored) are marked `SYSTEM` includes and excluded.

`.clang-tidy` (applied to Qt-free targets only, D-034):

```yaml
Checks: >
  -*, bugprone-*, performance-*, modernize-*, readability-identifier-naming,
  cppcoreguidelines-pro-type-member-init, cppcoreguidelines-slicing, misc-unused-*,
  -modernize-use-trailing-return-type, -bugprone-easily-swappable-parameters
WarningsAsErrors: '*'
HeaderFilterRegex: 'libs/(base|units|store|model|formats|analysis|maths|project|plotcore)/'
CheckOptions:
  readability-identifier-naming.ClassCase: CamelCase
  readability-identifier-naming.FunctionCase: camelBack
  readability-identifier-naming.VariableCase: camelBack
  readability-identifier-naming.PrivateMemberSuffix: '_'
  readability-identifier-naming.EnumConstantCase: CamelCase
  readability-identifier-naming.ConstexprVariableCase: CamelCase
  readability-identifier-naming.ConstexprVariablePrefix: 'k'
```

`.clang-format`: `BasedOnStyle: LLVM`, `IndentWidth: 4`, `ColumnLimit: 110`, `AllowShortFunctionsOnASingleLine: Inline`, `BreakBeforeBraces: Attach`, `PointerAlignment: Left`, `IncludeBlocks: Regroup` with categories (own `esses/` headers, third-party, std), `SortIncludes: CaseSensitive`. `lint` runs `clang-format --dry-run --Werror` on every `.cpp/.hpp/.h` outside `third_party/`.

### 3.9 Configure-time Qt-free check

`cmake/qt-free-check.cmake` runs at the end of configure (`cmake_language(DEFER)`) over every target marked `ESSES_QT_FREE` and fails configure on either of two findings; the definitive proof is the `core` CI job on runners with no Qt installed (D-007).

```cmake
function(esses_mark_qt_free target)
  set_property(TARGET ${target} PROPERTY ESSES_QT_FREE TRUE)
  set_property(GLOBAL APPEND PROPERTY ESSES_QT_FREE_TARGETS ${target})
endfunction()

function(_esses_links_of target out)
  set(libs "")
  get_target_property(type ${target} TYPE)
  if(NOT type STREQUAL "INTERFACE_LIBRARY")
    get_target_property(l ${target} LINK_LIBRARIES)
    if(l) list(APPEND libs ${l}) endif()
  endif()
  get_target_property(il ${target} INTERFACE_LINK_LIBRARIES)
  if(il) list(APPEND libs ${il}) endif()
  list(TRANSFORM libs REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1")   # unwrap generator expressions
  set(${out} "${libs}" PARENT_SCOPE)
endfunction()

function(esses_qt_free_check)
  get_property(targets GLOBAL PROPERTY ESSES_QT_FREE_TARGETS)
  foreach(t IN LISTS targets)
    # (1) transitive link closure must contain no Qt target
    set(todo ${t}) ; set(seen "")
    while(todo)
      list(POP_FRONT todo cur)
      if(cur IN_LIST seen) continue() endif()
      list(APPEND seen ${cur})
      if(cur MATCHES "^(Qt6?::|Qt6?[A-Z]|ads::|esses::ads$|qt6advanceddocking)")
        message(FATAL_ERROR "qt-free-check: ${t} links ${cur} (via ${seen})")
      endif()
      if(TARGET ${cur}) _esses_links_of(${cur} more) ; list(APPEND todo ${more}) endif()
    endwhile()
    # (2) no source or public header may include a Qt header or use QString
    get_target_property(srcs ${t} SOURCES)
    get_target_property(dir  ${t} SOURCE_DIR)
    file(GLOB_RECURSE hdrs "${dir}/include/*.hpp" "${dir}/include/*.h" "${dir}/src/*.hpp" "${dir}/src/*.h")
    foreach(s IN LISTS srcs hdrs)
      if(NOT IS_ABSOLUTE "${s}") set(s "${dir}/${s}") endif()
      if(NOT EXISTS "${s}") continue() endif()
      # CMake regex has no \b (it matches a literal 'b'), so word boundaries are spelled out as character classes.
      # Include: <QString>, "QString", <QtCore>, <QtCore/QString>, and lower-case <qglobal.h>.
      file(STRINGS "${s}" hits REGEX "^[ \t]*#[ \t]*include[ \t]*([<\"]Q[A-Za-z]|<q[a-z0-9_]+\\.h>)")
      if(hits) message(FATAL_ERROR "qt-free-check: ${s} includes Qt: ${hits}") endif()
      file(STRINGS "${s}" qs REGEX "(^|[^A-Za-z0-9_])Q(String|Object|Image|Painter|Variant|Byte[A-Z][A-Za-z]*|List|Map|Hash)([^A-Za-z0-9_]|$)")
      if(qs) message(FATAL_ERROR "qt-free-check: ${s} uses a Qt type: ${qs}") endif()
    endforeach()
  endforeach()
  message(STATUS "qt-free-check: ${targets} are Qt-free")
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL esses_qt_free_check)
```

The `lint` job also runs the negative tests: it injects, one at a time, `#include <QString>`, `#include <QtCore>`, `#include <qglobal.h>` and the line `QString s;` into a scratch copy of `libs/base/src/rate.cpp` and asserts that configure fails each time, plus one positive case (`Quantity q;`, `QuantityMap m;`) that must pass (AC-M0-07, 01 §11 item 2). This file is the only implementation of the check; 01 §2.3 refers here instead of keeping its own sketch.

### 3.10 Version stamping

`cmake/version.cmake` runs `git describe --tags --dirty --always` at configure, parses `vMAJOR.MINOR.PATCH[-pre][+N-gHASH]`, and configures `esses/version.hpp` with `ESSES_VERSION_STRING`, `ESSES_GIT_SHA`, `ESSES_BUILD_DATE` (ISO, from `SOURCE_DATE_EPOCH` when set for reproducibility) and `ESSES_QT_VERSION` (when built with Qt). `esses --version` and `esses-cli --version` print all four plus the renderer backend. `esses.exe` is a `WIN32`-subsystem executable with no console, so for `--version` (and `--help`) it first calls `AttachConsole(ATTACH_PARENT_PROCESS)` and reopens `stdout`; scripts and CI use `esses-cli --version`, which is a console program.

### 3.11 Install and export

`install(TARGETS esses esses-cli esses-render)` to `bin/`, the ADS shared library (`$<TARGET_FILE:qt6advanceddocking>`) beside them, `data/` to `share/esses/data`, `NOTICES.md` and `THIRD_PARTY_LICENSES.md` to `share/esses/licenses`. At v1.0, `sdk/` installs `include/esses/esses_abi.h` and `lib/cmake/EssesSDK/EssesSDKConfig.cmake` so `find_package(EssesSDK)` works out of tree (AC-M6-02).

## 4. CI matrix (GitHub Actions)

### 4.1 Jobs

| Job | Workflow | Runner(s) | Preset | Qt | Runs | Trigger | Budget |
|---|---|---|---|---|---|---|---|
| `core` | `ci.yml` | ubuntu-24.04, windows-2022, macos-14 | `ci-linux-gcc-core`, `ci-win-msvc-core`, `ci-mac-clang-core` | none installed | Catch2 unit + property tests, golden JSON, `DrawList` JSON goldens (the cross-platform strict check, §6.4), `fixture verify`, CLI smoke | PR + push | ≤ 6 min |
| `qt-app` (from M1, D-150) | `ci.yml` | windows-2022 (MSVC) and ubuntu-24.04 (GCC) on PR + push; macos-14 (Apple Clang) nightly, not merge-blocking | `ci-win-msvc-release`, `ci-linux-gcc`, `ci-mac-clang` | 6.12.0 via `install-qt-action@v4` | build all; Qt Test offscreen (`QT_QPA_PLATFORM=offscreen`); keymap behaviour tests; render parity; **pixel golden images authoritative on ubuntu only**, render smoke tests (renders without error, image non-empty, size and DPR correct) on Windows and macOS (§6.4) | PR + push (macOS: nightly) | ≤ 20 min |
| `sanitize` | `ci.yml` | ubuntu-24.04 | `linux-clang-asan`, `linux-clang-tsan` | none | ASan+UBSan on all Qt-free tests; TSan on `esses_base_tests`, `esses_store_tests` (including the `tryPin`-versus-eviction test, 01 §5.3 rule 8), `esses_maths_tests`, `esses_plotcore_tests` (D-031); plus a nightly Linux TSan build with a TSan-instrumented Qt for `tests/qtrender/text_metrics_concurrency.cpp` (04 §5.4) | PR + push (Qt TSan: nightly) | ≤ 15 min |
| `fuzz-smoke` | `ci.yml` | ubuntu-24.04 | `linux-clang-fuzz` | none | each target 60 s with corpus seeds | PR + push | ≤ 8 min |
| `lint` | `ci.yml` | ubuntu-24.04 | `ci-linux-gcc-core` + `ESSES_ENABLE_CLANG_TIDY` | none | clang-format, clang-tidy (changed Qt-free files on PR, all nightly), `reuse lint`, qt-free-check positive + negative, layer-check negative test, `check_vendor_marks.py`, `check_layering.py`, `check_keymap_coverage.py`, forbidden-path check (§1), schema validation of every JSON in `data/`, `schemas/`, `fixtures/`, `tests/maths/` | PR + push | ≤ 10 min |
| `bench` | `bench.yml` | the **reference machine** as a self-hosted runner (gate, D-042); ubuntu-24.04 hosted (non-blocking alarm) | `bench` | none (plot bench needs `esses_qtrender` → a second `bench-qt` variant with Qt) | Google Benchmark JSON. Gate: absolute limits of 07 §12, median of ≥ 5 repetitions, `tools/bench_compare.py --limits bench/workloads.md`. Hosted: the same benchmarks as a non-blocking ±30 % regression alarm against their own baseline | gate: push to `main`, nightly, tags, manual dispatch, **never** on pull requests from forks; hosted alarm: nightly | ≤ 30 min |
| `fuzz` | `fuzz.yml` | ubuntu-24.04 | `linux-clang-fuzz` | none | 30 min per target; new crashes uploaded as artefacts and minimised | nightly | ≤ 3 h |
| `soak` | `bench.yml` | ubuntu-24.04 (ASan build with Qt) | `linux-gcc` + sanitizer | 6.12 | `tests/soak/saturday` (M4+): 30 min nightly, 2 h before each tag; LSan suppressions `tests/soak/lsan.supp` for Qt, fontconfig and glib; a leak counts only if its stack has an `esses_*` frame (AC-M4-03) | nightly (30 min); pre-tag (2 h) | ≤ 2.5 h |
| `package` | `release.yml` | windows-2022, macos-14, ubuntu-22.04 (older glibc for AppImage) | `ci-win-msvc-release`, `ci-mac-clang`, `ci-linux-gcc` | 6.12 | windeployqt + Inno Setup + portable zip (release-blocking); CLI tarballs for Linux and macOS (release-blocking); macdeployqt + DMG and linuxdeploy AppImage (best-effort at 0.1: a failure does not block the Windows release; artefacts labelled "builds green, not supported" until U-09; launch smoke test on the same runner); `NOTICES.md`, `THIRD_PARTY_LICENSES.md`, SBOM, sha256sums; GitHub Release draft | tags `v*` | ≤ 40 min |
| `abi-compat` | `abi-compat.yml` | windows-2022, ubuntu-24.04, macos-14 | `*-release` | 6.12 (host) | build `plugins/examples/c` with Clang and MinGW-GCC, load into the MSVC host; Rust example if cheap | PR touching `sdk/`; nightly | v1.0 (D-028) |

### 4.2 Workflow excerpt (`ci.yml`)

As built in M0, `.github/workflows/ci.yml` is the source of truth and differs from this excerpt: it has no vcpkg steps, because every library dependency is vendored (D-147); the `qt-app` job below arrives in M1 (D-150); sccache is not configured yet; and a `fuzz-smoke-windows` job also runs the fuzz targets with MSVC libFuzzer and ASan.

```yaml
name: ci
on: { pull_request: {}, push: { branches: [main] }, schedule: [ { cron: '0 3 * * *' } ] }   # schedule: nightly jobs
concurrency: { group: ci-${{ github.ref }}, cancel-in-progress: true }
env:
  SCCACHE_GHA_ENABLED: "true"                # sccache stores objects in the Actions cache (§4.3)
jobs:
  core:
    strategy: { fail-fast: false, matrix: { include: [
      { os: ubuntu-24.04, preset: ci-linux-gcc-core },
      { os: windows-2022, preset: ci-win-msvc-core },
      { os: macos-14,     preset: ci-mac-clang-core } ] } }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
        with: { submodules: false }           # fixtures/private only when the deploy key secret exists
      - uses: lukka/get-cmake@latest           # Ninja + CMake
      - uses: mozilla-actions/sccache-action@v0.0.9   # pin the then-current tag at bootstrap
      - uses: ilammy/msvc-dev-cmd@v1          # no-op outside Windows
      - run: cmake --preset ${{ matrix.preset }}                  # ci-* presets carry -Werror or /WX, /Z7 and sccache
      - run: cmake --build --preset ${{ matrix.preset }}
      - run: ctest --preset ${{ matrix.preset }}
      - run: ./build/${{ matrix.preset }}/bin/esses-cli fixture verify fixtures/
        shell: bash
  qt-app:
    strategy: { fail-fast: false, matrix: { include: [
      { os: windows-2022, preset: ci-win-msvc-release, qtarch: win64_msvc2022_64 },
      { os: ubuntu-24.04, preset: ci-linux-gcc,        qtarch: linux_gcc_64 } ] } }
      # a separate job qt-app-macos (macos-14, ci-mac-clang, clang_64) has the same steps and
      # `if: github.event_name == 'schedule'`, so it runs nightly and never blocks a merge (§4.4)
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - uses: jurplel/install-qt-action@v4      # also puts the Qt bin directory on PATH (§3.7.5)
        with: { version: '6.12.0', arch: ${{ matrix.qtarch }}, cache: true }
      - if: runner.os == 'Linux'
        run: sudo apt-get install -y libgl1 libegl1 libxkbcommon0 libfontconfig1 libdbus-1-3
      - uses: mozilla-actions/sccache-action@v0.0.9
      - uses: ilammy/msvc-dev-cmd@v1
      - run: cmake --preset ${{ matrix.preset }} -DCMAKE_PREFIX_PATH="$QT_ROOT_DIR"
      - run: cmake --build --preset ${{ matrix.preset }}
      - run: ctest --preset ${{ matrix.preset }}
        env: { QT_QPA_PLATFORM: offscreen, ESSES_TEST_FONT_DIR: ${{ github.workspace }}/tests/golden/fonts }
      - uses: actions/upload-artifact@v4
        if: failure()
        with: { name: golden-diffs-${{ matrix.os }}, path: build/${{ matrix.preset }}/golden-diffs/ }
```

### 4.3 Caching

- **Library dependencies:** nothing to cache, because every library is vendored and built in the tree (D-147); the vcpkg binary cache this section first specified is not used.
- **Qt:** `install-qt-action` cache.
- **Objects:** `sccache` on all three OSes, keyed on preset + compiler. On MSVC the `ci` presets set `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` (`/Z7`), because sccache cannot cache objects compiled with `/Zi` (the RelWithDebInfo default writes a shared PDB). The `ci` presets set sccache as the compiler launcher, so every workflow job that configures a `ci-*` preset (`ci.yml`, `release.yml`) installs it with `mozilla-actions/sccache-action` first. Not yet configured in M0 (§3.6, §4.2).
- **Fixtures:** remote files cached by sha256 under `~/.cache/esses-fixtures`.

### 4.4 Required checks and branch rules

`main` is protected: required checks `core (×3)`, `qt-app` on Windows and Linux (from M1, D-150), `sanitize`, `fuzz-smoke`, `lint`. `qt-app` on macOS runs nightly and is not merge-blocking: the developer works on Windows and may have no Mac (U-09), D-034 asks Linux and macOS to "build green", not to gate every merge, and a red macOS nightly opens an issue instead. Squash merges; DCO sign-off enforced by the `DCO` GitHub app or a `lint` step (`git log --format=%B` must contain `Signed-off-by:`); the self-hosted `bench` gate is required only on release tags (D-042). From M6, `abi-compat` is required for PRs touching `sdk/`.

### 4.5 Private fixtures in CI

The `fixtures/private` submodule is checked out only when the `FIXTURES_DEPLOY_KEY` secret is present (never on fork PRs); `esses-cli fixture verify` skips fixtures whose `input` is absent and reports them as "skipped (private)", so public CI is always green and complete on the maintainer's runs (D-005).

## 5. Lint job details

| Check | Tool | Scope | Failure means |
|---|---|---|---|
| Format | `clang-format --dry-run --Werror` | all C/C++ outside `third_party/` | unformatted file |
| Static analysis | clang-tidy via `CMAKE_CXX_CLANG_TIDY` | Qt-free targets (D-034) | any warning (treated as error) |
| Licence hygiene | `reuse lint` | whole tree | a file without SPDX info or a missing licence text |
| Layering | `tools/check_layering.py`, plus the configure-time layer check of §3.4 and its negative test | `compile_commands.json` + include lines | an include outside declared `DEPS` or an upward dependency |
| Forbidden paths | a `git ls-files` check | whole tree | a tracked path under `research/`, `help_text/`, `conv_samples/`, or a `.chm/.i2wkb/.i2wsp/.mcs/.mfs/.mt2` file outside `fixtures/private` (§1) |
| Qt-free | configure with `ESSES_BUILD_QT=OFF`, plus the negative injection test | layers 0–3 | Qt leaked below the line |
| Vendor marks | `tools/check_vendor_marks.py` | identifiers, string literals, file names, asset names, JSON values under `libs/ apps/ plugins/ sdk/ data/ schemas/ tests/ui/ tools/packaging/` | a vendor mark outside the allow-list (D-003) |
| Keymap coverage | `tools/check_keymap_coverage.py` | `data/keymaps/default.json` vs `tests/ui/keymap/` | an `implemented` row without `KEYMAP_TEST("<command.id>")` |
| Schemas | `esses-cli validate --all` (built in the same job) | every JSON in `data/`, `fixtures/`, `tests/maths/`, `schemas/` (meta-schema) | invalid document, with JSON pointer |
| Docs | `markdownlint`, link check | `docs/`, root `*.md` | broken link |

`check_vendor_marks.py` runs two scans (00 §8 rule 3):

1. **Words** in string literals, comments, JSON values, file and asset names: `(?i)\bmotec\b|\bi2\b|i2pro|\bi2w(kb|sp)\b`.
2. **Identifiers** (C++, CMake, Python, JSON keys) under `libs/ apps/ plugins/ sdk/`: case-insensitive substring `motec`, or a token equal to `i2`, `i2pro`, `i2wkb`, `i2wsp` or `mtc` after splitting at `_`, `-`, `.` and lower→upper case changes. This catches `I2MathsImporter`, `xI2`, `IMtcMathPlugin` and a loop variable named `i2`, which the word regex misses; `kPi2` (tokens `k`, `Pi2`) passes.

The allow-list (`tools/vendor_marks_allowlist.txt`) has exactly the classes of 00 §8 rule 2: (a) importer ids `motec.ld`, `motec.csv` (and the v1.0 configuration-importer ids when assigned) with their dictionary and profile file names; (b) import/export dialog format labels that name the file formats (nominative use, e.g. `"MoTeC log file (*.ld)"`); (c) the format-name literal that the MoTeC CSV writer emits on line 1 and that the `motec.csv` profile's sniff pattern matches, held as one named constant in `libs/formats/` and one field in `data/profiles-text/motec.csv.json` ([WRITER_UNRESOLVED #70](background/design-panel/WRITER_UNRESOLVED.md)); (d) the D-019 round-trip bag: the JSON key `x-i2` in `schemas/workbook-1.schema.json`, `schemas/maths-1.schema.json` and test fixtures, and the C++ member `xI2` (outline §4); (e) prose in `CLEANROOM.md`, `NOTICES.md`, `TRADEMARK.md`, `docs/`, `fixtures/**/README.md` and `fixture.json` `source` fields. The v1.0 i2 configuration-file importers live in `libs/project/src/cfgimport/` with neutral names (`CfgWorkbookImporter`, `CfgSchemeImporter`, `CfgMathsImporter`) and need no allow-list class of their own. Everything else fails.

`check_text_overlap.py` (not in public CI): flags any run of ≥ 12 words shared between a tracked text file (`docs/`, `*.md`, string literals, keymap `doc` strings, tooltips) and the local reference corpus (the i2 help pages as displayed by the shipped help viewer, and the GPL reference sources). The corpus cannot be committed (§1), so the check runs on the maintainer's machine or a private runner that has it; it is a release-checklist item (§8) and is run before merging any PR that adds user documentation or UI text.

## 6. Testing strategy per layer

### 6.1 Overview

| Layer / target | Framework | What | Location | CI job |
|---|---|---|---|---|
| `base`, `units`, `store`, `model` | Catch2 v3 unit + property (seeded `GENERATE`) | `Rate` arithmetic, `TaskPool` priorities/coalescing/cancel, unit round-trips and delta/absolute, pyramid/M4 vs brute force, validity modes, resampler boundaries and monotonicity, marker/range identity, sidecar merge precedence | `libs/<lib>/tests/` | `core`, `sanitize` |
| `formats` | Catch2 + fixture oracle + fuzz | `.ld` dialect matrix, `.ldx` unknown-element round-trip, writer round-trip, text engine profiles, sniff ranking, `probe_json` canonical equality | `libs/formats/tests/`, `fixtures/`, `tests/fuzz/` | `core`, `fuzz-smoke` |
| `analysis` | Catch2 + golden JSON | beacon-pair semantics, `DistanceIndex` round trips, variance vs brute force, track generation vs GPS, report cells vs hand-computed | `libs/analysis/tests/`, `tests/golden/json/` | `core` |
| `maths` | Catch2 + case files + fuzz + TSan | parser arities/aliases/precedence, type inference warnings, every kernel class, graph shadowing/cycles/versions, `SelectionDependent` keys | `libs/maths/tests/`, `tests/maths/*.case.json` | `core`, `sanitize` |
| `project` | Catch2 | schema validation, migrations, canonical byte-identical round-trip per file kind, journal replay, unknown-type preservation | `libs/project/tests/` | `core` |
| `plotcore` | Catch2 + `DrawList` goldens + TSan | `DomainMapper`, `TraceBuilder` vs brute force at every zoom, layouts, hit testing; `DrawList` JSON goldens for 8+ worksheet cases | `libs/plotcore/tests/`, `tests/golden/drawlist/` | `core`, `sanitize` |
| `cli` | ctest scripts | golden JSON of `info/channels/laps --json`, `validate` exit codes, `fixture verify` | `tests/golden/json/` | `core` |
| `qtrender` | golden images (authoritative on ubuntu only) + render smoke tests (Windows, macOS) | every component at DPR 1 and 2; render parity with the on-screen composite | `tests/golden/images/` | `qt-app` |
| `qtmodel`, `widgets`, `app` | Qt Test (offscreen) | one test per keymap row; selection model; F9 preservation; like-units grouping; N/P; CTRL+F rules; properties round-trip; undo property; walkthroughs M1/Saturday | `tests/ui/` | `qt-app` |
| whole app | soak | scripted Saturday session under ASan, 30 min nightly and 2 h before tags | `tests/soak/` | `soak` |
| perf | Google Benchmark | every §12 budget row of 07_ROADMAP.md | `bench/` | `bench` |

### 6.2 Catch2 conventions

Test executables `esses_<lib>_tests` link the vendored Catch2 3.7.1 (amalgamated) and are registered by the project's own discovery (`cmake/catch_discover.cmake`, D-148), so each `TEST_CASE` is one ctest entry named `<library>.<test name>`. A test name that a Catch2 test spec cannot select exactly (a leading or trailing `*`, an `exclude:` prefix, a line break, or two names in one executable that are equal once case and surrounding spaces are ignored) fails the build. Tags: `[unit]`, `[property]`, `[slow]` (excluded from PR runs when > 2 s), `[oracle]` (needs private fixtures; skipped when absent). Property tests use a seed from `ESSES_TEST_SEED` (default fixed; CI nightly randomises and prints the seed on failure). Coverage target 80 % lines on `store/model/formats/maths/analysis/plotcore` (D-033), measured with `llvm-cov` in a weekly job, informational only.

### 6.3 Golden JSON via the CLI

- Canonical JSON is the single algorithm of 02 §6.5 rule 1, `JsonDoc::canonical()`, used by the CLI, fixtures and round-trip tests: `$schema` then `schemaVersion` first, all other keys sorted by UTF-8 code units, 2-space indent, LF, UTF-8 without BOM, no trailing whitespace; numbers in shortest round-trip form (`std::to_chars`, so `0.0` prints as `0`), `-0` written as `0`; non-finite numbers are a serialisation error (schemas use `null` for an invalid value).
- Layout: `tests/golden/json/<formatId>/<fixture>/{info,channels,laps}.json`; ctest runs `esses-cli <cmd> <input> --json --canonical` and diffs with the committed file; a mismatch prints a unified diff.
- Update workflow: `tools/golden_update.py --json <fixture>` regenerates after a reviewed behaviour change; the PR must say why.

### 6.4 Golden images via `OffscreenRenderer`

- Cases are JSON files `tests/golden/images/<component>/<case>.golden.json` naming a workbook, worksheet, source fixtures, `ViewState`, `SelectionSnapshot`, size and DPR list `[1, 2]`; expected PNGs `<case>@1x.png`, `<case>@2x.png`.
- **One authoritative platform.** Pixel goldens are compared only on `ubuntu-24.04`, Qt 6.12, `QT_QPA_PLATFORM=offscreen`. Pinned fonts do **not** make text identical across operating systems: the offscreen platform still uses each OS's font engine (FreeType/fontconfig on Linux, the Windows font engine on Windows, CoreText on macOS), so hinting, antialiasing and glyph positions differ. On Windows and macOS the same cases run as **render smoke tests**: they must render without error, produce a non-empty image of the expected size and DPR, and their `DrawList` must equal the JSON golden.
- Rendering: `RasterBackend`, fonts forced from `ESSES_TEST_FONT_DIR` = `tests/golden/fonts/`, holding Noto Sans (OFL-1.1, the family of the shipped schemes' font roles, 06 §16; D-025, D-127) with its licence file, REUSE-annotated `OFL-1.1` and excluded from the `data/**` MIT annotation (§10), for determinism on the authoritative platform; the font is for goldens only and the 0.1 installer does not bundle it (users without Noto Sans get Qt family substitution, 06 §16 rule 6); `QT_ENABLE_HIGHDPI_SCALING=0` with explicit DPR in `OffscreenRenderer`.
- Diff metric: a pixel differs when any channel differs by > 8/255; fail when > 0.5 % of pixels differ; the diff image and both PNGs are uploaded on failure. When `GlBackend` exists it is compared to the raster goldens at ≤ 2 % (D-033).
- **Regeneration only from CI.** Expected PNGs are never rendered on the developer's machine (Windows, Qt 6.7.2 or 6.12): `tools/golden_update.py --images` downloads the `golden-candidates` artefact of the ubuntu `qt-app` run for the PR and commits it after review.
- **Bounded matrix.** Every MVP component × {light, dark} × {1×, 2×} at ≤ 3 cases each, and `time_distance` as ≤ 12 named cases (for example: single trace, 20 traces, overlapped group, two panels, distance with overlay, datum on, measurements on, variance on, offset axis on, bands and annotations, status strip, "no distance data" banner) instead of the cross product of those toggles (04 §6.7 item 3 uses the same cap). This also keeps AC-M3-02 (≥ 12 `time_distance` cases) satisfied.
- The strict, platform-independent companion is the `DrawList` JSON golden of the same case in the `core` job on all three OSes (geometry, colours, text runs), which catches layout changes without font noise.

### 6.5 Behaviour tests per hotkey

`data/keymaps/default.json` rows:

```json
{ "command": "data.next_range", "keys": ["N"], "context": "worksheet", "status": "implemented", "milestone": "M1",
  "doc": "Move Main to the next displayed range, preserving zoom" }
```

The row shape is 06 §14.3's (`command`, `keys`, `context`, `args?`, `status`, `milestone`, `doc`; snake_case command ids; contexts `global | worksheet | component | graph | summary | track | data_window | channels_window | values_window`). Each implemented row has exactly one Qt Test case registered with `KEYMAP_TEST("data.next_range")` in `tests/ui/keymap/<area>_tests.cpp`. The harness opens a fixture, sets a known state, calls `KeymapBinder` with the row's keys, and asserts the state change through `DataSelectionModel`/`WorksheetViewModel` (never through pixels). `tools/check_keymap_coverage.py` mirrors 06 §14.5: it fails on an `implemented` row without a test, on a test without a row, and on a row whose `milestone` is at or before the current milestone but whose status is not `implemented`; `planned` rows are exempt until their milestone, and `reserved` rows are covered by one generic row asserting the command is registered, disabled and shows its tooltip (07_ROADMAP.md §15 item 8). The command palette test reuses the same harness by command id.

### 6.6 Maths case files

Schema `maths-case-1` is defined once, in 03 §14.1, which owns it; this section covers only location, runner, lint and provenance, and the example follows 03's field names (`precision: "single"`, `valid` as 0/1 arrays, `tolerance` and `diagnostics` inside `expected`). The fields this document's tooling relies on are those 03 §14.1 defines: `$schema`, `schemaVersion`, `id`, a `provenance` object, and `expected.tolerance` as an object `{abs, rel}`. `precision` is persisted as `single` | `double` (the 03 §4.1 vocabulary). Location: `tests/maths/*.case.json` (D-033; one directory, no `cases/` subdirectory).

```json
{
  "$schema": "urn:esses:maths-case-1", "schemaVersion": 1,
  "id": "smooth_odd_window_edges_truncated",
  "expression": "smooth('Throttle', 5)",
  "rate": "auto", "compat": { "unaryMinusAbovePower": false }, "precision": "single",
  "inputs": { "Throttle": { "rate": { "num": 100, "den": 1 }, "unit": "%",
                            "values": [0, 10, 20, 30, 40, 50, 60, 70, 80, 90], "valid": [1,1,1,1,1,1,1,1,1,1] } },
  "ranges": null, "selection": null,
  "expected": { "rate": { "num": 100, "den": 1 }, "unit": "%",
                "values": [10, 15, 20, 30, 40, 50, 60, 70, 75, 80], "valid": [1,1,1,1,1,1,1,1,1,1],
                "tolerance": { "abs": 1e-5, "rel": 1e-6 }, "diagnostics": [] },
  "provenance": { "kind": "help", "ref": "maths function reference: smooth",
                  "note": "integer window = samples; odd window centred; at the extent edges the window is truncated to the available samples and the output stays valid (03 §5.2, compat row smooth.edges)" }
}
```

The expected values follow 03 §5.2: sample 0 averages samples 0–2 (10), sample 1 averages 0–3 (15), sample 8 averages 6–9 (75), sample 9 averages 7–9 (80), and every output is valid. `provenance.kind ∈ {help, release-notes, derived, oracle, public-maths}` (03 §14.1); `oracle` cases live only in `fixtures/private` (D-005). A case that encodes a **[compat]** reading names its `docs/maths-compat.md` row; changing a reading (for example making `smooth` edges invalid) changes 03 §5.2, that row and the affected cases in one PR. The runner is a Catch2 generator over the directory plus `esses-cli eval --case <file>` for ad-hoc use. Every function, arity and documented edge rule in `docs/maths-compat.md` must be referenced by at least one case id (checked by `lint`).

### 6.7 Fixture corpus

Layout per D-033:

```
fixtures/motec.ld/synthetic-acc-dialect/      # public: our own writer emitting the ACC (sim-exporter) dialect
  fixture.json                             # inputs generated by esses-gen-fixtures; > 2 MB inputs are not committed
  expected.probe.json  expected.markers.json  expected.laps.json
  expected.channels/Ground Speed.csv.gz  ...
  README.md                                # provenance, quirks
fixtures/private/motec.ld/acc-spa-2024/   # private submodule: a real ACC export, licence not cleared
  input.ld  input.ldx  fixture.json  expected.*  README.md
```

`fixture-1` is defined once, in 05 §9.1, and this section follows it: `input {file, sha256, bytes, remote}`, companions beside the input with the same stem, `source`, `licence`, `permission`, `private`, `importOptions` (the reader options such as `{ "ld": { "shiftOrder": "afterMul" } }`, D-037), `dialect` as the string `probe_json` reports, and `oracle {kind, ref, permitted, tolerance}` with per-dtype tolerances (`F32`, `F64`, `I32`, `timesUs`). `fixture.json` of the real ACC file:

```json
{ "$schema": "urn:esses:fixture-1", "schemaVersion": 1,
  "format": "motec.ld", "name": "acc-spa-2024",
  "input": { "file": "input.ld", "sha256": "…", "bytes": 48213456, "remote": null },
  "companions": [ { "file": "input.ldx", "sha256": "…" } ],
  "source": "ACC built-in telemetry export, generated by the maintainer",
  "licence": "LicenseRef-unknown", "permission": "private: third-party game output, nothing real is public for now (Q-04)", "private": true,
  "importOptions": { "ld": { "shiftOrder": "afterMul" } },
  "dialect": "sim-exporter",
  "oracle": { "kind": "public-project", "ref": "motec-i2 0.2.0 (MIT)", "permitted": true,
              "tolerance": { "F32": "1ulp", "F64": "1ulp", "I32": "exact", "timesUs": "exact" } },
  "notes": "num_channs disagrees with the list by one" }
```

The maintainer cannot dedicate to CC0 any rights that Kunos or MoTeC may hold in game-generated output, so a real ACC file is an ordinary third-party fixture: it stays in `fixtures/private` with `licence: LicenseRef-unknown`, private for now like every real file (Q-04), and enters the public tree only if the game's terms permit redistribution. ACC is not MoTeC-licensed (the user's answer of 2026-10-10), so i2 is never run on it (D-038 (a)). Public CI uses `synthetic-acc-dialect` instead: `esses-gen-fixtures` writes a `.ld`+`.ldx` pair in the sim-exporter dialect (124-byte channel headers, ACC channel names and units, BCN beacons for an out lap, ≥ 10 flying laps and an in lap, a one-hour 1 kHz session of 3.6 M samples per fast channel), licensed CC0-1.0 as our own output. The M1 criteria that open "an ACC `.ld`" (AC-M1-01, AC-M1-05, AC-M1-11) run on this synthetic file in public CI and additionally on the real file only when the private submodule is present (07 §4).

Commands: `esses-cli fixture add <file> --format <id> --oracle ref-decode|<tool>|<dir> --out <dir> [--remote <url>]` records where the expected channel values come from: `ref-decode` (`tools/ld_ref_decode.py`, written from the public layout notes independently of `esses_formats`), a pinned third-party decoder (i3rs, motec-i2), or a directory of oracle CSVs; expectations of synthetic families never come from the current reader, so a symmetric writer/reader error cannot pass (D-041); `fixture verify [dir]` re-probes, decodes and compares with per-dtype tolerance; `fixture diff <dir>` explains mismatches channel by channel. Synthetic families (including `synthetic-acc-dialect`) are generated at test time by `esses-gen-fixtures` (uses `IWriter`) into `fixtures/motec.ld/synthetic-*/`; only their `fixture.json` and expectations are committed, not the generated inputs (05 §9.1). Third-party sim exports (MotecLogGenerator output; game exports such as ACC) are in-tree only where their licence permits redistribution (D-040); sim-to-motec output is private only, because the project has no licence; until cleared they are private like the user's real files, which go to `fixtures/private` (git submodule to a private repository, U-04, answered 2026-10-10: nothing real is public for now), never redistributed; a file > 2 MB is referenced by `remote` + hash and cached by CI. The corpus moves to its own CC0 repository at M6 (D-033).

### 6.8 Fuzz

Targets in `tests/fuzz/`: `fuzz_ld_header` (header + channel list walk), `fuzz_ldx` (both M0), `fuzz_csv_sniff` and `fuzz_text_profile` (profile engine on a fixed profile; both from M2 with the text engine, D-044), `fuzz_maths_parser` (M2+), `fuzz_sidecar_json` (schema + merge). Each has a seed corpus derived from fixtures (`tools/fuzz_seeds.py`) and, where useful, a dictionary. `fuzz-smoke` runs 60 s per target on PRs; `fuzz.yml` runs 30 min nightly; crashes are minimised and attached; a fixed crash becomes a regression test in the target's `tests/`.

### 6.9 Benchmarks

`bench/` holds one Google Benchmark executable per area (`store_bench`, `m4_bench`, `maths_bench`, `formats_bench`, `report_bench`, `open_bench`, `plot_bench` with Qt). Each benchmark name maps to a budget row in `bench/workloads.md` (RW-1…RW-5 reproduced from performance-first §1.2). Gating follows D-042 and 07_ROADMAP.md §12: every budget row is an **absolute limit**, checked as the median of ≥ 5 repetitions on one reference machine (the developer's machine registered as a self-hosted runner, or a pinned dedicated runner) whose CPU, core count, RAM and OS are recorded in `bench/workloads.md`; `tools/bench_compare.py --limits` fails a row above its limit. The self-hosted job runs only on pushes to `main`, nightly, on tags and on manual dispatch, never on fork pull requests. GitHub-hosted runners run the same benchmarks as a non-blocking ±30 % regression alarm against `bench/baselines/hosted-<os>.json`, updated only at milestone tags (`--accept`). "Cold" means a fresh process with the OS file cache flushed.

### 6.10 Sanitizers and soak

`ESSES_SANITIZE=address,undefined` adds `-fsanitize=address,undefined -fno-omit-frame-pointer`; `thread` adds `-fsanitize=thread` and defines `ESSES_TSAN` so `TaskPool` tests use smaller workloads. The `soak` job (M4+) runs `tests/soak/saturday` (a parameterised version of the Saturday walkthrough looping over 10 files) under ASan with `detect_leaks=1`: 30 min nightly and 2 h before each tag. `tests/soak/lsan.supp` suppresses leak stacks from Qt, fontconfig and glib; a reported leak counts only if its stack contains an `esses_*` frame (AC-M4-03).

## 7. Packaging

### 7.1 Windows

1. `cmake --build --preset win-msvc-release --target install` into `build/stage/win64`.
2. `windeployqt --release --no-translations --no-system-d3d-compiler --no-compiler-runtime esses.exe esses-render.exe` (plus `--no-opengl-sw` until `GlBackend` exists); copy `qt6advanceddocking.dll` (`$<TARGET_FILE:qt6advanceddocking>`, §3.7.3). **VC++ runtime version.** Binaries built with VS 2022 17.10 or later (the developer's v143 toolset; also the toolset of the Qt 6.12 kits) use the `constexpr` `std::mutex` constructor and crash in `mutex::lock` when they load an older `msvcp140.dll` (< 14.40). "Run `vc_redist` when missing" is therefore not enough, because an old redist counts as present. The installer checks the version (step 3, `[Code]`), and the portable zip carries the runtime app-locally (step 4).
3. Inno Setup `tools/packaging/windows/esses.iss`:
   - `[Setup]`: `AppId={{<fixed GUID>}}`, `AppName={#AppName}`, `AppVersion={#Version}` (from `cmake/version.cmake`), `DefaultDirName={autopf}\Esses`, `ArchitecturesInstallIn64BitMode=x64compatible`, `ArchitecturesAllowed=x64compatible`, `LicenseFile=LICENSE`, `OutputBaseFilename=esses-{#Version}-win64-setup`, `WizardStyle=modern`, `ChangesAssociations=yes`.
   - `[Tasks]`: `assocld` "Associate .ld log files with Esses" (unchecked by default when an existing `.ld` association is detected in `[Code]`, so an i2 installation keeps its association unless the user opts in); `desktopicon`.
   - `[Registry]` (task `assocld`): `HKA\Software\Classes\.ld` → `Esses.LogFile`; `HKA\Software\Classes\Esses.LogFile\shell\open\command` → `"{app}\esses.exe" -l "%1"`; `.ldx` is not associated (companion only).
   - `[Files]`: the staged tree, `esses-cli.exe`, `NOTICES.md`, `THIRD_PARTY_LICENSES.md`, `LICENSE`.
   - `[Code]`: read `HKLM\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64` values `Major`, `Minor`, `Bld`; set a flag when the key is absent or the version is below that of the redist shipped with the build toolset (`VC\Redist\MSVC\<ver>`, at least 14.40).
   - `[Run]`: `vc_redist.x64.exe /install /quiet /norestart` when the `[Code]` flag is set (the bundled redist is the build toolset's own); optional launch.
4. Portable zip `esses-<ver>-win64-portable.zip`: the same staged tree plus an empty `portable.marker`, plus the C++ runtime deployed app-locally from the build toolset's `VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT\` (`msvcp140.dll`, `msvcp140_1.dll`, `msvcp140_2.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`; app-local deployment is permitted by the Visual Studio redistribution terms), so the zip starts on a machine with no or an old redist; `SettingsStore` detects the marker and stores settings, schemes, keymap and MRU under `<exe dir>/config/`.
5. Command line: `esses -w <project dir> -l <log> [-l <log>…]`; `esses-cli` ships in both.
6. Signing: `signtool` steps present but skipped unless `WINDOWS_CERT_*` secrets exist (U-11).

### 7.2 macOS

Best-effort at 0.1 (§4.1 `package`): built on tags, labelled "builds green, not supported" until U-09 decides macOS release status; a failure does not block the Windows release; the launch check is a smoke test on the `macos-14` runner (the hosted runner is Apple hardware, so no macOS VM is needed). `cmake --build --preset ci-mac-clang --target install` → `Esses.app`; `macdeployqt Esses.app -always-overwrite -libpath=<ADS dir>`; `Info.plist` with `CFBundleDocumentTypes` for `.ld`; `create-dmg` (or `hdiutil create -volname Esses -srcfolder stage -ov -format UDZO`) → `esses-<ver>-macos.dmg`; universal binary when both Qt arches are installed (6.12 `clang_64` is universal). Unsigned/unnotarised until U-11: the release note documents right-click → Open; notarisation steps (`codesign --deep --options runtime`, `notarytool submit`, `stapler`) are present and gated on `APPLE_*` secrets.

### 7.3 Linux

Best-effort at 0.1 like §7.2 (labelled "builds green, not supported" until U-09; the launch check is a smoke test of the AppImage under `QT_QPA_PLATFORM=offscreen` on the runner); the `esses-cli` tarball is release-blocking. Built on `ubuntu-22.04` for an older glibc; `linuxdeploy --appdir AppDir --plugin qt --output appimage` with `QMAKE` pointing at the aqtinstall Qt, `tools/packaging/linux/esses.desktop` (MIME `application/x-esses-log` for `.ld` via a shared-mime-info XML), icon, `AppRun` exporting `ESSES_DATA_DIR`; `esses-cli` inside the AppImage and as a standalone tarball `esses-cli-<ver>-linux-x86_64.tar.gz`. Flatpak later.

### 7.4 Common

`cmake/notices.cmake` + `tools/gen_notices.py` generate `THIRD_PARTY_LICENSES.md` at build time from the licence file of each vendored library under `third_party/` (listed in `third_party/README.md`, D-147), the ADS licence, the Qt notice block, and every `ported-from` entry in `CLEANROOM.md` (upstream project, URL, commit, licence text) for code ported from MIT/BSD readers (motec-i2, i3rs, motorsport-telemetry-rs, libxrk, TrackDataAnalysis, …; 00 §7.1, §10 below). `NOTICES.md` (hand-maintained header + generated section) ships in every package and is shown from Help → About → Licences. `reuse spdx` produces an SPDX SBOM attached to the release. `sha256sums.txt` covers every artefact. The Qt-free CLI is a standalone download per OS.

## 8. Versioning and release process

- Semantic versioning. Pre-1.0: `0.MINOR.PATCH`; 0.1 = MVP; 0.x point releases for fixes. v1.0.0 after M5, v1.1.0 after M7. Milestone pre-release tags `v0.1.0-mN` (07_ROADMAP.md §1.2).
- Independent version streams: JSON schema ids (`sidecar-1`, bumped only on incompatible change, with a `Migrations` entry); the C ABI (`sdk/CHANGELOG.md`, `_v1` tables frozen at v1.0, D-028); the fixture corpus (`fixtures/manifest.json` `corpusVersion`).
- `CHANGELOG.md` in keep-a-changelog form; every PR adds an entry under Unreleased with its AC id when applicable.
- Release checklist (`tools/release.py --dry-run` prints it): (1) `main` green including `bench`; (2) `CHANGELOG.md` section cut; (3) `NOTICES.md` Qt version and source URL updated; (4) `reuse lint` clean and every `ported-from` entry present in `THIRD_PARTY_LICENSES.md`; (5) `tools/check_text_overlap.py` clean against the local reference corpus (§5); (6) tag `vX.Y.Z` signed; (7) `release.yml` builds and drafts the GitHub Release with artefacts, SBOM and checksums; (8) manual Windows VM launch test (AC-M4-04), including a VM with an old VC++ redist (< 14.40) installed for both the installer and the portable zip; (9) publish; (10) bump `project(VERSION)` for the next cycle.
- Qt bump task: a yearly scheduled issue (and a `schedule:` workflow that opens it) to move CI to the newest open-source Qt minor and raise `find_package(Qt6 6.x)` only when a used API requires it (D-006).
- Hotfix: branch from the tag, cherry-pick, tag `vX.Y.(Z+1)`.

## 9. LGPL compliance checklist (Qt LGPLv3, ADS LGPL-2.1, FFmpeg LGPL-2.1 later)

1. Dynamic linking only; no static Qt, no modified Qt; `windeployqt`/`macdeployqt`/`linuxdeploy` bundle unmodified official binaries (D-006).
2. `NOTICES.md` and the About dialog state: "This application uses Qt <version> under the GNU LGPL v3", include the full LGPLv3 and GPLv3 texts (LGPLv3 incorporates GPLv3), and the LGPL-2.1 text for ADS.
3. Source offer: `NOTICES.md` and the About dialog give the exact URL of the Qt source archive matching the shipped version (`https://download.qt.io/official_releases/qt/<major.minor>/<version>/single/qt-everywhere-src-<version>.tar.xz`) with its sha256, and the ADS tag URL; the project keeps a copy of both archives as a release asset or on project storage so the offer does not depend on third-party hosting (written offer valid for at least three years after each release).
4. Re-linking: users can replace the Qt DLLs/frameworks/so files in the install directory with their own builds; `docs/user/relinking.md` describes how (the anti-tivoisation requirement is satisfied because the application loads whatever compatible Qt is present).
5. Qt's own third-party attributions (`qtbase/LICENSES`, FFmpeg attribution page when Multimedia is added) are included in `THIRD_PARTY_LICENSES.md`.
6. The installer's licence page shows the project licence (MIT), not Qt's; Qt's notice is in the package and the About dialog.
7. No GPL-only Qt module and no GPL third-party library is ever linked (CI check: `lint` greps the link lines of `compile_commands.json`/link.txt for `Qt6Charts`, `Qt6Graphs`, `Qt6DataVisualization`, `Qt6Quick3D`, `fftw3`, `kddockwidgets`, `qcustomplot`, `vector_blf`).
8. Plugins (v1.0+) that privately link QtCore (harvested converter parsers) carry the same notice in their own `plugin.json` and `NOTICES`.

## 10. REUSE and SPDX

`REUSE.toml`:

```toml
version = 1
SPDX-PackageName = "esses"
SPDX-PackageSupplier = "Esses contributors"

[[annotations]]
path = ["data/**", "schemas/**", "docs/**", "tests/golden/**", "tests/maths/**", "bench/workloads.md"]
SPDX-FileCopyrightText = "Esses contributors"
SPDX-License-Identifier = "MIT"

[[annotations]]
path = ["fixtures/motec.ld/synthetic-*/**"]
SPDX-FileCopyrightText = "Esses contributors"
SPDX-License-Identifier = "CC0-1.0"

[[annotations]]
path = ["third_party/pocketfft/**"]
SPDX-FileCopyrightText = "Max-Planck-Society"
SPDX-License-Identifier = "BSD-3-Clause"

[[annotations]]
path = ["third_party/iir1/**"]
SPDX-FileCopyrightText = "Bernd Porr"
SPDX-License-Identifier = "MIT"

[[annotations]]
path = ["third_party/json-schema-validator/**"]
SPDX-FileCopyrightText = "Patrick Boettcher"
SPDX-License-Identifier = "MIT"

[[annotations]]
path = ["tests/golden/fonts/**"]          # Noto Sans for goldens only; takes precedence over the tests/golden/** MIT entry
precedence = "override"
SPDX-FileCopyrightText = "see tests/golden/fonts/OFL.txt"
SPDX-License-Identifier = "OFL-1.1"
```

Source header (every `.cpp/.hpp/.h/.cmake/.py/.yml`):

```cpp
// SPDX-FileCopyrightText: 2026 Esses contributors
// SPDX-License-Identifier: MIT
```

**Ported code.** A file ported from an MIT/BSD project keeps the upstream copyright line next to ours (MIT requires the notice to be retained), and `REUSE.toml` gets no blanket override for it:

```cpp
// SPDX-FileCopyrightText: 2026 Esses contributors
// SPDX-FileCopyrightText: 2023 <upstream author>        (ported from <project> <commit>, see CLEANROOM.md)
// SPDX-License-Identifier: MIT
```

The same file has a `ported-from` row in `CLEANROOM.md` (§11), which `gen_notices.py` turns into a `THIRD_PARTY_LICENSES.md` entry (§7.4). A `reuse lint` test fixture (`tests/reuse/ported_example.cpp`) checks that a ported file with two copyright lines passes and that the notice generator picks it up.

Every public fixture declares its licence in `fixture.json` and in a per-directory annotation; `reuse lint` runs in `lint`; `reuse spdx` emits the SBOM in `package`. `LICENSES/` holds the text of every SPDX id referenced (MIT, CC0-1.0, BSD-3-Clause, BSD-2-Clause, MPL-2.0, BSL-1.0, Apache-2.0, LGPL-2.1-or-later, LGPL-3.0-only, GPL-3.0-only, OFL-1.1, Zlib).

## 11. Governance file set (D-002, D-004)

| File | Required sections / sentences |
|---|---|
| `LICENSE` | MIT text; copyright "Esses contributors" |
| `DCO` | Developer Certificate of Origin 1.1 verbatim |
| `CONTRIBUTING.md` | How to build (link §12); branch and PR flow; `Signed-off-by` required on every commit (DCO, no CLA); coding standards (clang-format/tidy, layering, Qt-free rule, no exceptions across library boundaries); tests required per layer (§6); **provenance rule**: any code or behaviour derived from a public project or an i2 document must name it in `CLEANROOM.md`; **ported-code rule**: a file ported from an MIT/BSD project keeps the upstream `SPDX-FileCopyrightText` line beside ours and gets a `ported-from` row in `CLEANROOM.md` (§10); GPL sources are read for understanding only and never ported; **AI-provenance rule**: never paste i2 help text, i2 shipped files, vendor XML or GPL source into a prompt that generates committed code, documentation, keymap `doc` strings or tooltips; describe behaviour in your own words first; run `tools/check_text_overlap.py` before a PR that adds user-facing text; **repository hygiene**: `research/`, `help_text/`, `conv_samples/` and vendor files never enter the repository (§1, enforced by `lint`); adding a format = reader or profile + dictionary + one fixture with documented provenance + `CLEANROOM.md` entry + docs page; "which acceptance criterion does this serve?" in the PR description; licence of contributions = MIT |
| `.github/PULL_REQUEST_TEMPLATE.md` | Checkboxes: DCO sign-off; no MoTeC code/assets/strings; provenance recorded (or N/A); tests added; schema/ABI change flagged; `CHANGELOG.md` entry |
| `GOVERNANCE.md` | Maintainer (the user) and decision process (ADRs in `docs/adr/`); "the official distribution contains only MIT-licensed code"; **no relicensing without every contributor's consent**; no CLA; how `OWNERS` files delegate a format directory; conflict resolution; project name ownership |
| `CLEANROOM.md` | Rules: no disassembly or decompilation of MoTeC binaries or files; i2 behaviour only from the help pages as displayed by the shipped help viewer, shipped XML/config files, SDK samples and public sources; no MoTeC code, art, icons, help text, sample files or trademarks; the AI-provenance rule of `CONTRIBUTING.md`; the clean-room working material (`research/`, `help_text/`, vendor files) is kept outside the repository. Then one entry per format and per i2-derived behaviour with the columns Area, Behaviour, **Category** (one of: help page; public release notes; vendor-shipped template; file written by the user's install, with the D-005 (2) consent date; observed on a licensed install, D-005 (3)), Source (the page, file or public project + licence), Date. D-145 adds the categories user-contributed own work and user-owned device logs, and recognises public project, public standard, own design and rule, which rows already use. A second table lists **ported-from** entries: file, upstream project, URL, commit, licence, upstream copyright line; it gets rows for the `.ld` reader and writer only if the user confirms the converter's licence and lineage and its code is then reused verbatim (D-142, D-144), naming the converter files and the earlier Python code they mirror; until then the reader and writer are new code and the converter appears in the main table as user-contributed own work. A third records third-party file licences reviewed for fixtures (for example a game's terms, should a game-generated file such as an ACC export ever be proposed for the public tree, §6.7). Required rows from M0: the `.ld` reader and writer written from the converter's port specification (D-142, D-144), the layout facts spike S-1 confirmed on user-owned device logs, the exclusion of the pro-logging field (D-143), and the checks against public documentation (i3rs docs, motec-i2, motorsport-telemetry-rs) and the independent `tools/ld_ref_decode.py` (D-041), `.ldx` structure (user files; i3rs/motorsport-telemetry-rs/motec-i2, MIT; sim-to-motec and SARUSySLab consulted, unlicensed, nothing copied), MoTeC CSV dialect, units table (own table from SI Brochure / NIST SP 811 and standard automotive units; i2 `units.xml` consulted only for alias spellings), beacon semantics, maths precedence choice, shift-vs-mul decision, the i2 field-of-use rule (D-038) with the user's 2026-10-10 answer that ACC files are not MoTeC-licensed (so i2 never runs on them), the user's 2026-10-10 answers to the three D-005 questions (Q-03), local use of the i2 sample files for testing, never committed (Q-06, D-117), and the research-phase use of user-written files (D-039) |
| `TRADEMARK.md` | Esses name and logo policy; "MoTeC" and "i2" are trademarks of their owner and are used only nominatively to describe file formats; the project is not affiliated with or endorsed by MoTeC; rename policy |
| `SECURITY.md` | Reporting address; supported versions; plugins and C++ are trusted code, Lua is sandboxed (D-030) |
| `NOTICES.md` | §9 content |
| `CLAUDE.md` / `AGENTS.md` | Written in M1 (D-151). Layer map, naming rules (including the neutral-name rule for i2-format code, 00 §8), Qt-free rule, where tests live, how to run presets, the AI-provenance rule verbatim from `CONTRIBUTING.md` ("never paste i2 help text, i2 shipped files, vendor XML or GPL source into a prompt that generates committed code or text"), the ported-code rule, links to ADRs (D-004 bus-factor mitigation) |
| `.gitignore` | `build/`, `CMakeUserPresets.json`, `.vcpkg-bc/`, and the clean-room directories `research/`, `help_text/`, `conv_samples/` (defence in depth; the `lint` forbidden-path check is the gate) |
| `OWNERS` (per format dir, M6) | Maintainers for that directory; reviewers required for PRs touching it |

## 12. First-week bootstrap sequence (the developer's machine)

Facts: Windows 11, Visual Studio 2022 with the v143 toolset, CMake 4.4 on PATH, Qt 6.7.2 `msvc2019_64` at `C:\Qt\6.7.2\msvc2019_64`, Python 3.12, Git; Ninja not on PATH (Visual Studio and Qt Tools each ship one; `tools/dev/build-core.cmd` finds them), no vcpkg (none is needed, D-147). Commands are PowerShell 5.1 (no `&&`; run lines in order). Paths below assume `C:\dev`.

**As run in M0 (2026-10-10 to 2026-10-11).** Phase A had no downloads: it built with the Visual Studio 2022 toolchain (MSVC 14.44 through `vcvars64.bat`, the Ninja bundled with Visual Studio, CMake 4.4; `tools/dev/build-core.cmd` wraps this) and vendored pugixml 1.13 and xxHash 0.8.3 from local copies under `third_party/`, with a local copy of Catch2 2.13.10 for the first tests. Later on 2026-10-10 the user approved the downloads, and phase B vendored nlohmann/json 3.11.3, json-schema-validator 2.3.0, CLI11 2.4.2, fast_float 6.1.6 and Catch2 3.7.1, which replaced Catch2 2.13.10 (D-147, D-148; `third_party/README.md` records the routes and SHA-256 sums). There is no vcpkg, so the Day 1 vcpkg lines are gone and the skeleton has no `vcpkg.json`. clang-format and clang-tidy 19.1.5 came from Visual Studio's LLVM, and `reuse` is not installed locally, so `reuse lint` is a CI-only check (`docs/milestones/M0.md` §2.6, §4). The `.ld` reader and writer were written fresh from the converter's port specification (D-144). Nothing is committed or pushed yet: the Day 1 commit and `gh repo create` and the Day 2 CI run are still ahead, and the Day 2 `qt-app` job is M1 (D-150).

**Day 1 — tools and the empty repo**

```powershell
# Tools
winget install --id Ninja-build.Ninja -e
winget install --id GitHub.cli -e
winget install --id LLVM.LLVM -e                      # clang-format / clang-tidy on PATH after re-login
py -3.12 -m pip install --user pipx ; py -3.12 -m pipx ensurepath
pipx install reuse ; pipx install aqtinstall          # aqtinstall only needed for an optional local 6.12
# no vcpkg: every library dependency is vendored under third_party/ (§3.7.2, D-147)
# Visual Studio developer environment (needed for Ninja + MSVC in this shell)
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -DevCmdArguments "-arch=x64 -host_arch=x64" -SkipAutomaticLocation
cmake --version ; ninja --version ; cl.exe 2>&1 | Select-Object -First 1
# Repository skeleton
New-Item -ItemType Directory -Force C:\dev\esses ; Set-Location C:\dev\esses
git init -b main
# create: CMakeLists.txt, CMakePresets.json, cmake/*.cmake, third_party/ (§3.7.2), LICENSE, DCO, NOTICES.md, GOVERNANCE.md,
#         CONTRIBUTING.md, CLEANROOM.md, TRADEMARK.md, SECURITY.md, REUSE.toml, .clang-format, .clang-tidy,
#         .editorconfig, .gitattributes (* text=auto eol=lf ; *.ld binary ; *.png binary), .gitignore (build/, CMakeUserPresets.json)
#         libs/base/{CMakeLists.txt,include/esses/base/version.hpp,src/version.cpp,tests/version_tests.cpp}
#         apps/cli/{CMakeLists.txt,main.cpp}   (prints --version)
# CMakeUserPresets.json of §3.6 (configure + build + test presets). WriteAllText writes UTF-8 without a BOM;
# PowerShell 5.1 "Set-Content -Encoding utf8" would prepend one.
[IO.File]::WriteAllText("$PWD\CMakeUserPresets.json", '{ "version": 8, "configurePresets": [ { "name": "dev", "inherits": "win-msvc-debug", "cacheVariables": { "CMAKE_PREFIX_PATH": "C:/Qt/6.7.2/msvc2019_64" }, "environment": { "PATH": "C:/Qt/6.7.2/msvc2019_64/bin;$penv{PATH}" } }, { "name": "dev-core", "inherits": "win-msvc-core" } ], "buildPresets": [ { "name": "dev", "configurePreset": "dev" }, { "name": "dev-core", "configurePreset": "dev-core" } ], "testPresets": [ { "name": "dev", "configurePreset": "dev", "output": { "outputOnFailure": true }, "environment": { "QT_QPA_PLATFORM": "offscreen" } }, { "name": "dev-core", "configurePreset": "dev-core", "output": { "outputOnFailure": true } } ] }')
cmake --preset dev-core ; cmake --build --preset dev-core ; ctest --preset dev-core
.\build\dev-core\bin\esses-cli.exe --version
reuse lint
git add -A ; git commit -s -m "Bootstrap repository skeleton"
gh auth login ; gh repo create esses --private --source . --push
```

**Day 2 — CI green on three OSes, Qt path proven**

```powershell
# .github/workflows/ci.yml with core (3 OSes), lint, sanitize, fuzz-smoke; qt-app building an empty esses_app arrives in M1 (D-150)
git add .github ; git commit -s -m "CI: core/lint/sanitize/fuzz-smoke" ; git push
gh run watch
# Local Qt build (6.7.2 kit); the preset environment puts the Qt bin directory on PATH for build and tests (§3.7.5)
cmake --preset dev ; cmake --build --preset dev ; ctest --preset dev
.\build\dev\bin\esses-cli.exe --version
$env:PATH = "C:\Qt\6.7.2\msvc2019_64\bin;$env:PATH" ; .\build\dev\bin\esses.exe --version   # GUI exe: prints via AttachConsole (§3.10)
cmake --build --preset dev --target deploy-dev        # once, so build\dev\bin\esses.exe also starts from Explorer
```

**Day 3 — `esses_base`, `esses_units`, `esses_store` with property tests** (07_ROADMAP.md M0 week 2 starts here; the first week is tools + spikes). Run `ctest --preset dev-core -L property` and `cmake --preset dev-core -DESSES_ENABLE_CLANG_TIDY=ON` locally before pushing.

**Day 4 — spike S-1 (`.ld` reader on the user's real files)**

```powershell
git switch -c spike/s1-ld-reader
# prototype under tools/spikes/s1/ ; run on every real file the user supplies
.\build\dev-core\bin\ld_probe.exe "D:\Logs\bathurst_r1.ld" --json > s1-bathurst.json
.\build\dev-core\bin\ld_probe.exe "D:\Logs\bathurst_r1.ld" --channel "Ground Speed" --csv > s1-gs.csv
# compare with a public-project oracle (motec-i2 / i3rs CLI output recorded once) and, for a file from a MoTeC device, i2's CSV export (U-03 yes, 2026-10-10)
# write docs/spikes/S-1.md and the CLEANROOM.md rows; do not merge the prototype
```

As run on 2026-10-10 (`docs/spikes/S-1.md` §10 item 6): M0 had no C++ reader yet and git history was not to change in that phase, so the prototype is Python (`tools/ld_ref_decode.py`, `tools/spikes/s1/`), untracked in the working tree, with no `ld_probe.exe` and no spike branch.

**Day 5 — spikes S-2, S-3, S-4 and the lint tooling**

```powershell
git switch -c spike/s2-m4-bench   # Catch2 bench, 20 columns × 3.6 M samples; record ms per zoom in docs/spikes/S-2.md
git switch -c spike/s3-ads        # FetchContent ADS, 6-dock test app, Qt Test offscreen; push so CI runs it on 3 OSes
git switch -c spike/s4-raster     # throwaway QWidget: 20 polylines × 15,200 pts, Static/Dynamic timings at DPR 1 and 2
# tools: check_vendor_marks.py, check_layering.py, check_keymap_coverage.py, qt-free-check negative test → lint job
```

Recommended in week 1 (U-08, §3.7.4; about an hour): `aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 -O C:\Qt` and a second user preset `dev-612` with `CMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64` and the matching `PATH` entry, plus its build and test presets. Golden PNGs are still never regenerated locally (§6.4).

## 13. Discrepancies and proposed decisions

1. **ADS via FetchContent, not vcpkg.** D-026 allows either; the vcpkg `qtadvanceddocking` port depends on vcpkg's `qtbase`, which would build Qt through vcpkg against D-006/tech-stack advice. Proposed: FetchContent with a pinned tag, shared library.
2. **Extra top-level directories** `schemas/`, `bench/`, `third_party/` beyond the task's list; `schemas/` and `bench/` are named by D-019/outline §8, `third_party/` is implied by D-034's vendored libraries.
3. **Catch2 and Google Benchmark as a vcpkg `tests` feature** so release builds without tests do not fetch them; D-034 lists them in the manifest without a feature. *Superseded by D-147 and D-148: both are vendored under `third_party/` (Catch2 3.7.1 in M0, Google Benchmark when `bench/` needs it).*
4. **`$schema` placeholder** `urn:esses:<schema>-<n>` until the project domain exists (D-003 says ids live under a project-owned URL once the domain exists); `SchemaRegistry` accepts both forms.
5. **`DrawList` JSON goldens** in the `core` job as the strict companion to PNG goldens (not in D-033).
6. **`.ld` association installer task** defaults to unchecked when an existing `.ld` association is detected, so an i2 installation is not hijacked; D-034 says only ".ld association".
7. **Test font**: Noto Sans (OFL-1.1), the shipped schemes' font family, under `tests/golden/fonts/` for goldens only, as D-025's "scheme's default font files for tests" implies; this replaces D-118's DejaVu Sans (proposed as D-127). The 0.1 installer does not bundle Noto Sans; users without it get Qt family substitution (06 §16 rule 6; `OPEN_QUESTIONS.md` F-14, applied).
8. **`-Werror` only in CI presets** (D-034 says "in CI"); local builds warn.
9. **Bench gate**: one reference machine as a self-hosted runner with absolute limits; hosted runners are a non-blocking ±30 % alarm (D-042, §4.1, §6.9).
10. **`fixtures/private` checkout gated on a deploy-key secret**, with `fixture verify` reporting skipped private fixtures, so fork PRs stay green (D-005 implementation detail).
11. **`ubuntu-22.04` for the AppImage build** (older glibc) while tests run on `ubuntu-24.04`; D-034 names only 24.04.
12. **Vendor-mark allow-list.** The v1.0 i2 configuration-file importers live in `esses_project` (`libs/project/src/cfgimport/`, neutral `Cfg*Importer` classes), as 01 §2.1, 03 §12 and 06 §16 rule 9 place them, so the former source-directory class `libs/formats/src/i2config/` is gone. The allow-list classes are those of 00 §8 rule 2: importer ids, dialog labels, the MoTeC CSV writer's format-name constant ([WRITER_UNRESOLVED #70](background/design-panel/WRITER_UNRESOLVED.md)), the D-019 `x-i2` key and its `xI2` member, and prose files. D-003 names only importer ids and dialog labels; the other three classes are proposed in DECISIONS.md Addendum A as D-125, together with the option of renaming `x-i2` to a neutral key (`x-import` + `format`); the importer location is D-122, which supersedes D-103's `libs/formats/src/cfgimport/`. Identifier tokens are now linted as well as words (§5).
13. **Iir1 and json-schema-validator are compiled libraries** (D-124), not header-only as D-034 states; `cmake/third_party.cmake` builds them as static libraries from the vendored sources without their upstream CMake files (§3.7.2).
14. **`x64-windows-static-md` triplet and one `bin/` output directory** so that executables and tests start on Windows without vcpkg DLLs; Qt is found via `PATH` (presets, `ENVIRONMENT_MODIFICATION`) rather than copied (§3.6, §3.7.5). D-034 names no triplet. *The triplet is superseded by D-147 (no vcpkg); the single `bin/` directory stands.*
15. **vcpkg `files` binary cache with `actions/cache` instead of `x-gha`**, and `/Z7` (`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`) in CI presets so sccache works on MSVC (§4.3). *The vcpkg cache is superseded by D-147; `/Z7` stands.*
16. **Pixel goldens authoritative on ubuntu only**, with render smoke tests and `DrawList` JSON goldens on Windows and macOS, regeneration only from CI artefacts, and `time_distance` capped at 12 named cases (§6.4). D-033 does not name an OS; 07 AC-M1-04, R-17 and 04 §6.7 item 3 follow (D-127).
17. **Required checks without macOS `qt-app`**; macOS `qt-app` runs nightly. DMG and AppImage are still built by `package` (D-034) but are best-effort at 0.1 with runner smoke tests instead of VM checks; the Windows installer, the portable zip and the CLI tarballs are release-blocking (§4.1, §4.4, §7.2, §7.3). 07 AC-M4-01 (Windows and Linux required, macOS nightly) and AC-M4-04 (macOS runner smoke test instead of a macOS 14 VM, plus a Windows VM with an old VC++ redistributable) follow (D-124).
18. **Qt 6.12 locally in M0 week 1** as the new default of U-08 (§3.7.4, D-126); D-006 leaves the local version to the developer.
19. **Real ACC files are private for now** (ordinary third-party fixtures under Q-04; ACC is not MoTeC-licensed, answered 2026-10-10); public CI uses `synthetic-acc-dialect` from our own writer (§6.7). 07 AC-M1-01/05/11 and U-04 follow (D-127).
20. **Clean-room controls against AI-assisted copying**: forbidden paths in the repository, `.gitignore` entries, `tools/check_text_overlap.py` (local/private, release checklist) and the AI-provenance rule in `CONTRIBUTING.md`, `CLEANROOM.md` and `CLAUDE.md` (§1, §5, §8, §11). Not in D-003/D-004; proposed as D-128.
21. **Ported-code attribution**: upstream copyright lines kept in ported files, `ported-from` rows in `CLEANROOM.md`, generated into `THIRD_PARTY_LICENSES.md` (§7.4, §10, §11).
22. **VC++ runtime version check** in the installer and an app-local CRT in the portable zip (§7.1), because a VS 2022 17.10+ build crashes against `msvcp140.dll` older than 14.40.
23. **`maths-case-1` is owned by 03 §14.1** and lives in `tests/maths/*.case.json` (D-033's path); this document no longer defines a second shape (§6.6).
24. **The layer check lives in `esses_add_library()`** (configure time) plus `tools/check_layering.py`, replacing the `esses_link()`/`cmake/layers.cmake` mechanism 01 §2.3 used to describe; 01 has been updated to match.
25. **M0 as built** (`docs/milestones/M0.md` §5; `DECISIONS.md` A.11): dependencies vendored under `third_party/` instead of a vcpkg manifest (D-147; §1, §3.6, §3.7, §4, §12), Catch2 3.7.1 amalgamated with the project's own test discovery (D-148; §6.2), the `ESSES_WITH_<LIB>` switches (D-149; §3.2), the `qt-app` job from M1 (D-150; §4) and `CLAUDE.md`/`AGENTS.md` from M1 (D-151; §1, §11).

**Cross-references.** 07_ROADMAP.md (milestone placement, acceptance ids, spikes, user-confirm items U-01…U-16); the sibling design documents for formats/fixtures (D-028, D-029, D-033), persistence and schemas (D-015, D-019), the plot engine and `OffscreenRenderer` (D-025), the maths engine and `docs/maths-compat.md` (D-020..D-024), and the UI shell/keymap (D-027, D-030); `ARCHITECTURE_OUTLINE.md` §1 (targets), §5 (schemas), §8 (budgets).
