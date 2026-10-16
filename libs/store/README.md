# esses_store

Layer 1, Qt-free. The chunked column store every channel lives in (01_ARCHITECTURE.md section 2.1,
02_DATA_MODEL.md section 3; D-009, D-011, D-012, D-043, D-054, D-062, D-123):

- `Chunk` / `ChunkBuilder`: immutable 65,536-sample blocks (F32, F64, I32, and I64 for time columns) in one
  64-byte-aligned allocation with an optional validity bitmap (1 = valid, LSB first) and a factor-4
  `MinMax{float lo, hi}` pyramid (levels 4 ... 65,536; wider dtypes rounded directionally, NaN and invalid
  samples excluded, empty bucket `{+inf, -inf}`). `seal()` downgrades `AllValid` to `Bitmap` on NaN.
- `MinMaxPyramid`, `SummaryPyramid` and `rangeMinMax`: the hierarchical boundary descent of exact M4; the
  per-column summary (levels with bucket >= 256, about 1 % of raw) is always resident.
- `IChunkProvider`, `Column`, `ChunkCache`, `ChunkHandle`: lazy materialisation with RAII pins, `tryPin` /
  `prefetch` for any thread, `require` for workers (Rule W: inline materialisation), one free path for chunk
  memory (cache lock, pin count zero). Budget tracking and a warning hook; no eviction before M5.
  `ChunkCache::insertGroup` seals the sibling chunks of one sweep; `WeakColumn` (M2, D-167) names those siblings
  without keeping them alive (the text importers' slab decoder).
- `UniformSeries`, `TimestampedSeries`, `Resampler`, `ResampleView`: the four interpolation policies.
- `InMemoryProvider`: in-memory columns for small derived data and tests.

Public headers live in `include/esses/store/`; the types are in namespace `esses::store`.

Must never depend on: any `esses_*` library other than `esses_base`, Qt, or third-party libraries.
