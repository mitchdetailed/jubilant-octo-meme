# Changelog

All notable changes are recorded here, newest first. The project follows semantic versioning from 0.1.

## Unreleased (M0)

- M0 acceptance review fixes (`docs/milestones/M0.md` §3): GCC 13 `-Werror` portability (comment, missing
  initialisers, dangling-reference heuristics) and `fast_float` for floating-point text (Apple Clang 15);
  clang-tidy clean `esses_formats`; `.ld` reader: C-series record rules keyed on the logger layout, tighter [mi2]
  fingerprint, work budgets for shared gap/aux blocks and the enumeration scan, linear duplicate renaming,
  reversed gap pairs and the `ld.gapConvention` option, cut UTF-8 tails, F64 for scaled integers beyond F32 and
  for u32, the all-zero terminator rule, device-event array bounds, new Info codes `ld.gap_block_shared`,
  `ld.gap_convention_shifted`, `ld.enum_index_duplicate`, `ld.enum_ref_ignored`; Esses' own device-event labels;
  `Column` exception safety and in-order builds for sequential providers; share-key tasks whose cancellation
  fired are not joined; JSON nesting limit (256); `fixture add` requires `--licence` for public fixtures and
  defaults `oracle.validity` to `finite-only` for the reference decoder; `bench/workloads.md`;
  `docs/milestones/M0.md`.
- Build (phase B): vendored-library targets `esses::nlohmann_json`, `esses::json_schema_validator`,
  `esses::cli11`, `esses::fast_float`, `esses::catch2` (Catch2 v3.7.1 amalgamated with its default main) and
  `esses::catch2_nomain`; all tests moved from Catch2 v2 to v3 and Catch2 v2 removed; test discovery through
  Catch2's JSON reporter, so test names with `;` and other special characters register correctly (names a test
  spec cannot select exactly are refused); per-component switches `ESSES_WITH_UNITS/MODEL/FORMATS/CLI`;
  provenance of every vendored library in `third_party/README.md`, `NOTICES.md` and `REUSE.toml` updated.
- Repository skeleton: CMake presets, prefix table, target helpers with the layer check, configure-time
  Qt-free check with negative tests, vendored pugixml/xxHash/Catch2 targets, CI workflow stubs, governance
  and clean-room files, vendor-mark and layering lint scripts.
- `esses_base`: time base, exact 128-bit helpers and `Rate`, `Result`/`Error`, `Uuid`, `Hash128`, UTF-8 and
  case folding, `CancelToken`, `Future`, `TaskPool`, `Executor`, `ThreadRole`, `MappedFile`, `ByteReader`,
  `AtomicFile`, `Paths`, developer log, version stamping.
- `esses_store`: `Chunk`/`ChunkBuilder` (F32/F64/I32 and I64 time chunks, validity modes, NaN downgrade),
  factor-4 `MinMaxPyramid` with directional float rounding, `SummaryPyramid`, the exact-M4 range query
  (hierarchical boundary descent), `IChunkProvider`, `ChunkCache` with budget tracking and a warning hook (no
  eviction before M5), `ChunkHandle` pins, `Column` (Rule W `require`, `tryPin`/`prefetch`, `buildSummary`),
  `UniformSeries`/`TimestampedSeries`, `Resampler`/`ResampleView` with the four interpolation policies,
  `InMemoryProvider`.
- `esses_units`: `Dimension` (with `pow`/`unknown`), affine `Converter`, `Quantity`/`Unit` and `UnitRegistry`
  (exact, case-folded and normalised lookups, quantity-scoped lookups, family-checked conversions, delta
  quantities, `byDimension`), loaded from the new `data/units.json` (29 quantities, 111 units from the SI
  Brochure and NIST SP 811) and an optional `units.user.json`; `schemas/units-1.schema.json` gains `toBase`,
  `caseSensitive`, `note` and quantity `aliases`.
