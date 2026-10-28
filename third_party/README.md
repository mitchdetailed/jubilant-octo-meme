# Vendored third-party code

Upstream files, unchanged, with each library's own licence file. Licences and copyright lines: `NOTICES.md`
and `REUSE.toml` at the repository root; licence texts: `LICENSES/`. Updating a library is a change that
touches only its directory, this file, `NOTICES.md` and `REUSE.toml` (08_REPO_AND_BUILD.md section 3.7.2).

## Provenance

Two routes:

- **Local copies (no download).** Copied from sources already on the developer's machine during M0 phase A
  (2026-10-10).
- **Downloads approved by the user.** Downloaded with the user's explicit approval, from the official GitHub
  repositories at the tags below: the M0 phase B libraries on 2026-10-10, the Qt Advanced Docking System on
  2026-10-11 (D-153).

| Library | Version | Licence (file) | Route | Source of this copy | Files | Upstream |
|---|---|---|---|---|---|---|
| pugixml | 1.13 | MIT (`pugixml/LICENSE.md`, also at the end of `pugixml.hpp`) | local copy | Qt 6.7.2 sources, qt3d assimp contrib | `pugixml/src/pugixml.{hpp,cpp}`, `pugiconfig.hpp` | https://github.com/zeux/pugixml |
| xxHash | 0.8.3 (header-only) | BSD-2-Clause (header of `xxhash.h`) | local copy | pyarrow vendored copy | `xxhash/xxhash.h` | https://github.com/Cyan4973/xxHash |
| nlohmann/json | 3.11.3 | MIT (`nlohmann_json/LICENSE.MIT`) | download | https://github.com/nlohmann/json/tree/v3.11.3/single_include | `nlohmann_json/include/nlohmann/json.hpp`, `json_fwd.hpp` | https://github.com/nlohmann/json |
| json-schema-validator | 2.3.0 | MIT (`json-schema-validator/LICENSE`) | download | https://github.com/pboettch/json-schema-validator/archive/refs/tags/2.3.0.tar.gz | `json-schema-validator/src/*` | https://github.com/pboettch/json-schema-validator |
| CLI11 | 2.4.2 | BSD-3-Clause (`cli11/LICENSE`) | download | https://github.com/CLIUtils/CLI11/releases/tag/v2.4.2 | `cli11/include/CLI/CLI11.hpp` | https://github.com/CLIUtils/CLI11 |
| Catch2 | 3.7.1 (amalgamated) | BSL-1.0 (`catch2v3/LICENSE.txt`) | download | https://github.com/catchorg/Catch2/releases/tag/v3.7.1 | `catch2v3/src/catch_amalgamated.{hpp,cpp}` | https://github.com/catchorg/Catch2 |
| fast_float | 6.1.6 | MIT OR Apache-2.0 OR BSL-1.0 (`fast_float/LICENSE-MIT`; all three in the header of `fast_float.h`) | download | https://github.com/fastfloat/fast_float/releases/tag/v6.1.6 | `fast_float/include/fast_float/fast_float.h` | https://github.com/fastfloat/fast_float |
| Qt Advanced Docking System (ADS) | 5.1.1 (tag v5.1.1, released 2026-08-20) | LGPL-2.1-or-later (`qtads/LICENSE`, also `qtads/gnu-lgpl-v2.1.md`) | download (2026-10-11) | https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/archive/refs/tags/v5.1.1.tar.gz, archive SHA-256 `9ce947acbb1984946f9145c976e42e6984f99b98fb414dd7354a290af6d4ff04` | `qtads/src/` (sources, `images/`, `stylesheets/`, `linux/`), `qtads/cmake/`, `qtads/CMakeLists.txt`, `qtads/adsConfig.cmake`, licence files, `README.md` (104 files); demo, docs, examples, Python bindings dropped; two source files changed by Esses (`src/AutoHideSideBar.cpp`, `src/DockManager.cpp`, 2026-10-25); `qtads/UPSTREAM.md` is our note (provenance and those changes) | https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System |

Retired: Catch2 2.13.10 (single header, local copy from the Qt 6.7.2 qttools sources) was used in phase A and
removed when the tests moved to Catch2 v3 (D-033). `LICENSES/Apache-2.0.txt` (for fast_float's licence
expression) is the SPDX licence-list text, copied from the local Qt 6.7.2 sources (`qtbase/LICENSES/`).

SHA-256 of the vendored files as committed (ADS, with its 104 files, by a tree digest after this list):

