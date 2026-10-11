# esses_base

Layer 0, Qt-free. The primitives every other library builds on (01_ARCHITECTURE.md section 2.1,
02_DATA_MODEL.md section 1): `Micros`/`SampleIdx`/`UtcInstant` and the saturating time helpers, exact 128-bit
helpers (`muldivFloor`, `floorDiv128`), `Rate` with drift-free integer `timeAt`/`indexAt` (D-008, D-129),
`Result<T>`/`Error`/`ErrorCode`/`TextSpan` (01 section 6), `Uuid`, `Hash128` (xxh3-128), UTF-8 helpers and the
`caseFold` primitive, `CancelToken`/`CancelSource`, `Future<T>`, `TaskPool` with three priority bands and key
coalescing (D-031, D-123), `Executor`/`InlineExecutor`, `ThreadRole`, `MappedFile`, `ByteReader`, `AtomicFile`,
`Paths`, the developer log (`Log`, `ILogSink`, `ESSES_LOG`) and the build identification in `version.hpp`.

Public headers live in `include/esses/base/`; the types are in namespace `esses` (`esses::base` is an alias).

Must never depend on: any other `esses_*` library, Qt, or any third-party library other than the vendored
xxHash (private). `JsonDoc` (nlohmann-json) is reserved for phase B (`json_doc.hpp`).
