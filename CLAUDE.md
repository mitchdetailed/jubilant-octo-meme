# CLAUDE.md

Instructions for Claude Code in the Esses repository (D-128, D-151). The full guide for every AI agent is
`AGENTS.md`, imported at the end of this file; the rules below are the ones that must never be missed.

## Never

- **Never paste i2 help text, i2 shipped files, vendor XML or GPL source into a prompt that generates committed
  code, documentation, keymap `doc` strings or tooltips. Describe the behaviour in your own words first.**
  (The AI-provenance rule of `CONTRIBUTING.md`, verbatim.) This includes reading such material into your own
  context while you write committed text: work from the research summaries and the design documents, restate
  behaviour in your own words, and record the source in `CLEANROOM.md`.
- **Never commit private fixtures or vendor material:** nothing under `fixtures/private/` or
  `docs/design/background/` (both git-ignored; never `git add -f`), no real or vendor log, sample, export or
  configuration file, no `.chm`, `.i2wkb`, `.i2wsp`, `.mcs`, `.mfs` or `.mt2` file, no generated synthetic
  `input.ld`/`input.ldx`. Committed text may report aggregate results of a private check, never its content.
- **Never disassemble or decompile** the vendor's binaries or files, and never port GPL code.
- **Never put the vendor's or product's names** in identifiers, file names, UI strings, string literals or JSON
  values outside the allow-list (`tools/vendor_marks_allowlist.txt`); use neutral names (`Cfg*`, `Legacy*`).
- **Never let a lower layer depend upward or let Qt below layer 4** (`AGENTS.md` section 3).
- **Never take expectations from the code under test** (fixtures, goldens, lap tables; D-041), never edit
  `tools/ld_ref_decode.py` to agree with the reader, and never render or commit pixel goldens locally (they come
  from the ubuntu `qt-app` CI artefact, D-127).
- **Never commit, push, reset or switch branches** unless the maintainer asked for it in this session. Commits
  the maintainer asks for carry `Signed-off-by:` (`git commit -s`, DCO).

## The maintainer's machine

- Windows 11, MSVC 2022 (14.44), Ninja, CMake 4.4, Qt 6.7.2 at `C:/Qt/6.7.2/msvc2019_64`, Python 3.12. The Bash
  tool is Git Bash; the repository path contains spaces, so quote it and prefer absolute paths.
- Build with the wrappers, each into its own binary directory under `build/`:
  `tools\dev\build-core.cmd ci-win-msvc-core build\<task>-core` (Qt-free, as CI) and
  `tools\dev\build-qt.cmd ci-win-msvc-qt build\<task>-qt` (with Qt, offscreen tests, as CI). Run Qt tests with
  `QT_QPA_PLATFORM=offscreen` and the kit's `bin` on `PATH` (the wrapper does both).
- Run the Python tools with `python -I` (standard library only).
- The design documents are long: `grep -n` for the section, then read it by line range. Authority:
  `docs/design/DECISIONS.md`, then `ARCHITECTURE_OUTLINE.md`, then the numbered documents.

@AGENTS.md