```
9dd1610e73dd94c4ad20696011cd4f2d343566cc0d5a8fa7c4e6c0e36d20fcae  pugixml/src/pugixml.cpp
a722f38db893b83d4d3cda15f79979d7c82569b87733162d2b1ee78d23284107  pugixml/src/pugixml.hpp
18947b268c29b39ce49e6898fda61cf0de00dbb20c9fbc7a725db20a7f49e1fb  pugixml/src/pugiconfig.hpp
c340dda16bab2a2d3f7b737a33b2f7e712e8f592b7ca306ff441bc33d16c1cf3  xxhash/xxhash.h
9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6  nlohmann_json/include/nlohmann/json.hpp
5cefbba751baf5243033fd894c70be9b37103d64b3d3a959e4173197d71137b9  nlohmann_json/include/nlohmann/json_fwd.hpp
3eda0face337a52ff6ca27b3b1b53725f775e350cc47aa5dbffb14420aea0878  json-schema-validator/src/json-patch.cpp
a6be0afd659b0f1e66381430c1c321cf1c24cbc84fad2c38e56be76ad49aac9a  json-schema-validator/src/json-patch.hpp
9486585610d3f4a335b08cde0b5ea8aec27c979271e2581bc5ab6f3c86b4c05c  json-schema-validator/src/json-schema-draft7.json.cpp
97261225bb67e95b333e44d1e6e8e0ee13cac1aad2dd2f5c6c1a8fc5465c907c  json-schema-validator/src/json-uri.cpp
45bb6430ad26a0c1c63016f5d2ba881227f728a4de52d9adcffd239bf284bf7a  json-schema-validator/src/json-validator.cpp
2d9772aee4cb7ddb6d338eac207c965fe2cd283a045dc35dfa9c0aaec481247b  json-schema-validator/src/nlohmann/json-schema.hpp
e25a32424dc8050f36f1db721f0bd32d9804c186222c2dc089d96c2aabf5dd52  json-schema-validator/src/smtp-address-validator.cpp
e7d1351661378e4d07c2deaf92fa9547f6adfb414bd844aa17095516f487c23b  json-schema-validator/src/smtp-address-validator.hpp
f6fafb7289a8499e8d7465825dee36f9791ffe994dba156191d0669bd604afe8  json-schema-validator/src/string-format-check.cpp
3935283e4c9db7089ed304476dbab2805c9311c41b125a55e92da4659df0d660  cli11/include/CLI/CLI11.hpp
91d47e2ddfb2949df22ff582d6c292554184da743e11d3811dcdd06b5b0a159b  catch2v3/src/catch_amalgamated.hpp
450fc5f213a87668f0f48c91a2afa4f4226186fef453d36e6fa3bc184ccdb9ba  catch2v3/src/catch_amalgamated.cpp
734de683d1ba414c8190b48103a10991886cac421aab07e7e910c48000df9e59  fast_float/include/fast_float/fast_float.h
```

ADS tree digest: the SHA-256 of the lines `<sha256 of the file>  <path under qtads/>` of every file of
`qtads/` except `UPSTREAM.md`, sorted by path, each ending in `\n`:
`a72b087a06be613f7938595eff078e4ca32e14383d5ee6532aa9039eb5705dc9` (104 files). On 2026-10-11 the downloaded
archive matched the SHA-256 in the table, and each of the 104 files was byte-identical to its copy in the
archive (tree digest then `5f5cca0a4916cc7e9b764fa43099319440bcc305b36610855674c57fe7a3bd19`). On 2026-10-25
two of them were changed to fix undefined behaviour that the soak's UBSan reported, as `qtads/UPSTREAM.md`
describes; each change is marked and dated in the file (LGPL-2.1 section 2(b)). Their SHA-256 now, and in the
archive:

```
07bea452910a31bcbfcfe8b43f499ed7a4d9b91cce350b10d3e61994f3da89c9  qtads/src/AutoHideSideBar.cpp
13ca11444b818b6aaedcb0828a1e907149d43b595e54f85bcbfd8223d702b784  (archive)
c266fd5d60e1534648b96844ebe70e35919bc887245385ca6d653119118b3585  qtads/src/DockManager.cpp
ff8ad253023595fd689abef2dc5ce09341bc9f90c2fca42a10361c24db7b49cc  (archive)
```

Recompute the tree digest with

