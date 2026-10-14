# esses_base

Layer 0, Qt-free. The primitives every other library builds on (01_ARCHITECTURE.md section 2.1,
02_DATA_MODEL.md section 1): `Micros`/`SampleIdx`/`UtcInstant` and the saturating time helpers, exact 128-bit
helpers (`muldivFloor`, `floorDiv128`), `Rate` with drift-free integer `timeAt`/`indexAt` (D-008, D-129),
`Result<T>`/`Error`/`ErrorCode`/`TextSpan` (01 section 6), `Uuid`, `Hash128` (xxh3-128), UTF-8 helpers and the
`caseFold` primitive, `CancelToken`/`CancelSource`, `Future<T>`, `TaskPool` with three priority bands and key
coalescing (D-031, D-123; `queuedTasks(prefix)` lists the queued keys and bands for diagnostics and tests),
`Executor`/`InlineExecutor`, `ThreadRole`, `MappedFile`, `ByteReader`, `AtomicFile`,
`readWholeFile`/`readFileHead`/`requireRegularFile` (`read_file.hpp`: the one whole-file reader of documents,
which checks the file type before it opens anything, so a directory is Io on every standard library, and on
Windows shares delete access, so a file that `AtomicFile` is replacing opens at every instant), `Paths`, the
developer log
(`Log`, `ILogSink`, `ESSES_LOG`), the build identification in `version.hpp`, and
`JsonDoc` (`json_doc.hpp`): an immutable JSON value held as compact canonical text and the owner of the one
canonical serialisation of 02 section 6.5 rule 1 (`canonical()`, `saveAtomic()`, `schemaId()`). Libraries that
build or read JSON with nlohmann-json use the interop header `json_nlohmann.hpp` (namespace `esses::jsonio`:
`parseJson`, `readFile`, `canonicalJson`, `compactCanonicalJson`, `toDoc`/`fromDoc`, the typed readers
`as*`/`ObjectReader`/`checkSchemaId` and `guardJson`) and link `esses::nlohmann_json` themselves.

Public headers live in `include/esses/base/`; the types are in namespace `esses` (`esses::base` is an alias).

Must never depend on: any other `esses_*` library, Qt, or any third-party library other than the vendored
xxHash and nlohmann-json (both private; 01 section 2.1). `json_nlohmann.hpp` is the only public header that
includes a third-party header, and no other public header includes it.
