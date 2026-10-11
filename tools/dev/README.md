# Developer tools

## Building the Qt-free core on Windows

`build-core.cmd` configures, builds and tests with MSVC and Ninja from any shell (cmd, PowerShell, Git Bash).
It calls `vcvars64.bat` (Visual Studio 2022 Community by default; override with `ESSES_VCVARS`), keeps the
CMake that was on `PATH` before vcvars (Visual Studio puts its own, older CMake first), and falls back to
`C:\Qt\Tools\Ninja` when Ninja is not on `PATH`.

```
tools\dev\build-core.cmd [preset] [binary-dir] [build-type] [extra configure arguments...]

tools\dev\build-core.cmd                                              # win-msvc-core in build\win-msvc-core
tools\dev\build-core.cmd win-msvc-core build\skeleton-debug Debug
tools\dev\build-core.cmd win-msvc-core build\skeleton-rel RelWithDebInfo
set ESSES_CMAKE_ARGS=-DESSES_WARNINGS_AS_ERRORS=ON
tools\dev\build-core.cmd win-msvc-core build\wx Debug
```

From PowerShell, quote the path because the repository path contains spaces:
`& "<repo>\tools\dev\build-core.cmd" win-msvc-core build\skeleton-debug Debug`.

Environment: `ESSES_CMAKE_ARGS` passes extra configure arguments (use it for `-DNAME=VALUE`, because cmd.exe
splits positional arguments at `=`); `ESSES_SKIP_TESTS=1` configures and builds only; `ESSES_CTEST_ARGS` adds
ctest arguments (for example `-L property` or `-R taskpool`).

## Presets

| Preset | Use |
|---|---|
| `win-msvc-core`, `win-msvc-core-rel` | Qt-free core with MSVC, Debug / RelWithDebInfo (M0) |
| `win-msvc-debug`, `win-msvc-release` | with Qt (M1 onwards; needs `CMAKE_PREFIX_PATH` from `CMakeUserPresets.json`) |
| `linux-gcc-core`, `mac-clang-core` | Qt-free core on Linux and macOS |
| `linux-clang-asan`, `linux-clang-tsan`, `linux-clang-fuzz` | sanitizers and fuzzing (Linux, Clang) |
| `ci-*` | the CI variants: warnings as errors and assertions on |

`CMakeUserPresets.json` (git-ignored) holds machine-specific presets such as the Qt kit path (08 section 3.6).

## Leaving out an unfinished library

`ESSES_WITH_UNITS`, `ESSES_WITH_MODEL`, `ESSES_WITH_FORMATS` and `ESSES_WITH_CLI` (all ON by default) let a
configure skip `libs/units`, `libs/model`, `libs/formats` or `apps/cli` while it is being written, so work on a
sibling is not blocked. A component that DEPS on a switched-off one fails the layer check with a message naming
the switch to add:

```
set ESSES_CMAKE_ARGS=-DESSES_WITH_FORMATS=OFF -DESSES_WITH_CLI=OFF
tools\dev\build-core.cmd win-msvc-core build\units-debug Debug
```

## Tests

Tests use Catch2 v3 (`third_party/catch2v3`, amalgamated). Include the modular headers:
`<catch2/catch_test_macros.hpp>`, plus `<catch2/catch_approx.hpp>` (`Catch::Approx`), `<catch2/matchers/...>` or
`<catch2/generators/...>` as needed. `esses_add_test()` links Catch2 with its default `main()`;
`esses_add_test(... CUSTOM_MAIN ...)` links the variant without `main()` for a test that brings its own.

Each Catch2 `TEST_CASE` is one ctest entry named `<library>.<test name>`; Catch2 tags become ctest labels:
`ctest --test-dir build\skeleton-debug -L property`. A test case that calls `SKIP()` shows as skipped in ctest
(Catch2 exit code 4). The `lint` label holds the configure-time negative tests (Qt-free check, layer check, the
`ESSES_WITH_<LIB>` message) and the self-tests of the lint scripts and of the test discovery. Property tests
take their seed from `ESSES_TEST_SEED` (default fixed).

Discovery runs after each link: it lists the executable through Catch2's JSON reporter and gives every test its
quoted, escaped name as the test spec, so any name works, including `;`, `,`, brackets, quotes, backslashes and
non-ASCII text (on Windows the default `main()` is `wmain()`, which hands the command line to Catch2 as UTF-8).
A name that a Catch2 spec cannot select exactly fails the build with a message naming the test: a leading or
trailing `*`, an `exclude:` prefix, a line break, or two names that are equal once case and surrounding spaces
are ignored (Catch2 v3 accepts the same name twice when the tags differ). `tests/catch_discovery_tests.cpp`
holds the regression cases.

## Lint scripts (standard library only; run with `python -I`)

```
python -I tools/check_vendor_marks.py [--self-test]               # vendor marks outside the allow-list
python -I tools/check_layering.py --build-dir build/skeleton-debug  # includes and DEPS vs layers
python -I tools/check_layering.py --self-test
```

## Formatting and static analysis on this machine

clang-format and clang-tidy ship with Visual Studio's LLVM component:

```
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe" -i <files>
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe" -p build\skeleton-debug libs\base\src\*.cpp
```

## Fixtures, fuzzing and the probe timing

```
build\<dir>\bin\esses-gen-fixtures --inputs-only            # synthetic inputs into fixtures\motec.ld
build\<dir>\bin\esses-cli fixture verify fixtures\           # AC-M0-01..03 (fixtures\README.md)
python -I tools\build_private_fixtures.py --root . --cli build\<dir>\bin\esses-cli   # the user's files (private)
python -I tools\probe_timing.py --cli build\<rel>\bin\esses-cli --gen build\<rel>\bin\esses-gen-fixtures ^
       --file build\<rel>\perf\synthetic-500mb.ld            # AC-M0-12: median of 5, limit 200 ms
```

Fuzzing with MSVC's libFuzzer and ASan (VS 2022, x64): the preset `win-msvc-fuzz` builds only the libraries and
`fuzz_ld_header` / `fuzz_ldx` (tests and the CLI off). The ASan runtime DLL lives in the MSVC `bin\Hostx64\x64`
directory, which vcvars puts on `PATH`.

```
tools\dev\build-core.cmd win-msvc-fuzz build\fuzz
python -I tools\fuzz_seeds.py --inputs fixtures\motec.ld --out build\fuzz\seeds --max-bytes 65536
build\fuzz\bin\fuzz_ld_header.exe -max_total_time=60 -max_len=65536 build\fuzz\corpus\ld build\fuzz\seeds\fuzz_ld_header
build\fuzz\bin\fuzz_ldx.exe -max_total_time=60 -dict=build\fuzz\bin\fuzz_ldx.dict build\fuzz\corpus\ldx build\fuzz\seeds\fuzz_ldx
```

On Linux the same targets build with `linux-clang-fuzz` (the `fuzz-smoke` CI job).