```
python -I -c "import hashlib,pathlib;r=pathlib.Path('third_party/qtads');fs=sorted(p.relative_to(r).as_posix() for p in r.rglob('*') if p.is_file() and p.name!='UPSTREAM.md');h=hashlib.sha256();[h.update((hashlib.sha256((r/f).read_bytes()).hexdigest()+'  '+f+'\n').encode()) for f in fs];print(len(fs),h.hexdigest())"
```

## How the build uses these

`cmake/third_party.cmake` builds every library itself, never through the upstream CMake files, and adds its
include directories as SYSTEM, so the project's warning flags and warnings-as-errors never apply to them;
compiled third-party code is built without warnings. ADS is the exception (D-153): `cmake/qtads.cmake` builds it
with Qt (`ESSES_BUILD_QT=ON`, `ESSES_DOCK_BACKEND=ads`) through its own unmodified CMake project, as described
below.

| Target | Kind | Notes |
|---|---|---|
| `esses::pugixml` | static library | `.ldx` parsing |
| `esses::xxhash` | header-only | `esses_base` compiles it in one private translation unit with `XXH_INLINE_ALL` |
| `esses::nlohmann_json` | header-only | also spelled `nlohmann_json::nlohmann_json`; upstream defaults (implicit conversions on) |
| `esses::json_schema_validator` | static library | the six sources of `json-schema-validator/src`; links `esses::nlohmann_json` |
| `esses::cli11` | header-only | `#include <CLI/CLI11.hpp>` |
| `esses::fast_float` | header-only | `#include <fast_float/fast_float.h>` |
| `esses::catch2` | static library, tests only | Catch2 v3 amalgamated with its default `main()`; linked by `esses_add_test()` |
| `esses::catch2_nomain` | static library, tests only | the same without `main()`; `esses_add_test(... CUSTOM_MAIN)`; compiled only when linked |
| `esses::qtads` | **shared** library (LGPL), Qt builds only | alias of ADS's `qtadvanceddocking-qt6` (`...-qt6d` in Debug); `cmake/qtads.cmake` |

ADS details (D-026, D-153, `cmake/qtads.cmake`):

- Configured only through the options ADS declares (`BUILD_STATIC OFF`, `BUILD_EXAMPLES OFF`, `QT_VERSION_MAJOR`
  from our `find_package(Qt6)`), set in a function scope before `add_subdirectory(third_party/qtads ... SYSTEM)`;
  no upstream CMake file is edited (the two source changes of `qtads/UPSTREAM.md` touch no build file). The
  directory is SYSTEM (its headers are system headers for its users) and the target is built with
  `esses_silence_third_party()`.
- Its output directories (`<build>/x64/bin`, `x64/lib` upstream) are redirected to `<build>/bin` and
  `<build>/lib`, so the DLL, `.so` or `.dylib` sits beside every executable and test (08 section 3.7.5).
- Only `esses_widgets` links it (privately; `src/shell/ads_backend.cpp` is the only file that includes ADS) and
  the `esses_widgets` tests, which drive ADS's own controls. `cmake/qt-free-check.cmake` and
  `tools/check_layering.py` fail on any other link.
- Linux: ADS links `Qt6::GuiPrivate` (Qt 6.9 and later) and the system `xcb` library (`libxcb1-dev`).
- `-DESSES_DOCK_BACKEND=qdockwidget` builds without it (DockHost then has only its `QDockWidget` backend).

Catch2 details:

- Tests include the modular v3 headers (`<catch2/catch_test_macros.hpp>`, `<catch2/catch_approx.hpp>`,
  `<catch2/matchers/catch_matchers_floating_point.hpp>`, `<catch2/generators/catch_generators.hpp>`, ...).
  The build generates forwarders for the public headers (list in `cmake/third_party.cmake`) that include
  `catch_amalgamated.hpp`, so the tests need no change if the non-amalgamated Catch2 is adopted later.
- `CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS` is set for the library and every test, so `std::optional`, `std::pair`,
  `std::tuple`, `std::variant` and `std::chrono` values print in failure messages.
- On Windows the default `main()` is Catch2's `wmain()` (the library is compiled with `UNICODE`/`_UNICODE`, and
  `/ENTRY:wmainCRTStartup` is a link option of `esses::catch2`): the command line reaches Catch2 as UTF-8, so a
  test name outside ASCII still matches the spec ctest passes. A test with its own `main()` must use
  `CUSTOM_MAIN`; with `esses::catch2` its `main()` would be ignored on Windows and a duplicate elsewhere.
- Test discovery (`cmake/catch_discover.cmake`) lists each executable through Catch2's JSON reporter and
  registers one ctest entry per `TEST_CASE`; see `tools/dev/README.md`.
