# Contributing to Esses

Thank you for helping. Esses is a clean-room, MIT-licensed reimplementation of a motorsport data-analysis
workflow. Because of that, *where* code and behaviour come from matters as much as whether they work. Please
read the clean-room rules below and `CLEANROOM.md` before your first change.

## Building and testing

The developer setup and the presets are described in `tools/dev/README.md` (08_REPO_AND_BUILD.md section 12).
In short, the Qt-free core:

```
cmake --preset win-msvc-core        # or linux-gcc-core, mac-clang-core
cmake --build --preset win-msvc-core
ctest --preset win-msvc-core
```

On Windows `tools\dev\build-core.cmd` sets up the MSVC environment and runs all three steps.

## Branches, pull requests and sign-off

- Work on a branch and open a pull request against `main`; `main` is protected and merges are squashed.
- **Every commit needs a `Signed-off-by:` line** (`git commit -s`), certifying the Developer Certificate of
  Origin in `DCO`. There is no CLA; you keep your copyright and license your contribution under MIT.
- Say in the pull request which acceptance criterion (for example `AC-M0-08`) or decision (`D-nnn`) the
  change serves.
- CI must be green: `core` on three operating systems, `qt-app` on Windows and Linux (from M1), `lint`,
  `sanitize` and `fuzz-smoke`. `qt-app` on macOS runs nightly and does not block a merge.
- AI-assisted contributors: `AGENTS.md` (and `CLAUDE.md` for Claude Code) summarise the architecture, the
  build and test conventions and the rules below.

## Coding standards

- C++20. Formatting is `clang-format` with the repository's `.clang-format`; Qt-free libraries are also checked
  with `clang-tidy` (`.clang-tidy`). Warnings are errors in CI.
- Layering (01_ARCHITECTURE.md section 2): a library depends only on lower layers and declares every
  dependency in its `esses_add_library(... DEPS ...)`; configure fails on an upward edge and
  `tools/check_layering.py` fails on an undeclared include.
- **Qt-free rule:** layers 0-3 (`base`, `units`, `store`, `model`, `formats`, `analysis`, `maths`, `project`,
  `plotcore`, the CLI) never include a Qt header or link a Qt module (`cmake/qt-free-check.cmake`). Strings
  there are UTF-8 `std::string`.
- **No exceptions across library boundaries:** fallible public functions return `esses::Result<T>`.
- Names never contain vendor marks (`TRADEMARK.md`); code handling a vendor format uses neutral names
  (`Cfg*`, `Legacy*`).
- Every source file starts with the SPDX header:
<!-- REUSE-IgnoreStart -->
  `// SPDX-FileCopyrightText: 2026 the Esses contributors` and `// SPDX-License-Identifier: MIT`.
<!-- REUSE-IgnoreEnd -->

## Tests

Each layer has its own tests (08_REPO_AND_BUILD.md section 6): Catch2 unit and property tests for the Qt-free
libraries (`libs/<lib>/tests/`, tags `[unit]`, `[property]`, `[slow]`, `[oracle]`), fixtures with independent
oracles for importers, golden JSON for the CLI, Qt Test and golden images for the GUI layers. New behaviour
comes with tests; bug fixes come with a regression test.

## Clean-room and provenance rules

- **No disassembly or decompilation** of MoTeC binaries or files, ever.
- i2 behaviour may come only from the help pages as displayed by the shipped help viewer, shipped XML or
  configuration files, SDK samples, observation on a licensed install (black-box only), and public sources.
- **Provenance rule:** any code or behaviour derived from a public project or an i2 document must name it in
  `CLEANROOM.md` (area, behaviour, category, source, date).
- **Ported-code rule:** a file ported from an MIT/BSD project keeps the upstream `SPDX-FileCopyrightText`
  line beside ours and gets a `ported-from` row in `CLEANROOM.md`.
- GPL sources (for example gotzl/ldparser) are read for understanding only and never ported.
- **AI-provenance rule:** never paste i2 help text, i2 shipped files, vendor XML or GPL source into a prompt
  that generates committed code, documentation, keymap `doc` strings or tooltips. Describe the behaviour in
  your own words first. Before a pull request that adds user-facing text, run `tools/check_text_overlap.py`
  (local only; **planned**: the tool does not exist yet and arrives with the first user-facing text, as a
  release-checklist item of 08 §8). Until then, a reviewer compares new user-facing text by hand.
- **Repository hygiene:** `research/`, `help_text/`, `conv_samples/` and vendor files (`.chm`, `.i2wkb`,
  `.i2wsp`, `.mcs`, `.mfs`, `.mt2`) never enter the repository; the `lint` job fails if they do. Real logs and
  vendor samples stay in the private fixture checkout and are never committed.

## Adding a file format

A new format is: a reader (or a text profile) + a dictionary + at least one fixture with documented
provenance and an independent oracle + a `CLEANROOM.md` entry + a page in `docs/formats/`.

## Licence of contributions

Contributions are accepted under the MIT licence, the licence of the project.
