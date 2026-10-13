# 02 — Data model: `esses_base`, `esses_units`, `esses_store`, `esses_model`, `esses_project`

**Scope.** The Qt-free data layer of Esses: primitive types (`Micros`, `Rate`, `Uuid`, `Hash128`), the unit registry, the chunked column store and series kinds, the data-source model (sources, layers, channels, details, markers, ranges, resolver), the sidecar document, and every JSON document in a project folder together with its schema, migration, round-trip and undo rules. Everything here builds with `ESSES_BUILD_QT=OFF`. Maths evaluation, range-rule *execution*, distance indexing, plot geometry and the Qt application model are specified elsewhere; this document fixes the types they consume and the files they read and write.

**Related decisions.** D-007, D-008, D-009, D-010, D-011, D-012, D-013, D-014, D-015, D-016 (offsets/stretch fields only), D-017 (persisted selection), D-019, D-020 (compat flag persistence), D-022 (cache keys), D-024 (module schema), D-029 (dictionaries), D-031 (snapshot rule), D-032, D-033, D-036.

Conventions: namespace `esses`, sub-namespaces `esses::base`, `esses::units`, `esses::store`, `esses::model`, `esses::project`. All strings are UTF-8 `std::string`. All times are `Micros`. No exceptions cross a library boundary: fallible operations return `Result<T>` with `Error{ErrorCode, message, ErrorContext}` exactly as declared once in 01 §6.1 (an own C++20 class in `esses_base`; never `std::expected`, which is C++23). Every persisted number that carries a physical unit is written as `{"v": <number>, "unit": "<symbol>"}`; bare numbers are counts, indices, fractions or microseconds.

---

## 1. `esses_base` primitives

```cpp
namespace esses::base {

using Micros    = std::int64_t;          // microseconds since the data layer's origin
using SampleIdx = std::int64_t;

struct UtcInstant { std::int64_t unixUs; };   // optional absolute anchor of Micros 0 per layer

// Exact 128-bit helpers (behaviour 1). GCC/Clang: __int128. MSVC x64: _mul128/_umul128 + _div128/_udiv128
// with an explicit sign fix-up. Division rounds toward -infinity (C++ '/' truncates toward zero and is wrong for t < t0).
std::int64_t muldivFloor(std::int64_t a, std::uint64_t b, std::uint64_t c) noexcept;       // floor(a*b / c)
std::int64_t floorDiv128(std::int64_t hi, std::uint64_t lo, std::uint64_t d) noexcept;      // floor of a signed 128-bit / d

struct Rate {                              // rational hertz, D-008
  std::uint32_t num = 0, den = 1;          // num/den Hz; num == 0 means "no rate" (timestamped)
  constexpr bool   valid() const noexcept { return num != 0 && den != 0; }
  double           hz() const noexcept    { return double(num) / double(den); }   // display, maths "Auto"
  // D = 1'000'000ull * den, computed in uint64_t (never `1'000'000 * den`, which is a wrapping 32-bit multiply).
  Micros           timeAt(SampleIdx i, Micros t0) const noexcept;   // t0 + floorDiv(2*i*D + num, 2*num); 128-bit products
  SampleIdx        indexAt(Micros t, Micros t0) const noexcept;     // floorDiv(num*(2*(t-t0)+1) - 1, 2*D): largest i with timeAt(i) <= t
  static Rate      fromHz(std::uint32_t hz) { return {hz, 1}; }
  static Rate      fromPeriodUs(std::uint64_t periodUs);            // exact rational, reduced by gcd
  Rate             normalised() const;                              // gcd-reduced
  friend constexpr std::strong_ordering operator<=>(const Rate& a, const Rate& b) noexcept {
    return std::uint64_t(a.num) * b.den <=> std::uint64_t(b.num) * a.den; }   // value order, not memberwise
  friend constexpr bool operator==(const Rate& a, const Rate& b) noexcept {
    return std::uint64_t(a.num) * b.den == std::uint64_t(b.num) * a.den; }    // {2,2} == {1,1}
};

struct Uuid     { std::array<std::uint8_t, 16> b{}; std::string str() const; static Uuid parse(std::string_view);
                  friend auto operator<=>(const Uuid&, const Uuid&) = default;      // byte-wise; makes ChannelId/MarkerId/RangeId <=> compile
                  friend bool operator==(const Uuid&, const Uuid&) = default; };
struct Hash128  { std::uint64_t lo = 0, hi = 0; std::string hex() const; };   // xxh3-128
Hash128 hash128(std::span<const std::byte>);
Hash128 hash128(std::initializer_list<std::string_view> parts);     // length-prefixed concatenation, then xxh3-128
Uuid    uuidFromHash(Hash128, std::uint8_t version = 5);            // sets version/variant bits; deterministic ids
Uuid    uuidRandom();                                               // v4; user-created objects only

std::string caseFold(std::string_view utf8);   // NFKC normalisation then full Unicode case folding
bool        ciEquals(std::string_view, std::string_view);

// Error, ErrorCode, ErrorContext, Result<T>, ok(), fail(): declared once in 01 §6.1 (not repeated here).

class CancelToken;        // cooperative; checked at chunk/block granularity
template <class T> class Future;
class TaskPool;           // Priority {Interactive, Visible, Background}; see 01 §5

class MappedFile {        // read-only whole-file view; used by LdChunkProvider (05 §3.11)
public:
  static Result<MappedFile> open(const std::filesystem::path&);   // Windows: CreateFileW(GENERIC_READ,
                                                                  //   FILE_SHARE_READ | FILE_SHARE_DELETE) + MapViewOfFile
  std::span<const std::byte> bytes() const noexcept;
  struct Stamp { std::uint64_t size; std::int64_t mtimeUnixUs; Hash128 quick; };
  const Stamp& stampAtOpen() const noexcept;
  bool changedOnDisk() const;                                     // re-stat: size or mtime differs from stampAtOpen()
  // Runs a plain C-style decode routine over the view; a page fault inside it (EXCEPTION_IN_PAGE_ERROR on Windows via
  // __try/__except in a small C function; SIGBUS on POSIX via a handler + per-thread sigsetjmp) becomes fail(Io).
  Result<void> guarded(void (*decode)(void* ctx), void* ctx) const noexcept;
};
}
```

Numbered behaviours:

1. **Rate arithmetic.** With `D = 1'000'000ull·den` (64-bit), `timeAt(i) = t0 + floorDiv(2·i·D + num, 2·num)` (equal to D-008's `llround_div` for `i ≥ 0`) and `indexAt(t) = floorDiv(num·(2·(t − t0) + 1) − 1, 2·D)`, which is the **exact inverse**: the largest `i` with `timeAt(i) ≤ t`, so a lookup at a sample's own timestamp returns that sample. Every product is formed in 128 bits through `muldivFloor`/`floorDiv128` (products reach ≈ 4 × 10²⁸ for `num, den < 2³²`; MSVC has no `__int128`), and every division rounds toward −∞ so `t < t0` maps to negative indices. Worked check: `{512,25}` (MoTeC CSV 20.48 Hz) gives `timeAt(1) = 48,828`, `indexAt(48,828) = 1`, `indexAt(48,827) = 0`; `{30,1}` gives `timeAt(1) = 33,333`, `indexAt(33,333) = 1`. `fromPeriodUs(33'333)` returns `{1'000'000, 33'333}` (29.9997 Hz, already reduced); an importer that *knows* the nominal rate states it in its dictionary: a 30 Hz logger whose period prints as 33,333 µs is `{"num": 30, "den": 1}`, a 33.333 Hz logger (30 ms period) is `{"num": 100, "den": 3}`. The corrected `indexAt` formula is proposed for D-008 in DECISIONS.md Addendum A.
2. Two rates are equal iff `num·den' == num'·den` (64-bit cross products), so `{2,2} == {1,1}`; `operator<=>` orders by value on the same cross products (`{1,2}` = 0.5 Hz < `{1,1}`), and "highest input rate" (maths Auto) is `max` under it.
3. `hash128(parts)` is the only way to derive a deterministic id in the code base; every id derivation below lists its parts in order so the same inputs produce the same id on every platform and build.
4. `caseFold` is the single case-folding primitive; every "case-insensitive" lookup in this document means equality of `caseFold` results.
5. **A mapped log changed underneath the app.** `MappedFile` opens with read access and `FILE_SHARE_READ | FILE_SHARE_DELETE` only (never `FILE_SHARE_WRITE`): a writer that replaces the file atomically (temp + rename over) can proceed while the view keeps the old bytes, and an in-place writer that would truncate or extend the file (sims and logger downloads rewriting a session) gets a clean sharing error instead of shrinking a live mapping. `stampAtOpen()` records `(size, mtime, quick hash)`; the provider calls `changedOnDisk()` before each decode batch and the GUI on application focus; a change marks the source stale (Messages row with Reload, 06), and no further chunk is decoded from it. Each per-chunk decode runs inside `guarded()`, so a page fault (truncation by a writer that ignored sharing, a dropped network share) is mapped to `fail(Io)`, the chunk is sealed `AllInvalid` and logged once per source; it never crashes a worker. A test on NTFS and on an SMB share covers rename-over, in-place rewrite and a disconnected share (M4).

---

## 2. `esses_units`

### 2.1 Types

```cpp
namespace esses::units {

using UnitId = std::uint16_t; using QuantityId = std::uint16_t;
constexpr UnitId kNoUnit = 0;   // Unitless base

struct Dimension { std::array<std::int8_t, 7> exp{}; };   // [L, M, T, I, Θ, N, J]; angle and ratio are dimensionless

struct Quantity {
  QuantityId  id;
  std::string name;              // "Temperature", "Temperature Delta", "Angular Speed"
  Dimension   dim;
  std::string family;            // display family: distinct for Delta vs Absolute, Torque vs Energy, Angle vs Ratio
  UnitId      base;              // the SI base unit of this quantity (scalar 1, offset 0)
  bool        isDelta = false;
  std::optional<QuantityId> deltaFamily;   // Temperature -> Temperature Delta; Pressure -> Pressure Delta
};

struct Unit {
  UnitId      id;
  std::string symbol;            // canonical ASCII key: "km/h", "kPa a", "deg C"
  std::string display;           // Unicode: "°C", "µs", "m³/s"; empty = symbol
  QuantityId  qty;
  double      scalar = 1.0, offset = 0.0;   // value_in_unit = value_in_base * scalar + offset (i2's affine model)
  std::vector<std::string> aliases;         // case-insensitive import spellings, incl. i2 ASCII forms
};

struct Converter {               // y = a*x + b, from -> to
  double a = 1.0, b = 0.0;
  bool identity() const noexcept { return a == 1.0 && b == 0.0; }
  float  operator()(float x) const noexcept  { return float(a * x + b); }
  double operator()(double x) const noexcept { return a * x + b; }
};

class UnitRegistry {
public:
  static Result<UnitRegistry> load(const JsonDoc& unitsJson);        // data/units.json (+ units.user.json)
  std::optional<UnitId>     find(std::string_view symbolOrAlias) const;     // case-fold, whitespace-collapsed
  const Unit&               unit(UnitId) const;
  const Quantity&           quantity(QuantityId) const;
  QuantityId                quantityOf(UnitId) const;
  std::optional<QuantityId> quantityByName(std::string_view) const;
  Result<Converter>         converter(UnitId from, UnitId to) const;        // fail(ErrorCode::Invalid, detail "unit.family") when families differ
  Converter                 toBase(UnitId) const;  Converter fromBase(UnitId) const;
  std::span<const UnitId>   unitsOf(QuantityId) const;                      // in units.json order (display lists)
  std::optional<QuantityId> deltaOf(QuantityId) const;
  std::optional<QuantityId> byDimension(const Dimension&, std::string_view familyHint = {}) const;
};
}
```

### 2.2 Rules

1. Samples, scale limits, maths constants and every persisted limit are in the quantity's **base unit** (D-010). The importer converts logged values to base once at decode; the `.ld` writer re-quantises with the original `scale/dec/mul/shift` so round trips are exact.
2. `converter(from, to)`: `a = s_to / s_from`, `b = o_to − o_from · s_to / s_from`. Conversion between quantities of different *families* is an error even when dimensions match (`Temperature` → `Temperature Delta`, `Torque` → `Energy & Work`, `Angular Speed` → `Frequency`). A maths subtraction of two Absolute values yields the `deltaFamily` quantity; the checker (03 §3.2) asks `deltaOf`.
3. `find` collapses internal whitespace, case-folds, and tries symbol, then aliases, then symbol with the vendor normaliser (`kph`→`km/h`, `Nm`→`N.m`, `°C`/`øC`/`░C`→`deg C`). Unknown symbols are not errors at this layer; the importer's `UnitMappingProfile` (D-028) decides.
4. AFR: quantity `Air Fuel Ratio` with base unit `lambda` (alias `LA`); the i2 fuel-specific units (`P A/F` 14.7, `A A/F` 6.4, `D A/F` 14.5, `LPG A/F` 15.5) are affine units with fixed scalars kept so imported maths and logged units resolve. The vehicle-specific ratio is **not** a unit: the shipped circuit maths module defines `AFR = 'Lambda' * 'Stoich Ratio'` over the numeric detail `Stoich Ratio` (default 14.7), so one lambda channel displays correctly for any fuel without a per-fuel unit (D-010).
5. User units: `units.user.json` (per user) may add quantities and units; it may not redefine shipped symbols. Shipped symbols are frozen for life ("i2-compatible core").
6. Registry ids are process-local and assigned in load order; **files never store ids**, only symbols.

### 2.3 `data/units.json` excerpt (schema `units-1`)

```json
{ "$schema": "urn:esses:units-1", "schemaVersion": 1,
  "quantities": [
    { "name": "Speed", "family": "Speed", "dim": [1,0,-1,0,0,0,0], "base": "m/s",
      "units": [ { "symbol": "m/s" },
                 { "symbol": "km/h", "scalar": 3.6, "aliases": ["kph", "KPH", "km/hr"] },
                 { "symbol": "mph", "scalar": 2.2369362920544, "aliases": ["mi/h", "MPH"] } ] },
    { "name": "Temperature", "family": "Temperature", "dim": [0,0,0,0,1,0,0], "base": "K", "deltaFamily": "Temperature Delta",
      "units": [ { "symbol": "K" },
                 { "symbol": "deg C", "display": "°C", "offset": -273.15, "aliases": ["C", "°C", "degC", "øC"] },
                 { "symbol": "deg F", "display": "°F", "scalar": 1.8, "offset": -459.67, "aliases": ["F", "°F"] } ] },
    { "name": "Temperature Delta", "family": "Temperature Delta", "dim": [0,0,0,0,1,0,0], "base": "dK", "isDelta": true,
      "units": [ { "symbol": "dK", "display": "ΔK" }, { "symbol": "dC", "display": "Δ°C" }, { "symbol": "dF", "display": "Δ°F", "scalar": 1.8 } ] },
    { "name": "Pressure", "family": "Pressure", "dim": [-1,1,-2,0,0,0,0], "base": "Pa", "deltaFamily": "Pressure Delta",
      "units": [ { "symbol": "Pa" }, { "symbol": "kPa a", "scalar": 0.001, "aliases": ["kPa", "kPaA"] },
                 { "symbol": "bar a", "scalar": 1e-5, "aliases": ["bar"] }, { "symbol": "psi a", "scalar": 0.000145037738, "aliases": ["psi"] } ] },
    { "name": "Pressure Delta", "family": "Pressure Delta", "dim": [-1,1,-2,0,0,0,0], "base": "dPa", "isDelta": true,
      "units": [ { "symbol": "dPa" }, { "symbol": "kPa g", "scalar": 0.001, "aliases": ["dKPA", "kPag"] }, { "symbol": "psi g", "scalar": 0.000145037738 } ] },
    { "name": "Angular Speed", "family": "Angular Speed", "dim": [0,0,-1,0,0,0,0], "base": "rad/s",
      "units": [ { "symbol": "rad/s" }, { "symbol": "rpm", "scalar": 9.549296585513721, "aliases": ["RPM", "r/min"] },
                 { "symbol": "1000rpm", "scalar": 0.009549296585513721, "aliases": ["rpm1000", "krpm"], "compat": "i2" } ] },
    { "name": "Ratio", "family": "Ratio", "dim": [0,0,0,0,0,0,0], "base": "ratio",
      "units": [ { "symbol": "ratio" }, { "symbol": "%", "scalar": 100 }, { "symbol": "Trim", "offset": -1, "compat": "i2" },
                 { "symbol": "%Trim", "scalar": 100, "offset": -100, "compat": "i2" } ] },
    { "name": "Air Fuel Ratio", "family": "Air Fuel Ratio", "dim": [0,0,0,0,0,0,0], "base": "lambda",
      "units": [ { "symbol": "lambda", "display": "λ", "aliases": ["LA"] }, { "symbol": "P A/F", "scalar": 14.7, "compat": "i2" },
                 { "symbol": "A A/F", "scalar": 6.4, "compat": "i2" }, { "symbol": "D A/F", "scalar": 14.5, "compat": "i2" },
                 { "symbol": "LPG A/F", "scalar": 15.5, "compat": "i2" } ] } ] }
```

`Angle` (base `rad`, with a sign-flipping compatibility unit `dBTDC`, scalar −57.2957795) and `Acceleration` (base `m/s/s`, unit `G` scalar 1/9.80665) follow the same pattern. **Sizing and provenance.** Quantities, base units and conversion factors come from independent physical definitions (the SI Brochure and NIST SP 811 for SI and customary units; standard automotive units such as `km/h`, `mph`, `rpm`, `psi`, `bar`, `lambda`), and the table holds the quantities that the shipped readers, dictionaries, maths and components need; it is sized by need, not by the vendor's quantity list. Spellings that exist only for i2 compatibility (`P A/F`, `%Trim`, `1000rpm`, `dBTDC`, `dKPA` …) are defined from their physical meaning, flagged `"compat": "i2"`, and act as the alias map that lets imported user maths parse (D-010); no vendor file is copied. `CLEANROOM.md` line: *units: own table from SI Brochure / NIST SP 811 and standard automotive units; i2 `units.xml` consulted only for alias spellings and the affine `scalar/offset` model; no table copied.*

Acceptance: property test — for every unit `u`, `toBase(u) ∘ fromBase(u)` is identity within 1 ulp on 10⁶ random values; `converter(deg C, dC)` is an error; `find("KPH") == find("km/h")`; the registry loads the full table in < 5 ms.

---

## 3. `esses_store`

### 3.1 Chunk, validity, pyramid

```cpp
namespace esses::store {
using base::Micros; using base::SampleIdx; using base::Rate;

enum class Dtype    : std::uint8_t { F32, F64, I32, I64 /* time columns only; never a ChannelDescriptor dtype */ };
enum class Validity : std::uint8_t { AllValid, Bitmap, AllInvalid };
enum class Interp   : std::uint8_t { Linear, Closest, Previous, Next };    // i2 names, D-009

constexpr std::uint32_t kChunkSamples  = 65'536;
constexpr unsigned      kPyramidBase   = 4;          // buckets 4,16,64,256,1024,4096,16384,65536
constexpr unsigned      kPyramidLevels = 8;
constexpr unsigned      kSummaryFirstLevel = 3;      // bucket 256 and up form the SummaryPyramid
constexpr std::size_t   kBitmapBytes   = kChunkSamples / 8;   // 8 KiB

struct MinMax { float lo, hi; };                     // every dtype (D-054, 01 §7.1); wider dtypes (F64, I32) rounded
                                                     // directionally: lo toward −∞, hi toward +∞, so a bucket is a
                                                     // conservative enclosure; empty bucket: {+inf, -inf} (04 §4.1)
                                                     // (so min/max fold is NaN-free)

struct Chunk {                                       // immutable after seal(); one 64-byte-aligned allocation
  std::uint32_t count;                               // <= kChunkSamples; only the last chunk of a column is partial
  Dtype         dtype;
  Validity      validity;
  std::uint16_t builtLevels;                         // bitmask; all 8 bits set after seal()
  std::uint64_t version;                             // provider version at materialisation (cache key)
  std::byte*    data;                                // samples | [bitmap] | L0 | L1 | ... | L7

  template <class T> std::span<const T> samples() const;
  std::span<const MinMax> level(unsigned k) const;   // bucket = 4^(k+1) samples; float entries for every dtype
  const std::uint64_t* bitmap() const noexcept;      // nullptr unless validity == Bitmap
  bool validAt(std::uint32_t i) const noexcept;      // mode branch, then bit test, then NaN test for floats
  std::size_t bytes() const noexcept;                // samples + bitmap + pyramid
  static std::size_t layoutBytes(std::uint32_t count, Dtype, bool bitmap);
};

class MinMaxPyramid {                                // view over one chunk's levels; built by ChunkBuilder::seal()
public:
  explicit MinMaxPyramid(const Chunk&);
  MinMax query(std::uint32_t first, std::uint32_t last) const;   // hierarchical descent (D-012); exact for F32, a
                                                                 // conservative enclosure for wider dtypes
  unsigned levels() const noexcept;
};

class SummaryPyramid {                               // whole column, levels with bucket >= 256, always resident, ~1 % of raw
public:
  MinMax minmax(SampleIdx first, SampleIdx last) const;   // at bucket granularity, clamped; exact for F32,
                                                          // a conservative enclosure for wider dtypes
  bool complete() const noexcept;                    // false while background build is running
  std::uint64_t version() const noexcept;
};
}
```

Layout per F32 chunk: samples 262,144 B; bitmap 8,192 B when `Bitmap`; pyramid 16,384+4,096+1,024+256+64+16+4+1 = 21,845 pairs = 174,760 B (0.67 × raw); the pairs are `float` for every dtype, so an F64 chunk's pyramid is the same 174,760 B (01 §7.1). The `SummaryPyramid` holds the per-chunk slices of levels 3…7 (341 pairs = 2,728 B per chunk) in a contiguous array per column.

Behaviours:

1. A chunk is produced by a `ChunkBuilder` (owned by `ChunkCache`): `allocate(count, dtype, wantBitmap)` → write samples → optionally set invalid bits → `seal()` builds all 8 levels in one pass over the samples (NaN-blended, invalid-masked) plus 7 reductions, sets `builtLevels = 0xFF`, and returns a `const Chunk*`. After `seal()` the chunk is never written again (D-011).
2. For float dtypes, NaN is invalid regardless of mode; `seal()` *downgrades* `AllValid` to `Bitmap` if any NaN is present, so `validity` is authoritative and the decimator may skip the NaN test when the mode is `AllValid`.
3. For `I32` columns, invalidity is bitmap-only; `samples<int32_t>()` of an invalid sample holds the raw value (importers write 0).
4. `AllInvalid` chunks carry no sample storage (`data` points at a zero-length block) and `bytes()` is the header only.
5. Pyramid min/max exclude invalid samples; a bucket with no valid samples is the empty sentinel, which every consumer must test with `lo > hi`.

### 3.2 Providers, cache, handles, columns

```cpp
namespace esses::store {
using Priority = esses::Priority;                     // the TaskPool enum of 01 §5.1; no second enum

struct IChunkProvider {
  virtual ~IChunkProvider() = default;
  virtual SampleIdx     size() const = 0;
  virtual Dtype         dtype() const = 0;
  virtual std::uint64_t version() const = 0;           // bumps invalidate every chunk (maths def change, source change)
  virtual bool          chunkIndependent() const = 0;  // true: any chunk alone; false: sequential (stateful maths)
  virtual Result<void>  materialise(SampleIdx chunkNo, ChunkBuilder& out, base::CancelToken) = 0;   // thread-safe
  virtual std::string   debugName() const = 0;         // "ld:Engine RPM", "plan:Corr Dist@v12"
};
// Implementations: LdChunkProvider (esses_formats), ImporterChunkProvider (esses_formats), PlanProvider (esses_maths),
// later DcfProvider, LiveProvider.

class ChunkHandle {                                     // RAII pin over [firstChunk, lastChunk]; move-only
public:
  ChunkHandle() = default;
  ChunkHandle(ChunkHandle&&) noexcept; ChunkHandle& operator=(ChunkHandle&&) noexcept;   // move-assign unpins the old span first
  ChunkHandle(const ChunkHandle&) = delete; ChunkHandle& operator=(const ChunkHandle&) = delete;
  ~ChunkHandle();                                                                       // unpins
  bool          empty() const noexcept;
  const Chunk*  chunk(SampleIdx chunkNo) const noexcept;                            // nullptr outside the pinned span
  template <class T> std::span<const T> samples(SampleIdx chunkNo) const;
  double        at(SampleIdx i) const noexcept;                                     // NaN if invalid; any dtype
  SampleIdx     firstChunk() const noexcept; SampleIdx lastChunk() const noexcept;
};

class ChunkCache {                                      // process-wide, D-011
public:
  static ChunkCache& instance();                        // the cache installed by the runtime (01 §5.3 rule 11); asserts if
                                                        // none; no function-local static; tests construct their own
  void        setBudgetBytes(std::size_t);              // default max(1 GiB, 25 % physical RAM)
  std::size_t budgetBytes() const noexcept;
  struct Stats { std::size_t residentBytes, pinnedBytes, chunks, pinnedChunks; std::uint64_t hits, misses, evictions; bool overBudget; };
  Stats       stats() const;
  // Internal API used by Column: lookup / insert / pin / unpin / touch; eviction policy LRU over unpinned chunks (M5, D-043);
  // M0..M4: no eviction, overBudget flag raised, Messages dock warns (D-011). Chunk memory is freed on one path only: under
  // the cache lock, for a chunk whose pin count is zero (LRU eviction and stale drop alike, behaviour 9).
  struct SealedChunk { const Column* column; SampleIdx chunkNo; std::uint64_t version; const Chunk* chunk; };
  // Sibling chunks decoded in one sweep (a text slab, 05 §7.1): inserted as one group, members already resident are
  // skipped, the group is evictable only when no member is pinned and is evicted as a unit.
  void        insertGroup(std::span<const SealedChunk>);
};

class Column {                                          // lazily materialised sequence of chunks
public:
  explicit Column(std::shared_ptr<IChunkProvider>);
  SampleIdx     size() const noexcept;
  Dtype         dtype() const noexcept;
  std::uint64_t version() const noexcept;               // provider version
  SampleIdx     chunkCount() const noexcept;
  // Any thread, never blocks (GUI paint arena, summary-path workers): pins the chunk if it is resident and current,
  // else schedules its materialisation at `prio` and returns nullopt (behaviour 6). There is no raw-pointer accessor.
  std::optional<ChunkHandle> tryPin(SampleIdx chunkNo, Priority prio) const;
  // Any thread, never blocks: schedules materialisation of an absent chunk at `prio` without pinning it (04 §4.5).
  void          prefetch(SampleIdx chunkNo, Priority prio) const;
  // Worker threads only (asserted): returns [first,last] (sample indices) resident and pinned; absent chunks are
  // materialised inline on the calling worker (behaviour 7, 01 §5.1 rule W); honours cancel.
  Result<ChunkHandle> require(SampleIdx first, SampleIdx last, Priority prio, base::CancelToken) const;
  double        at(SampleIdx i) const noexcept;         // convenience = tryPin(chunk of i) -> read -> unpin; NaN when absent or invalid
  const SummaryPyramid& summary() const;                // may be incomplete; see complete()
  // Worker: streams the provider chunk by chunk into a scratch ChunkBuilder, folds levels >= 256 into the
  // SummaryPyramid and discards the samples without inserting them into ChunkCache (a resident chunk is read pinned
  // instead). Never fills the cache; cost = one sequential decode of the column.
  Result<void>  buildSummary(base::CancelToken) const;
  bool          chunkIndependent() const noexcept;
};
}
```

Behaviours:

6. `tryPin` coalesces requests: a chunk already scheduled or being built is not scheduled twice; the caller receives `nullopt` and is repainted by the `ChannelReady` completion (01 §5). The pin is an atomic reference-count increment under the cache shard lock, so a chunk read through a handle can never be freed under the reader (01 §5.3 rule 5); there is no `tryChunk` returning `const Chunk*` and no unpinned read.
7. Each chunk slot carries an atomic state `{Absent, Building(owner thread), Sealed}`. `require` walks the span: for an `Absent` chunk it CASes the state to `Building(self)` and materialises the chunk on the calling worker; a chunk-independent provider may first submit the other absent chunks of the span as `chunk/` tasks for parallelism, and the requester runs any of them that are still queued itself (`TaskPool::tryRunInline`). It waits on a chunk's latch only when another thread that is actually running is `Building` it, so every wait makes progress (01 §5.1 rule W). On a provider with `chunkIndependent() == false`, `require` materialises chunks `0..last` in order on the calling worker; the `Building` state of the first absent chunk serialises concurrent requesters, one worker per column at a time.
8. A pinned chunk is never evicted; eviction (M5, D-043) walks the LRU list of unpinned chunks until `residentBytes ≤ budget`. A `Column` whose chunk was evicted simply re-materialises it; `version` is unchanged.
9. `version()` mismatch between a cached chunk and the provider means the chunk is stale; `tryPin`/`require` treat stale as absent. Chunk memory, stale or evicted, is freed only under the cache lock at pin count zero: one free path for eviction and stale drop (01 §5.3 rule 5).
10. `summary()` of logged channels is built by **one** `Background` task per source (`summary/<sourceId>`), started after the source is published, that walks the source's columns chunk-sequentially through `buildSummary` (for `.ld` a sequential read of the mapping; for text sources the shared slab sweep of 05 §7.1 feeds every column at once). It never inserts chunks into `ChunkCache`, so it neither makes the whole file resident (M0–M4 have no eviction, D-043) nor evicts the chunks a visible worksheet uses (M5). Maths columns fill their summary while `PlanProvider` seals chunks (the levels already exist) and `complete()` turns true once every chunk has sealed once or a later `buildSummary` pass has covered the rest. Until `complete()` the autoscale falls back to the resident chunks and the component repaints on completion.
11. `Column` objects are immutable in interface and internally synchronised; a `std::shared_ptr<const DataSource>` snapshot shares `Column`s with later snapshots.

**Staged refinements (not in 0.1).** Three refinements are adopted with their first consumers and are not built in 0.1, where maths evaluates whole nodes (D-139, 03 §6.0): (a) at v1.0, with the chunk-parallel maths evaluator, `IChunkProvider` gains the range form `materialise(ChunkRange, ChunkSink&, CancelToken)`, so one task can seal one chunk (`Parallel`) or many (`Ordered`, `Whole`) (03 §6.2); (b) behaviour 7 then reads: chunks `0..last` for `Ordered` providers, from the nearest checkpoint, and the whole extent for `Whole` providers; (c) at M5, with LRU eviction (D-043), `ChunkCache` eviction groups (all or none) carry an LRU weight equal to the measured recompute cost, so whole-node maths outputs are evicted as one group (03 §6.2); `insertGroup` above already exists for text slabs. 07 §8.1 and §10.1 schedule them.

### 3.3 Series and resampler

```cpp
namespace esses::store {
struct UniformSeries     { Rate rate; Micros t0; Column values;
                           Micros timeAt(SampleIdx i) const { return rate.timeAt(i, t0); }
                           SampleIdx indexAt(Micros t) const { return rate.indexAt(t, t0); }
                           Micros duration() const; };
struct TimestampedSeries { Column timesUs;      // Dtype::I64, exact microseconds (rule 12)
                           Column values; Micros tMin, tMax; };
using Series = std::variant<UniformSeries, TimestampedSeries>;

struct ResampleView {                                   // lazily gathered block at an output rate; no materialised copy
                                                        // (owned here; esses_maths wraps it as MathInputGatherer, 03 §6.3)
  Rate out; Micros t0; SampleIdx first, n; Interp interp;
  float  f32(SampleIdx k) const noexcept;  double f64(SampleIdx k) const noexcept;   // NaN when invalid
  bool   valid(SampleIdx k) const noexcept;
};

class Resampler {                                       // view-layer service; never called by importers (D-009)
public:
  static double       valueAt(const Series&, Micros t, Interp);                       // single value, NaN if invalid
  static Result<ResampleView> block(const Series&, Rate out, Micros t0, SampleIdx first, SampleIdx n, Interp,
                                    const ChunkHandle& pinned);                      // worker: caller pins
  static Interp       defaultFor(SampleKind);                                          // Real→Linear, else Previous
};
}
```

12. `TimestampedSeries::timesUs` is a `Dtype::I64` column holding exact microseconds (D-008 forbids an F64 approximation). `I64` exists only for time columns: it never appears in a `ChannelDescriptor`, carries no pyramid (`builtLevels == 0`, `level()` is empty) and is always `AllValid`. Timestamps are strictly increasing after import de-duplication (equal timestamps: last wins; the importer logs a warning).
13. Resampling policies at output time `t`: `Linear` interpolates between the bracketing samples and is invalid if either is invalid; `Closest` picks the nearer sample, earlier on a tie; `Previous` holds the last sample at or before `t`; `Next` the first at or after. Outside `[tFirst, tLast]` every policy returns invalid.
14. Uniform → uniform at the same rate and phase is an identity gather (zero cost); at different rates the index map is affine on `Rate` cross products; timestamped → uniform uses a monotone cursor advanced across the block after one binary search.
15. Default `Interp` per `SampleKind`: `Real` → `Linear`; `Integer`, `Enum`, `Bool`, `Bitfield`, `Text` → `Previous`. Dictionaries mark gear channels `Previous` by name (`"interp": "Previous"`), as D-009 requires.
16. Property tests: `valueAt` is monotone-consistent with `block`; `Previous`/`Next` boundaries match a brute-force reference on random timestamped data; a uniform series resampled to itself is bit-identical.

---

## 4. `esses_model`

### 4.1 Channel identity and descriptor

```cpp
namespace esses::model {
using SourceId = base::Hash128;                        // quick content hash (05 §1.1: size + first/last MiB + format
                                                       // header); the full-content hash is computed later (D-011, D-015)
struct ChannelId   { base::Uuid v; auto operator<=>(const ChannelId&) const = default; };
struct EnumTableId { std::uint32_t v; };
struct EnumEntry   { std::int32_t value; std::string label;
                     std::uint8_t severity = 0; };     // 0 normal, 1 info, 2 warning, 3 fault (.ld enumeration blocks, 05 §3.9)
struct EnumTable   { EnumTableId id; std::string name; std::vector<EnumEntry> entries; };
struct SourceEncoding {                                // opaque on-disk encoding of one channel, owned by the importer that read it
  virtual ~SourceEncoding() = default;                 // (05 §1.3 IReader::sourceEncoding; .ld: the record of 05 §3.4)
  virtual std::string_view importerId() const = 0;     // a writer uses it only when this matches its own format (05 §5 item 2)
};

enum class SampleKind    : std::uint8_t { Real, Integer, Enum, Bool, Bitfield, Text };
enum class ChannelSource : std::uint8_t { Logged, Maths, Detail, Alias, Mapped, Constant, Setup };

struct ChannelDescriptor {                             // D-013 field set, plus sourceEncoding (2026-10-10)
  ChannelId      id;
  std::string    name;            // canonical analysis name: the lookup key for workbooks, maths, overrides
  std::string    shortName;       // <= 8 chars for gauges; empty = derived from name
  std::string    loggedName;      // as spelled by the file/vendor; equals name unless a dictionary or mapping renamed it
  units::UnitId  unit;            // storage (base) unit of the quantity
  units::QuantityId quantity;
  store::Dtype   dtype;
  SampleKind     kind;
  store::Interp  interp;
  int            dps = 2;         // file default; project override wins at display time
  int            colourIndex = -1;// -1 = auto from scheme palette
  std::optional<EnumTableId> enums;
  ChannelSource  source;
  std::string    statusText;      // "", "Pending", "Error: ..."; mirrored by Channel::status
  std::optional<Rate> rate;       // nullopt for timestamped
  std::shared_ptr<const SourceEncoding> sourceEncoding;   // null unless the importer kept one; shared on copy, never serialised
};

enum class ChannelStatus : std::uint8_t { Ok, Pending, Error };
struct Channel { ChannelDescriptor desc; store::Series series; ChannelStatus status = ChannelStatus::Ok; };
}

template <> struct std::hash<esses::model::ChannelId> {   // so std::unordered_map<ChannelId, …> (DataLayer::byId) compiles
  std::size_t operator()(const esses::model::ChannelId& id) const noexcept {
    std::uint64_t lo, hi;                                  // the 16 bytes folded into two u64s
    std::memcpy(&lo, id.v.b.data(), 8); std::memcpy(&hi, id.v.b.data() + 8, 8);
    return std::size_t(lo ^ (hi * 0x9E3779B97F4A7C15ull));
  }
};
```

`ChannelId` derivation (`uuidFromHash(hash128({...}))`), parts in order:

| `source` | parts |
|---|---|
| `Logged` | `"logged"`, `SourceId` hex, layer name, logged name, importer id |
| `Maths` (global; also built-in and transient modules, e.g. `module:esses.corrected_speed_distance`, `module:~transient`, 03 §8) | `"maths"`, `SourceId` hex, layer name, canonical name, `"module:" + module id` |
| `Maths` (local/sidecar) | `"maths"`, `SourceId` hex, layer name, canonical name, `"local"` |
| `Alias` / `Mapped` / `Detail` / `Constant` / `Setup` | the source kind in lower case, `SourceId` hex, layer name, canonical name, `""` |

The id is therefore stable across sessions without a registry, differs between two files that log the same channel (so cache keys never collide), and is unaffected by display overrides. The **name** remains the key users and documents use (D-013).

**Amended 2026-10-10** (spike S-1 and `docs/formats/ld-converter-port.md` §8 items 8 and 9). `EnumTable` entries carry a `severity`, because `.ld` enumeration blocks store one per value (05 §3.9); it round-trips through the writer and may colour values. `ChannelDescriptor::sourceEncoding`, registered in the outline (D-073) and relied on by 05 §5 item 2 but not declared until now, carries the importer's on-disk encoding of a channel to a writer of the same format, so `.ld` read → write → read is bit-exact. The base type is opaque here; `esses_formats` derives the `.ld` record from it. It never enters `ChannelId`, project documents or the sidecar, and maths, alias and mapped channels leave it null.

### 4.2 Layers, sources, details

```cpp
namespace esses::model {
enum class Provenance : std::uint8_t { Logged, Companion, User, Derived };

struct DateTime { std::int64_t unixUs; std::int32_t tzOffsetMin; std::string iso() const; };
struct Quantityd { double v; units::UnitId unit; int dps = 2; };
using DetailValue = std::variant<std::string, Quantityd, DateTime>;

struct Detail {
  std::string id;                 // "Driver", "Venue", "Vehicle Id", "Diff Ratio", "Log Date", custom
  DetailValue value;
  Provenance  prov;
  std::optional<DetailValue> original;   // the file's value when prov != Logged
};

struct DataLayer {
  std::string name;                                   // "Normal" canonical; "Fastest", "Burst0", "Burst1"
  std::optional<base::UtcInstant> origin;             // absolute time of Micros 0
  Micros duration = 0;
  std::vector<Channel> channels;                      // insertion order = file order; maths/alias/detail channels appended
  std::unordered_map<std::string, std::uint32_t> byNameCI;        // caseFold(name) -> index
  std::unordered_map<std::string, std::uint32_t> byLoggedNameCI;  // caseFold(loggedName) -> index
  std::unordered_map<ChannelId, std::uint32_t>   byId;
  std::vector<Marker>      markers;                   // all classes, all provenances, time-sorted, edits applied
  RangeGroupTree           ranges;                    // output of the rules; rebuilt on marker edits; one version per
                                                      // group path (§4.4, D-140)
  std::vector<EnumTable>   enumTables;
  const Channel* find(std::string_view name) const;   // case-fold on canonical name only (resolver does the rest)
};

struct Provenance_Source { std::filesystem::path path; std::vector<std::filesystem::path> companions;
                           std::string importerId, importerVersion; std::uint64_t size; SourceId hash /*quick*/;
                           std::optional<base::Hash128> contentHash;   // full xxh3-128, filled by a Background task
                           std::int64_t mtimeUnixUs; std::vector<std::string> dialectFlags;
                           JsonDoc probe;                              // probe_json (probe-1): sniff/probe/open entries
                                                                       // only; decode-time entries live in DecodeLog (05 §1.1)
                           std::vector<std::byte> rawHeader; };        // the importer's verbatim header bytes for a writer round
                                                                       // trip (.ld: header and detail records, 0x5DE..0x5E1 held
                                                                       // as 0, D-143; 05 §3.2); empty when none; never serialised

struct DataSource {                                   // published only as std::shared_ptr<const DataSource>
  SourceId id;
  Provenance_Source prov;
  std::vector<Detail> details;                        // merged file ⊕ companion ⊕ sidecar
  std::vector<DataLayer> layers;                      // layers[0].name == "Normal"
  SidecarDocument sidecar;                            // the edits (owned copy of the document state)
  std::vector<std::string> companionOpaque;           // unknown .ldx elements as XML strings (export round-trip)
  std::uint64_t version;                              // bumps on every sidecar commit
  const DataLayer* layer(std::string_view name) const;  // "" accepted as alias of "Normal"
};
}
```

Behaviours:

1. A `DataSource` is immutable once published (D-031). A sidecar edit produces a *new* `DataSource` with `version + 1`: channels and their `Column`s are shared; `details`, `markers` and `ranges` of the affected layer are rebuilt; the old snapshot stays alive while any worker holds it. Only the group paths whose ranges changed bump their `RangeGroupTree` version (§4.4); maths node versions read those per-path versions and never `DataSource::version` (03 §7.2, D-140).
2. Standard detail ids are the i2 union (research `config_model.md`), typed; numeric ones carry a quantity and are stored in base units:

   | Group | Ids (type; quantity where numeric) |
   |---|---|
   | Summary | `Event`, `Venue`, `Venue Category`, `Driver`, `Team`, `Vehicle Id`, `Vehicle Desc`, `Vehicle Number`, `Engine Id`, `Session`, `Short Comment`, `Long Comment` (text); `Venue Length` (Length); `Start Lap` (count); `Log Date`, `Log Time` (date-time) |
   | Statistics (derived, Circuit) | `Fastest Time` (Time), `Fastest Lap`, `Total Laps` (count) |
   | File (derived) | `File Name`, `File Path` (text), `File Size` (count) |
   | Weather | `Sky`, `Weather Comment` (text); `Track Temp`, `Air Temp` (Temperature); `Baro Pressure` (Pressure); `Rel Humidity` (Ratio); `Wind Direction` (Angle); `Wind Speed` (Speed) |
   | Device | `Device Type`, `Device Version`, `Device Serial`, `Device Options` (text) |
   | Vehicle | `Vehicle Type`, `Vehicle Drive Type`, `Vehicle Comment` (text); `Vehicle Weight` (Mass); `Fuel Tank Capacity` (Volume); `Vehicle Track`, `Vehicle Wheelbase` (Length); `Diff Ratio`, `Gear 1` … `Gear 10` (Ratio) |
   | Drag run (profile `drag`, v1.0) | `Run Name`, `Lane` (text); `Dial in Time`, `Reaction Time`, `Time 60 ft` … `Time 1320 ft` (Time); `Speed 660 ft`, `Speed 1320 ft` (Speed) |
   | Esses addition | `Stoich Ratio` (Ratio, default 14.7; §2.2 rule 4) |

   Importers may add vendor ids (custom details); `data/profiles/<id>.profile.json` declares which are shown in the Details editor and their types. `Start Lap`, when present, is the number given to the first flying lap by the beacon-pair rule (§4.4 rule 11).
3. Numeric details are exposed as `ChannelSource::Detail` constant channels in every layer (`'Diff Ratio'[]`, `'Vehicle Wheelbase'[m]` as in i2's shipped Oversteer expression), so default maths reads vehicle geometry from details rather than redefining it as a constant; text details are visible to maths comparisons only. `Fastest Time/Lap/Total Laps` and the File group are *derived* after the rules run and are never written to the sidecar.
4. The layer name `""` is accepted on import as an alias of `"Normal"` and written by the `.ldx` exporter; every in-memory and sidecar key uses `"Normal"` (D-015).

### 4.3 Markers and marker edits

```cpp
namespace esses::model {
enum class MarkerClass : std::uint8_t { SOL, EOL, BCN, SPLTBCN, IGRDBCN, RESET, STGSTRT, STGEND, DeviceEvent };
struct MarkerId { base::Uuid v; auto operator<=>(const MarkerId&) const = default; };

struct Marker {                                       // D-014
  MarkerId      id;
  MarkerClass   cls;
  std::string   group;            // "" intrinsic (SOL/EOL/RESET), "Beacons", "StageMarkers", "DeviceEvents"
  Micros        t;                // effective time (after MarkerEdit offset)
  std::string   name, comment;
  Provenance    prov;             // Logged | Companion | User | Derived
  std::uint32_t flags = 0;        // vendor flags passed through (e.g. companion Flags="77")
  std::optional<Micros> originalT;// set when an offset edit is applied
  bool          enabled = true;   // false after an ignore edit
};

enum class MarkerEditOp : std::uint8_t { Offset, Ignore, Restore, OffsetDistance };
struct MarkerEdit { MarkerId ref;                 // unused (empty) for OffsetDistance, which applies to every BCN of the layer
                    MarkerEditOp op; Micros dtUs = 0;
                    double dM = 0; };             // OffsetDistance only: metres, positive = beacon further along the track

MarkerId markerIdLogged(MarkerClass, Micros loggedT, std::string_view group, std::uint32_t ordinal);
// = uuidFromHash(hash128({"marker", className, decimal(loggedT), group, decimal(ordinal)}))
}
```

5. `ordinal` is the marker's index among markers of the same `(class, group, loggedT)` in file order (0 for the first), so two beacons logged at the same microsecond still get distinct ids. Companion markers use the same derivation (prov differs, id does not), which is what makes the precedence merge in §5.3 deterministic.
6. User markers (`SHIFT+F7`, Lap Editor insert, auto-insert by distance/GPS gate) get `uuidRandom()` and `prov = User`; rule-generated markers (lap-counter edges, GPS gate crossings produced by an importer's `lapSource`) get the logged derivation with `prov = Derived` and `group = "Beacons"`.
7. Edits: `Offset` sets `originalT = loggedT` (if unset) and `t = loggedT + dtUs`; `Ignore` sets `enabled = false`; `Restore` removes every edit for `ref` (it is a command, never a persisted record: the sidecar array holds at most one `offset` and one `ignore` per marker id). Edits that reference an id absent from the merged marker set are kept in the document and reported as orphaned in the Import Log; they are never dropped.
7a. **Beacon offset by distance** (`OffsetDistance`, the Lap Editor's per-source beacon offset, 06 §13.3 item 3): at most one per layer; it moves every `BCN` of the layer to `t' = first time Corr Dist reaches CorrDist(t_logged) + dM`. The resolution needs the `Corr Dist` column, so a worker computes the new times (`ranges/<sourceId>/<layer>/<markerSetHash>` task, §4.4 rule 13) and posts them to the GUI, where the phase-1 range rebuild then runs synchronously as for any marker edit; until it lands (or while `Corr Dist` is Pending) the old laps stay and the Lap Editor shows "pending". A change of the corrected-distance settings re-resolves it. Persisted in `sidecar-1` `markerEdits[]` as `{ "op": "offsetDistance", "dM": { "v": …, "unit": "m" } }` with no `ref` (§5.2).
8. The marker list of a layer is sorted by `(t, cls, id)` after edits; `SOL` is first and `EOL` last by construction (importers synthesise them when the file lacks them). At equal `t` an `EOL` sorts before a `SOL` (an outing that ends where the next one starts), then the other classes in declaration order (D-158, 2026-10-11).

**Device events are markers** (one model; 04 §8.10 draws them): an importer reports each logger/firmware event (power cycle, logger start/stop, Pi `Burst Trigger` rows, later ibt/MDF events) as a `Marker{cls = DeviceEvent, group = "DeviceEvents", name, comment = message}` with `prov = Logged`; `DataLayer` has no separate event list. Because they sit in their own group, range rules (which list `markerGroups`) never see them. `.ldx` marker classes outside the nine listed here are not markers at all: the whole element is kept opaque in `DataSource::companionOpaque` (§5.3 rule 3) and reported once (05 §4).

### 4.4 Ranges, groups, references

```cpp
namespace esses::model {
enum class RangeKind : std::uint8_t { Outing, Lap, Run, Stage, Section, Split, Data };
enum class LapClass  : std::uint8_t { Flying, Out, In, Partial, Pit /*reserved: never assigned by the circuit rule*/, None };
struct RangeId { base::Uuid v; auto operator<=>(const RangeId&) const = default; };

struct Range {                                        // D-014; [start, end)
  RangeId    id;
  RangeKind  kind;
  LapClass   lapClass = LapClass::None;
  MarkerId   startMarker, endMarker;                  // anchor markers (see rule 10)
  Micros     start, end;
  std::string name, abbrev;                           // "Lap 3" / "L3", "Out Lap" / "Out"
  int        number = 0;
  bool       trusted = true, enabled = true;
  std::uint16_t stint = 0;                            // annotation (phase 2, rule 13): 1-based stint index, 0 = unknown
  bool       pitStop = false;                         // annotation (phase 2): lap contains a stationary period; never
                                                      // changes lapClass, number or trusted
  Micros     offsetUs = 0;                            // alignment offset, time domain (sidecar)
  double     offsetM  = 0;                            // alignment offset, distance domain (sidecar)
  std::optional<double> stretch;                      // lap-stretch factor applied by LapStretch (derived, not persisted)
  Micros     duration() const { return end - start; }
};

struct RangeGroup {
  std::string path;                                   // "Outings:Laps:Track Sections:Default"
  std::string name;                                   // last segment
  std::vector<Range> ranges;                          // time-ordered
  std::vector<RangeGroup> children;
  bool supportsTrackGeneration = false, supportsReporting = true;
  std::string detailTemplate;                         // "'Driver' Lap {n}" for Data-window labels
  std::uint64_t version = 0;                          // per group path (D-140): bumps only when this group's ranges change
};
struct RangeGroupTree { std::vector<RangeGroup> roots; const RangeGroup* find(std::string_view path) const;
                        std::uint64_t rangeVersion(std::string_view path) const; };   // 03 §7.2's rangeVersion(g); 0 if absent

struct RangeRef {                                     // what selection and view state hold (D-014)
  SourceId    src;
  std::string layer;
  std::string groupPath;
  RangeId     range;
  std::string name;                                   // fallback key for persistence / .i2wsp import only
};

RangeId rangeId(std::string_view groupPath, MarkerId anchor, std::string_view qualifier = {});
// = uuidFromHash(hash128({"range", groupPath, anchor.v.str(), qualifier}))
}
```

9. **Identity.** `RangeId` is a pure function of `(group path, start-marker id[, qualifier])` (D-014). Inserting, offsetting or ignoring a beacon changes which markers *exist*, not the ids of the ranges that still start at an unchanged marker, so trust flags and alignment offsets in the sidecar survive edits. A lap whose start beacon is ignored disappears together with its overrides (they remain in the document, unapplied, and reappear on restore).
10. **Anchors for derived ranges.** Marker-bounded kinds (`Outing`, `Lap`, `Run`, `Stage`, `Split`) have `start == t(startMarker)` and `end == t(endMarker)`, qualifier empty. Derived kinds (`Section`, `Data`) copy `startMarker/endMarker` from the enclosing marker-bounded ancestor and set `qualifier` to the section name (`"Turn 3"`), so a section's id is stable under beacon edits of *other* laps and changes only when its own lap's start beacon changes.
11. **Numbering (circuit).** Flying laps are numbered `s…s+n−1` in time order per outing, where `s` is the numeric `Start Lap` detail when present (i2 updates it automatically) and 1 otherwise; `Out` has `number = 0`; `In` has `number = n + 1`; `Partial` takes the number the flying lap would have had and does not consume it (the next flying lap gets the same number + 1 sequence continues from the last *flying* number). `name` is produced from the rule's templates (`"Lap {n}"`, `"Out Lap"`, `"In Lap"`, `"Lap {n} (partial)"`), `abbrev` likewise (`"L{n}"`, `"Out"`, `"In"`, `"P{n}"`). Observation on an i2 install (D-005 §3) may adjust the In/Partial numbering; the templates make that a data change.
12. **Trust.** Default `trusted` comes from the rule's class table only: `out` and `in` are untrusted (Laps help: every lap is trusted except the in and out laps) and `partial` is untrusted (the shipped i2 circuit lap script marks the lap after a reset untrusted); every other lap, including one that contains a pit stop, is trusted. The stint and `pitStop` annotations never change it. The sidecar override replaces the default. `enabled = false` ranges are hidden from the Data window and reports but keep their ids.
13. **Rebuild, in two phases.** Phase 1 runs synchronously on the GUI thread whenever a layer's marker set or details change (D-031; O(markers), budget < 5 ms for 500 markers): marker transitions → `Outings`, `Outings:Laps` and the detail groups (ids, kinds, classes, numbers, default trust), then `rangeOverrides` re-applied by id, then by `name` for entries whose id did not match (import fallback), with an Import Log line per fallback hit. It reads markers and details only, never a sample. Phase 2 is a worker task keyed `ranges/<sourceId>/<layer>/<markerSetHash>` at `Interactive` priority (any older `ranges/<sourceId>/<layer>/` task is cancelled first): it pins the columns it needs (`counterChannel`, the stationary-speed channel, `Corr Dist`) through `Column::require`, computes stint and `pitStop` annotations and the track-section groups, and publishes a new `DataSource` snapshot through `Dispatcher`; the GUI discards the result when the layer's current `markerSetHash` differs. Because `RangeId`s depend only on markers (rule 9), phase 2 never renames a range and overrides keep matching by id. Until phase 2 lands, section bands and section reports show Pending. At open, phase 1 runs inside the open task (05 §2 step 6) and phase 2 is submitted right after publication, so first paint never waits for `Corr Dist`.
14. `RangeRef` equality for selection purposes is `(src, layer, groupPath, range)`; `name` is ignored.

### 4.5 Declarative range rules (`data/rules/*.rule.json`, schema `rule-1`)

Rules are data consumed by `esses_analysis::IRangeRule` implementations keyed by `kind`; this document fixes the file format so the profile can reference rules by id.

```json
{ "$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "circuit.beacon-pair", "kind": "beacon-pair",
  "produces": "Outings:Laps", "parent": "Outings", "layers": ["Normal", "Fastest"],
  "markerGroups": ["", "Beacons"], "ignoreClasses": ["IGRDBCN", "SPLTBCN"],
  "transitions": [
    { "from": "SOL",   "to": "BCN", "class": "out" },
    { "from": "BCN",   "to": "BCN", "class": "flying" },
    { "from": "BCN",   "to": "EOL", "class": "in" },
    { "from": "SOL",   "to": "EOL", "class": "out" },
    { "from": "*",     "to": "RESET", "class": "partial" },
    { "from": "RESET", "to": "BCN", "class": "out" } ],
  "classes": {
    "flying":  { "name": "Lap {n}", "abbrev": "L{n}", "trusted": true,  "numbered": true },
    "out":     { "name": "Out Lap", "abbrev": "Out",  "trusted": false, "number": 0 },
    "in":      { "name": "In Lap",  "abbrev": "In",   "trusted": false, "number": "n+1" },
    "partial": { "name": "Lap {n} (partial)", "abbrev": "P{n}", "trusted": false, "numbered": false } },
  "numbering": { "startDetail": "Start Lap", "default": 1 },
  "annotations": {
    "enabled": true, "phase": 2,
    "stint": { "counterChannel": "Lap Number", "dataGapUs": 10000000 },
    "pitStop": { "stationarySec": 15, "speedChannel": "Corr Speed", "stationaryBelow": { "v": 1.0, "unit": "km/h" } } },
  "groupFlags": { "supportsTrackGeneration": true, "supportsReporting": true } }
```

```json
{ "$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "circuit.whole-outing", "kind": "whole-outing",
  "produces": "Outings", "layers": ["*"], "name": "'File Name'", "abbrev": "{outingIndex}", "trusted": true,
  "groupFlags": { "supportsTrackGeneration": false, "supportsReporting": true } }
```

```json
{ "$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "circuit.track-sections", "kind": "track-sections", "phase": 2,
  "produces": "Outings:Laps:Track Sections:{set}", "categoryChild": "{set}:{categoryGroup}", "parent": "Outings:Laps",
  "sets": { "from": "trackMap", "always": ["Default"] },
  "categoryGroups": { "corner": "Corners", "straight": "Straights", "*": "{category}" },
  "requires": { "trackMap": true, "channel": "Corr Dist" },
  "groupFlags": { "supportsTrackGeneration": false, "supportsReporting": true } }
```

```json
{ "$schema": "urn:esses:rule-1", "schemaVersion": 1, "id": "circuit.detail-groups", "kind": "detail-group", "phase": 1,
  "layers": ["*"], "anchor": "SOL", "sortDetails": ["Log Date", "Log Time"],
  "groups": [ { "produces": "Events",         "name": "'Event'",          "missing": "(no event)" },
              { "produces": "Vehicles",       "name": "'Vehicle Id'",     "missing": "(no vehicle)" },
              { "produces": "Sessions",       "name": "'Session'",        "missing": "(no session)" },
              { "produces": "Driver",         "name": "'Driver'",         "missing": "(no driver)" },
              { "produces": "Vehicle Id",     "name": "'Vehicle Id'",     "missing": "(none)" },
              { "produces": "Vehicle Number", "name": "'Vehicle Number'", "missing": "(none)" } ],
  "groupFlags": { "supportsTrackGeneration": false, "supportsReporting": true } }
```

Semantics fixed here: `transitions` are evaluated over the time-sorted, enabled markers of the listed groups; `*` matches any class; a `RESET` closes the open range as `partial` and the next range starts at the `RESET` marker. Phase-1 rules (beacon-pair transitions and numbering, whole-outing, detail-group) read markers and details only; phase-2 work (beacon-pair `annotations`, track-sections) reads pinned columns on a worker (§4.4 rule 13).

- **Annotations (beacon-pair, phase 2, default on, D-014).** A stint boundary is a reset of `counterChannel` (its value decreases), a `RESET` marker, or a hole in the *logged data* longer than `dataGapUs` (two consecutive timestamps of the layer's timestamped series, or of a logger pause recorded by the importer, more than `dataGapUs` apart); the beacon-to-beacon interval is never a criterion, so ordinary laps never split stints. A lap that contains `≥ stationarySec` of `speedChannel` below `stationaryBelow` gets `pitStop = true` and the next lap starts a new stint. Both are attributes for Data-window filters, reports and the Lap Editor; they never change `lapClass`, `number` or `trusted` (§4.4 rule 12). The thresholds come from motorsport-telemetry-rs, not from i2; `annotations.enabled: false` turns them off. `LapClass::Pit` stays in the enum (D-014) for profiles that classify pit laps explicitly; the shipped circuit rule never assigns it (proposed in DECISIONS.md Addendum A).
- **Track sections (phase 2).** For every section set `S` of the venue's track map (`Default`, always present once default sections exist, plus every user-named set), the rule emits group `Outings:Laps:Track Sections:S` holding all sections of `S` for every lap, and under it one child group per category present in `S`: `corner` → `…:S:Corners`, `straight` → `…:S:Straights`, any user category `c` → `…:S:c` (e.g. `Fast`, `Slow`). So `range_is("Outings:Laps:Track Sections:Default:Corners")` from the Maths Functions help resolves (case file `tests/maths/range_is_track_sections_default_corners.case.json`, owned by 03 §14.1). Each section's `[start,end)` comes from the lap's `DistanceIndex` (04 §3.1, D-016), which is keyed by `(Corr Dist column version, lap start/end marker ids and times, stretch settings)` and never by `DataSource::version` (D-140, 01 §7.1). A track-section rebuild bumps only the versions of the `Outings:Laps:Track Sections:S` group paths (§4.4), never that of `Outings:Laps`, so `Corr Dist` and its consumers are not invalidated by the sections built from it. A lap that does not start at a `BCN` (an out lap `SOL→BCN`, or a partial lap after a `RESET`) is positioned backwards from its end beacon: a section at track distance `s` starts at lap distance `lapLength − (trackLength − s)`, and sections that would start before the lap start are omitted (04 §13 item 5, §14 item 5).
- **Detail groups (phase 1).** Each listed group gets, per layer, one range spanning `[SOL, EOL)` (anchor `SOL`, so its `RangeId` is stable) named by the detail template, `missing` when the detail is empty. Consumers that span several open sources (Data-window views `Events › Vehicles › Sessions › Outings › Laps`, channel-report headings such as `Event, Sessions, Outings, Laps, Corners`) merge ranges of equal name across sources and order them by `Log Date`/`Log Time`; a detail edit reruns phase 1 so names follow the edit.

### 4.6 `ChannelResolver`

```cpp
namespace esses::model {
struct ResolutionStep { enum class Kind : std::uint8_t { Dictionary, Mapping, Alias, CaseFold, LoggedName, Missing } kind;
                        std::string from, to, note; };
struct ResolutionTrace { std::string requested; std::vector<ResolutionStep> steps; std::optional<ChannelId> result;
                         std::string summary() const; };   // "'Wheel Speed' → alias → 'Wheel Speed FL' (km/h)"

// Qt-free POD inputs owned by esses_model (layer 1). esses_project's Mappings / Aliases documents (layer 2) convert
// to these on load and on every edit; esses_model never includes an esses_project header (01 §2.2 rule 1).
struct MappingRow { std::string from, to; std::optional<units::UnitId> unit; bool unitlessOnly = false; };
struct MappingTable { std::vector<MappingRow> rows; };                 // matched by caseFold(from)
struct AliasCandidate { std::string name; bool enabled = true; };
struct AliasRow { std::string name; std::vector<AliasCandidate> candidates; int colourIndex = -1; std::optional<int> dps; };
struct AliasTable { std::vector<AliasRow> rows; };                     // matched by caseFold(name)

class ChannelResolver {
public:
  ChannelResolver(MappingTable, AliasTable);
  struct Resolution { const Channel* channel; ResolutionTrace trace; };
  Resolution resolve(std::string_view name, const DataLayer&) const;
private:
  // order (D-013): Mappings (from→to) → Aliases (ordered candidates, enabled only; a candidate may name another alias)
  //                → byNameCI → byLoggedNameCI → Missing
};
}
```

15. Dictionaries (`data/dictionaries/<importerStem>.json`, schema `dictionary-1`, 05 §8.3) are applied by the importer *before* the layer is built: they set `name`, `unit`, `quantity`, `interp`, `kind`, `enums`, and expand bitfield groups into `Bool` channels; the original spelling stays in `loggedName`. The resolver records a `Dictionary` step only when `name != loggedName`, by reading the descriptor.
16. Mappings rename a logged name to an analysis name case-insensitively; the renamed channel's `loggedName` is the `from`, `name` the `to`, `source = Mapped` only when the unit was stamped (otherwise `Logged`). A lookup of the *old* name resolves through a `Mapping` step; the Channels window does not list it (i2 rule).
17. Aliases: `name` matches an alias → candidates are tried in order, skipping disabled ones. Each candidate is resolved through the whole resolver **except the alias being resolved**, so a candidate spelled like its own alias reaches the logged or maths channel of that name, and a candidate that names another alias follows that alias (i2 allows alias-to-alias settings; its release notes fix a crash on circular ones). The resolver keeps the chain of aliases being expanded; revisiting one stops with `Error: alias cycle 'A' → 'B' → 'A'` on the requesting alias (no loop, no crash). A candidate that resolves to a channel whose status is `Error` (for example a maths channel that failed) is skipped and the next candidate is tried (`W-ALIAS-FALLBACK`, i2 release-note behaviour); `Missing` candidates are skipped likewise. The resolved channel is wrapped in an `Alias` descriptor that carries the alias's own `colourIndex` and `dps`, so colours are consistent across outings (D-013). The step order stays D-013's (aliases before the canonical-name lookup), as 03 §3.3 also states.
18. Workbook references that end in `Missing` are kept and rendered as "(missing)"; the trace is visible in Channel Status (CTRL+U) for every reference of the active worksheet.

The `dictionary-1` format is owned by 05 §8.3; this excerpt uses its keys:

```json
{ "$schema": "urn:esses:dictionary-1", "schemaVersion": 1, "importer": "motec.ld", "version": "0.1.0",
  "entries": [
    { "match": { "vendorId": "13" },     "name": "Eng Oil Temp", "quantity": "Temperature", "interp": "Linear" },
    { "match": { "name": "WheelSP_FL" }, "name": "Wheel Speed FL", "unit": "km/h" },
    { "match": { "nameRegex": "^Gear$" },"kind": "Integer", "interp": "Previous" },
    { "match": { "name": "Lap Beacon" }, "lapSource": { "kind": "beacon_event" } },
    { "match": { "vendorId": "920" },    "name": "ECU Error Group 3", "kind": "Bitfield" } ],
  "bitfields": [
    { "match": { "name": "ECU Error Group 3" }, "device": { "type": "M800", "minVersion": 300, "maxVersion": 399 },
      "bits": [ { "mask": "0x0001", "name": "Error Throttle Pos" }, { "mask": "0x0002", "name": "Error Manifold Pres" } ] } ] }
```

---

## 5. Sidecar document (`<log>.<ext>.sidecar.json`, schema `sidecar-1`)

### 5.1 Type

```cpp
namespace esses::model {
struct Annotation { base::Uuid id; Micros tUs; std::string text; std::optional<std::string> channel; };
struct VideoLink  { std::filesystem::path file; std::string cameraView; Micros offsetUs;
                    std::vector<std::pair<Micros, Micros>> syncPoints; };   // (log t, media pts)
struct RangeOverride { std::string groupPath; std::optional<MarkerId> startMarker; std::string name;   // name = fallback key
                       std::optional<bool> trusted; std::optional<Micros> offsetUs; std::optional<double> offsetM; };

struct SidecarLayer {
  std::vector<Detail>        details;       // prov == User only
  std::vector<Marker>        markers;       // prov == User only
  std::vector<MarkerEdit>    markerEdits;
  std::vector<RangeOverride> rangeOverrides;
  std::optional<JsonDoc>     localMaths;    // a complete maths-1 module (id "local")
  std::vector<Annotation>    annotations;
  std::vector<VideoLink>     videoLinks;
  JsonDoc runs, stages;                     // reserved; preserved verbatim in MVP
};

class SidecarDocument {
public:
  static std::filesystem::path pathFor(const std::filesystem::path& log);  // "<log>.<ext>.sidecar.json"
  static Result<SidecarDocument> load(const std::filesystem::path&);        // missing file → empty document, ok
  Result<void> save(const std::filesystem::path&) const;                    // canonical JSON, atomic (§6.5)
  void   apply(DataLayer&) const;                                           // details, markers, edits, overrides
  const SidecarLayer& layer(std::string_view name) const; SidecarLayer& layer(std::string_view name);
  bool   empty() const;                                                     // true → the file is deleted on save
  std::string sourcePathRel; std::uint64_t sourceSize; SourceId sourceHash /*quick*/;
  std::optional<base::Hash128> sourceContentHash;     // full hash, written once the Background task has produced it
  std::string importerId, importerVersion;
  DateTime edited;
};
}
```

### 5.2 Example

```json
{ "$schema": "urn:esses:sidecar-1", "schemaVersion": 1,
  "source": { "path": "bathurst_r1.ld", "size": 48213344, "hash": "xxh3:9f3c1b2a0d4e5f60718293a4b5c6d7e8",
              "contentHash": "xxh3:41d07a9e3c2b1f0e5d6c7b8a99a8b7c6",
              "importer": { "id": "motec.ld", "version": "0.1.0" }, "companions": ["bathurst_r1.ldx"] },
  "edited": "2026-10-09T21:14:00+10:00",
  "layers": {
    "Normal": {
      "details": [
        { "id": "Driver", "value": { "t": "string", "v": "M. Minton" }, "original": { "t": "string", "v": "MMINTON" } },
        { "id": "Diff Ratio", "value": { "t": "number", "v": 3.9, "unit": "ratio", "dps": 2 } },
        { "id": "Stoich Ratio", "value": { "t": "number", "v": 14.7, "unit": "ratio", "dps": 1 } } ],
      "markers": [
        { "id": "3f6a2c1e-8d4b-4f0a-9c1d-2b7e5a9f0c11", "class": "BCN", "group": "Beacons", "tUs": 412356789,
          "name": "Manual.1", "comment": "missed beacon T1", "prov": "User" } ],
      "markerEdits": [
        { "ref": "7c0e9a2b-1d3f-5a6c-8e9f-0a1b2c3d4e5f", "op": "offset", "dtUs": -120000 },
        { "ref": "9b1d2e3f-4a5b-5c6d-7e8f-9a0b1c2d3e4f", "op": "ignore" },
        { "op": "offsetDistance", "dM": { "v": 12.5, "unit": "m" } } ],
      "rangeOverrides": [
        { "group": "Outings:Laps", "startMarker": "7c0e9a2b-1d3f-5a6c-8e9f-0a1b2c3d4e5f", "name": "Lap 3", "trusted": false },
        { "group": "Outings:Laps", "startMarker": "a1b2c3d4-e5f6-5a7b-8c9d-0e1f2a3b4c5d", "name": "Lap 7",
          "offsetUs": 350000, "offsetM": 0 } ],
      "localMaths": { "$schema": "urn:esses:maths-1", "schemaVersion": 1, "id": "local", "enabled": true, "precision": "single",
        "constants": [], "enumerations": [],
        "items": [ { "kind": "scale_offset", "name": "Brake Pres F", "enabled": true, "scale": 1.02,
                     "offset": { "v": 0, "unit": "bar" }, "display": { "unit": "bar", "dps": 1 } } ] },
      "annotations": [ { "id": "0d9e8f7a-6b5c-4d3e-2f1a-0b9c8d7e6f5a", "tUs": 456000000, "text": "Wheel lock T4", "channel": "Brake Pres F" } ],
      "videoLinks": [ { "file": "cam1.mp4", "cameraView": "Driver", "offsetUs": -1250000, "syncPoints": [ [0, 1250000] ] } ],
      "runs": [], "stages": [] } } }
```

### 5.3 Behaviours

1. **Location and name.** Next to the log, `<file>.<ext>.sidecar.json`; `source.path` is relative to the sidecar's directory. A sidecar whose `source.hash` (the quick hash, 05 §1.1) matches the opened file is applied regardless of `path` (file and sidecar moved together). A quick-hash mismatch, or a later mismatch between `source.contentHash` and the full hash the Background task computes, applies the document with an Import Log warning `sidecar.source_changed`; orphaned marker edits and overrides are reported, not dropped. `contentHash` is added on the first save after the full hash exists.
2. **Merge precedence** on every load: *sidecar > companion (`.ldx`) > file* (D-015). Details: the highest-precedence value wins per id, lower ones become `original`. Markers: the union keyed by `MarkerId`; identical ids from file and companion collapse to one marker with `prov = Companion`; `markerEdits` are then applied; sidecar `markers` are appended. Local maths: only the sidecar's; a companion's log-file maths is imported into `localMaths` once (first load with no sidecar) and reported.
3. **Unknown `.ldx` elements** are preserved as opaque XML strings on the `DataSource` (`companionOpaque`) and emitted unchanged by the `.ldx` export writer; they never enter the sidecar. The `.ldx` writer is export-only (v1.0) and writes layer key `""` for `Normal`.
4. **Write policy.** The sidecar is written atomically (§6.5) by `ProjectSession` after each committed sidecar command, coalesced within 500 ms, and on source close; it is not journaled (it lives beside the log, outside the project). An empty document deletes the file. The sidecar never contains transient (filter) maths; scale/offset/zero from a channel label become `localMaths` `scale_offset` items with the channel's DPS preserved (D-015).
5. **Keys.** `rangeOverrides` entries carry both `startMarker` and `name`; `apply` matches by `(group, startMarker)` first, then `(group, name)`, and rewrites the entry with the resolved id on the next save so an imported `.i2wsp` override (`source::layer::group::name`, layer `""` ↔ `Normal`) heals itself.
6. **Annotations** are absolute Main-source times (`tUs` since layer origin), never range-relative, so they survive beacon edits.
7. **Alignment offsets** (`offsetUs`, `offsetM`) are per-range facts about a file and live only here (D-015/D-016); pressing O again zeroes them, which removes the keys.
8. `markers-1` (fixture `expected.markers.json`) is the array form of the `markers` record above with `prov` of every provenance; the same serializer is used. One marker key vocabulary serves `sidecar-1`, `markers-1` and `probe-1` (05 §1.2): `class`, `group`, `tUs`, `name`, `comment`, `flags`, plus `id`/`prov` where the document carries them and `ordinal` in `probe-1`.

---

## 6. `esses_project`: the project folder

### 6.1 Layout (D-019)

```
<Project>/                                  name = folder name
  project.json                              project-1
  channels/overrides.json                   overrides-1
  channels/aliases.json                     aliases-1
  channels/mappings.json                    mappings-1
  channels/unitmaps/<importerStem>.json     unitmap-1     (stem = importer id without "text.profile:", since ':' is
                                                          not allowed in Windows file names: generic.csv.json, motec.ld.json)
  maths/<module>.maths.json                 maths-1       (order from project.json "mathsOrder")
  workbooks/<name>.workbook.json            workbook-1
  trackmaps/<venue>.track.json              track-1
  backup/<file>.<yyyymmdd-hhmmss>.json      previous copy of each file rewritten by an explicit Save (last 5 kept)
  .local/layout.json                        layout-1      per-user; excluded from archives and version control
  .local/settings.json                      per-user project settings (recent sources, window geometry)
  .journal/<seq>.json                       journal-1     write-ahead autosave; empty after an explicit Save
Per user (not project):  <config>/ (01 §9): schemes/*.scheme.json (scheme-1), keymap.json (keymap-1),
                         settings.json (settings-1), profiles-text/*.json (importer-profile-1)
Shipped (repo data/):    units.json, profiles/*.profile.json (profile-1), rules/*.rule.json (rule-1),
                         dictionaries/<importerStem>.json (dictionary-1), profiles-text/<profileId>.json
                         (importer-profile-1), schemes/*.scheme.json, keymaps/default.json,
                         workbooks/circuit-default.workbook.json
```

```cpp
namespace esses::project {
struct SchemaRef { std::string id; int version; };     // "sidecar-1" → {"sidecar", 1}
class SchemaRegistry {
public:
  static SchemaRegistry& instance();                   // schemas/*.schema.json compiled in
  Result<void>  validate(const JsonDoc&, std::string_view schemaId) const;
  Result<SchemaRef> schemaOf(const JsonDoc&) const;    // from "$schema" + "schemaVersion"
};
class Migrations {
public:
  using Fn = std::function<Result<JsonDoc>(JsonDoc)>;
  void          add(std::string_view schema, int fromVersion, Fn);   // registered at static init per document type
  Result<JsonDoc> upgrade(JsonDoc, std::string_view schema, int toVersion, std::vector<std::string>& log) const;
};
template <class T> struct Document {                   // every file type is a Document<T>
  static Result<T>   load(const std::filesystem::path&, std::vector<std::string>& log);  // validate → migrate → parse
  static JsonDoc     toJson(const T&);                 // members in canonical key order (§6.5 rule 1)
  static Result<void> save(const T&, const std::filesystem::path&);                      // canonical bytes, atomic
  JsonDoc            unknown;                          // unrecognised top-level members, preserved (re-emitted sorted)
};
}
```

### 6.2 `project.json` (`project-1`)

```json
{ "$schema": "urn:esses:project-1", "schemaVersion": 1, "name": "Bathurst 2026", "profile": "circuit",
  "options": {
    "general": { "saveOnExit": true, "autosaveMinutes": 2, "loadPreviousData": true, "selectFastestOnLoad": false,
                 "looseChannelSearch": false, "autoShowMessagesOnError": true, "reduceCpuDuringAnimation": false },
    "autoscale": { "includeUntrusted": false, "includeOverlays": true },
    "timeFormat": "m:ss.fff", "lapTimeFormat": "m:ss.fff", "distanceUnit": "km",
    "fastLapTolerancePct": 1.0, "autoCentreCursor": false, "animation": { "rate": 1.0 },
    "lapStretch": { "enabled": true, "tolerancePct": 5.0 },
    "trackMaps": { "autoGenerateOnOpen": true },
    "maths": { "precision": "single", "compat": { "unaryMinusAbovePower": false } },
    "reports": { "includeUntrusted": false, "probeSec": 5 } },
  "ui": { "layoutLocked": false },
  "mathsOrder": ["esses-circuit", "user"],
  "builtinMaths": {
    "esses.corrected_speed_distance": { "version": 1, "enabled": true,
      "settings": { "method": "fastestWheel", "fastestWheel": { "channels": ["Wheel Speed FL", "Wheel Speed FR"] } } } },
  "workbooks": ["General", "Chassis"],
  "recentSources": [ { "path": "../logs/bathurst_r1.ld", "importer": "motec.ld", "hash": "xxh3:9f3c…", "openedAt": "2026-10-09T20:50:00+10:00" } ],
  "lastSelection": {
    "main":     { "source": { "path": "../logs/bathurst_r1.ld", "hash": "xxh3:9f3c…" }, "layer": "Normal",
                  "groupPath": "Outings:Laps", "startMarker": "7c0e9a2b-1d3f-5a6c-8e9f-0a1b2c3d4e5f", "name": "Lap 7" },
    "overlay0": { "source": { "path": "../logs/bathurst_r1.ld", "hash": "xxh3:9f3c…" }, "layer": "Normal",
                  "groupPath": "Outings:Laps", "startMarker": "a1b2c3d4-e5f6-5a7b-8c9d-0e1f2a3b4c5d", "name": "Lap 5" },
    "overlays": [], "overlaysHidden": false, "singleColourMain": false },
  "dataWindow": {
    "filters": { "default": { "kind": "all" }, "xxh3:9f3c…": { "kind": "lapNumbers", "laps": "3,5-7" } },
    "applyToAllSources": false } }
```

Complete `project-1` key set (every other key is an unknown member, preserved):

| Key | Type / values | Owner of the behaviour |
|---|---|---|
| `name`, `profile` | string; profile id (`circuit`; `drag`, `rally` at v1.0) | D-036 |
| `options.general.*` | `saveOnExit` bool, `autosaveMinutes` int (0 = off, default 2), `loadPreviousData`, `selectFastestOnLoad`, `looseChannelSearch`, `autoShowMessagesOnError`, `reduceCpuDuringAnimation` bool | 06 §13.7 General |
| `options.autoscale.{includeUntrusted, includeOverlays}` | bool | 04 §8.3 |
| `options.timeFormat`, `options.lapTimeFormat` | `m:ss.fff` \| `ss.fff` \| `h:mm:ss.f` | 06 §13.7 Analysis |
| `options.distanceUnit` | unit symbol of the Length quantity | 04 status bar, axes |
| `options.fastLapTolerancePct` | number, default 1.0 | 06 §5.2 CTRL+ALT+F |
| `options.autoCentreCursor` | bool | 04 §8.5 |
| `options.animation.rate` | number, 0.25 … 4 | 04 §8.5 |
| `options.lapStretch.{enabled, tolerancePct}` | bool, number (default 5) | D-016 |
| `options.trackMaps.autoGenerateOnOpen` | bool | 06 §13.5 |
| `options.maths.precision` | `single` \| `double` (the `maths-1` vocabulary, 03 §4.1); default `single` | D-022, 03 |
| `options.maths.compat.unaryMinusAbovePower` | bool | D-020 |
| `options.reports.{includeUntrusted, probeSec}` | bool, number | 04 §12 |
| `ui.layoutLocked` | bool | 06 §4.2 Lock Layout |
| `mathsOrder` | module ids, top to bottom | 03 §4.3 |
| `builtinMaths.<moduleId>` | `{version, enabled, settings}`; `settings` validated by `schemas/builtin-<id>-<version>.schema.json` | 03 §9 (the Corrected Speed/Distance dialog writes it) |
| `workbooks` | workbook names in display order | 06 §2 |
| `recentSources[]` | `{path, importer, hash, openedAt}` | 06 §18 |
| `lastSelection` | `{main, overlay0?, overlays[], overlaysHidden, singleColourMain}` of `RangeKey` | 06 §5.5 |
| `dataWindow.filters` | map `SourceId` hex or `default` → filter; `kind ∈ all \| selected \| fastest \| fastest3 \| withinOnePercent \| lapNumbers` (D-017's list), `laps` string (`"3"`, `"3-7"`, `"3,5,9"`) for `lapNumbers` | 06 §5.4 |
| `dataWindow.applyToAllSources` | bool | 06 §5.4 |

`RangeKey { source: {path, hash}, layer, groupPath, startMarker, name }` is the persisted form of `RangeRef` (D-014 identity `(groupPath, startMarker)`); restore resolves by `(hash, layer, groupPath, startMarker)`, then by `(path, layer, groupPath, name)` (06 §5.5).

### 6.3 Channel documents

```json
{ "$schema": "urn:esses:overrides-1", "schemaVersion": 1,
  "channels": {
    "Engine RPM":  { "displayUnit": "rpm", "dps": 0, "colourIndex": 2, "autoScale": false,
                     "scaleMin": { "v": 0, "unit": "rpm" }, "scaleMax": { "v": 8000, "unit": "rpm" },
                     "interp": "Linear", "warn": { "max": { "v": 7800, "unit": "rpm" } }, "fault": { "max": { "v": 8200, "unit": "rpm" } } },
    "Throttle Pos": { "displayUnit": "%", "dps": 1, "scaleMin": { "v": 0, "unit": "%" }, "scaleMax": { "v": 100, "unit": "%" } },
    "Gear":         { "interp": "Previous", "dps": 0 } } }
```

```json
{ "$schema": "urn:esses:aliases-1", "schemaVersion": 1,
  "aliases": [ { "name": "Wheel Speed", "candidates": ["Wheel Speed FL", "Wheel Speed FR", "Ground Speed"],
                 "enabled": [true, true, false], "colourIndex": 4, "dps": 1 } ] }
```

```json
{ "$schema": "urn:esses:mappings-1", "schemaVersion": 1,
  "mappings": [ { "from": "WheelSP_FL", "to": "Wheel Speed FL", "unit": "km/h", "unitlessOnly": true },
                { "from": "GPS Latitudine", "to": "GPS Latitude" } ] }
```

```json
{ "$schema": "urn:esses:unitmap-1", "schemaVersion": 1, "importer": "text.profile:generic.csv",
  "rules": [ { "match": "nameAndUnit", "name": "Speed", "vendorUnit": "MPH", "quantity": "Speed", "unit": "mph", "analysisName": "GPS Speed" },
             { "match": "unitOnly", "vendorUnit": "kpa", "quantity": "Pressure", "unit": "kPa a" },
             { "match": "nameOnly", "name": "Lambda1", "quantity": "Air Fuel Ratio", "unit": "lambda", "analysisName": "Lambda 1" } ] }
```

The `unitmap-1` format is owned by 05 §8.2 (`vendorUnit` = the file's spelling, `unit` = the target unit). `overrides-1` stays a map keyed by channel name (above); editors that present it as rows (06 §13.1) convert on load and save. Loader rules: every `{v, unit}` is converted to base on read through `UnitRegistry` and re-emitted in the unit it was written in (the unit symbol is kept on the in-memory override so a round trip is byte-identical); an override whose `displayUnit` belongs to a different family than the channel's quantity in *any* open source is dropped for that session with a Messages line (i2 rule, D-010); unknown channels in `overrides` are kept untouched.

### 6.4 Workbook (`workbook-1`)

```cpp
namespace esses::project {
struct Rect { double l, t, r, b; };                  // fractions 0..1 of the worksheet; overlap allowed
struct Links { bool zoom = true, cursor = true, datum = true; };
struct Caption { bool show = true; bool autoText = true; std::string text; };   // JSON {show, auto, text} (04 §7.3)
struct Legend  { bool channels = true; bool colour = false; };
struct ComponentSpec {                               // the canonical header is 04 §7.3
  base::Uuid id; std::string type; int version; Rect rect; int z = 0; Links links;
  Caption caption; Legend legend; std::string activeChannel;
  JsonDoc body;                                      // type-specific; validated by schemas/components/<type>-<version>.schema.json
  JsonDoc xI2;                                       // JSON key "x-i2": i2's ModeId and attributes imported but not implemented
  bool known = true;                                 // false → preserved verbatim, placeholder rendering
};
struct WorksheetDef { base::Uuid id; std::string name; std::string linkTo; std::string domain /*time|distance*/;
                      std::array<int,2> grid{100,100}; std::vector<ComponentSpec> components; std::optional<std::string> hotkey; };
struct WorkbookFile { base::Uuid id;                // assigned at creation; key of the layout-1 perspective (D-019)
                     std::string name; std::string profile;
                     std::optional<std::string> activeWorksheet;    // used only when .local/layout.json has no entry (06 §4.1)
                     std::vector<WorksheetDef> worksheets; JsonDoc xI2; };
}
```

```json
{ "$schema": "urn:esses:workbook-1", "schemaVersion": 1, "id": "b7c1d2e3-4f5a-4b6c-8d7e-9f0a1b2c3d4e", "name": "General",
  "profile": "circuit", "activeWorksheet": "Overlay",
  "worksheets": [ {
    "id": "6e1f…", "name": "Overlay", "linkTo": "Laps", "domain": "distance", "grid": [100, 100],
    "components": [
      { "id": "c1a0…", "type": "time_distance", "version": 1, "rect": [0.0, 0.0, 0.75, 1.0], "z": 0,
        "caption": { "show": true, "auto": true, "text": "" },
        "links": { "zoom": true, "cursor": true, "datum": true },
        "legend": { "channels": true, "colour": false }, "activeChannel": "Corr Speed",
        "body": {
          "display": { "traceStyle": "line", "lineWidth": 1, "showGrid": true, "showMarkers": true, "showBands": true,
                       "bandMode": "sections", "bandGroup": "Outings:Laps:Track Sections:Default", "overlapMode": "panels",
                       "cursorStyle": "line", "cursorSnap": true,
                       "key": { "mode": "labels", "pos": "left", "values": true, "stats": false },
                       "variance": { "show": true, "heightFrac": 0.15, "autoScale": true,
                                     "min": { "v": -2, "unit": "s" }, "max": { "v": 2, "unit": "s" } } },
          "groups": [
            { "name": "Speed", "scaleMode": "channel", "edges": [0.0, 0.33], "lo": 0.0, "hi": 0.33,
              "lines": { "zero": { "enabled": true }, "min": { "enabled": false }, "max": { "enabled": false } },
              "traces": [ { "channel": "Corr Speed", "visible": true, "displayUnit": "km/h", "dps": 1 },
                          { "channel": "GPS Speed", "visible": true } ] },
            { "name": "Pedals", "scaleMode": "manual", "edges": [0.33, 0.66],
              "scaleMin": { "v": 0, "unit": "%" }, "scaleMax": { "v": 100, "unit": "%" },
              "traces": [ { "channel": "Throttle Pos" }, { "channel": "Brake Pres F", "displayUnit": "bar a" } ] } ] },
        "x-i2": { "ModeId": "0", "unmapped": { "TOD.Format.Date": "dd/MM/yyyy" } } },
      { "id": "c2b1…", "type": "histogram", "version": 1, "rect": [0.75, 0.0, 1.0, 0.5], "z": 1,
        "caption": { "show": true, "auto": true, "text": "" },
        "links": { "zoom": true, "cursor": false, "datum": false },
        "body": { "traces": [ { "channel": "Damper Vel FL", "visible": true } ],
                  "bins": { "mode": "even", "bandMode": "width", "dataMin": { "v": -250, "unit": "mm/s" },
                            "dataMax": { "v": 250, "unit": "mm/s" }, "bandWidth": { "v": 5, "unit": "mm/s" } },
                  "yMode": "percent", "style": "line", "layout": "overlapped", "excludeUntrusted": true,
                  "gating": { "channel": "Cornering Gate" }, "colourChannel": { "channel": "", "axis": null } } },
      { "id": "c3c2…", "type": "mixture_map", "version": 1, "rect": [0.75, 0.5, 1.0, 1.0], "z": 2,
        "body": { "x": "Engine RPM", "y": "Lambda 1", "lambdaDelayUs": 150000 } } ] } ],
  "x-i2": { "Version": "200" } }
```

Rules: `id` is a random UUID set when the workbook is created; a file without `id` (hand-written, or older) gets `uuidFromHash(hash128({"workbook", fileName}))` on load and is marked dirty, so its perspective key is stable; renaming the file keeps the `id`. `rect` fractions convert losslessly from i2 v101 `RectGridPts` (÷ `GridSize`) and v200 `PosPC` (÷ 1000); `z` is defined as in 04 §7.3 (ascending paint order; send-to-background and bring-to-front renormalise the worksheet's `z` values to `0…n−1` in the same command); `caption`, `legend` and `activeChannel` follow the 04 §7.3 header, and i2's `ModeId` lives in `x-i2`; channel references are **names**; the third component above has a `type` not implemented in 0.1 and is preserved byte-for-byte (its `body` is never re-serialised from a typed struct) and rendered as a placeholder; shared sub-schemas `BandAxis` (`{mode: even | arbitrary, bandMode: width | count, dataMin, dataMax, bandWidth | bandCount, palette, …, bands[]}`, definition only, never generated bands), `ReportSpec` (`{scope, headings[], rows, channels[{name, stats[]}], summaries[], transposed}`) and `ColourBands` (`[{min, max, open, colour, visible}]`) live in `schemas/common-1.schema.json` with the field sets of 04 §7.3. Component headers and bodies are owned by 04_RENDERING_AND_COMPONENTS.md (§7.3 header, §8.14–§17 bodies); this document fixes the envelope.

### 6.5 Canonical form, atomic writes, round-trip rule

1. **Canonical JSON** (one algorithm, `JsonDoc::canonical()`, used by every document kind, `probe_json`, fixtures and CLI goldens; 05 §1.2 and 08 §6.3 refer here): UTF-8 without BOM, LF, two-space indent, one space after `:` and `,`; object members **sorted by key in UTF-8 code-unit order**, except that `$schema` then `schemaVersion` come first when present (known and unknown members are sorted alike, so no schema metadata is needed to serialise); arrays keep their order, one element per line when any element is an object; numbers in shortest round-trip form (`std::to_chars`), integers without a decimal point, `-0` written as `0`; non-finite numbers are not representable: serialising one is an error and a schema that needs an "invalid" value uses `null`; trailing newline. Examples in the design documents are written for reading, not in canonical order.
2. **Atomic write**: serialise to memory → write `<file>.tmp-<pid>-<rand>` in the same directory → `fsync` → rename over the target (`MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows, `rename(2)` elsewhere) → `fsync` the directory where supported. A reader never sees a partial file.
3. **Round-trip rule** (test per file kind, D-019/D-033): `save(load(f)) == f` byte-for-byte when `f` is canonical and current-version; for any valid `f`, `save(load(save(load(f))))) == save(load(f))`; unknown members, unknown component types and `x-i2` bags survive both.
4. Content hashes of documents (journal, dirty tracking) are xxh3-128 of the canonical bytes.

### 6.6 Migration policy

1. **One `$schema` form for every Esses JSON file** (project documents, sidecar, `probe-1`, `fixture-1`, `importer-profile-1`, `dictionary-1`, `unitmap-1`, `plugin-1`, `layout-1`, `settings-1`, `maths-case-1`, …): `"$schema": "urn:esses:<name>-<major>"` plus an integer `"schemaVersion"` (the minor version within the major), the form D-072 and the outline conventions fix until a project domain exists. On load `SchemaRegistry` also accepts the later `https://<project domain>/…/<name>-<major>[.schema].json` form and the bare `esses:<name>-<major>` spelling of earlier drafts, and normalises to the `urn:esses:` form on save.
2. Loader: parse → read `$schema`/`schemaVersion` → refuse a major newer than supported (`fail(ErrorCode::Unsupported)` with `context.schemaId`, the file is not opened) → run registered migrations `(schema, from) → from+1` in sequence up to the current minor → validate against the current schema → parse to the typed document. A migrated document is marked dirty; the original file is copied to `backup/` before the first overwrite.
3. Migrations are pure JSON → JSON functions with a unit test per step and never delete information: fields that no longer exist move into `x-esses-legacy`.
4. A missing `$schema` is treated as the oldest known version of the file's kind (determined by path) and logged.
5. `esses-cli validate <file|dir>` runs steps 1–2 without writing and prints the migration log; CI runs it on `data/`, `fixtures/` and every example in `docs/`.

### 6.7 Per-user layout (`.local/layout.json`, `layout-1`)

The shape is the one 06 §3.4 owns (dock ids and state names from 06 §3.2):

```json
{ "$schema": "urn:esses:layout-1", "schemaVersion": 1,
  "appVersion": "0.1.0", "qtVersion": "6.7.2", "adsVersion": "4.3.1",
  "window": { "geometry": "<base64 QMainWindow::saveGeometry>", "maximised": true,
              "screen": { "name": "\\\\.\\DISPLAY1", "dpr": 1.5 } },
  "active": { "workbook": "b7c1d2e3-4f5a-4b6c-8d7e-9f0a1b2c3d4e", "worksheet": "Overlay" },
  "perspectives": {
    "_default": { "ads": "<base64 DockHost::savePerspective>", "savedAt": "2026-10-09T18:22:04Z" },
    "b7c1d2e3-4f5a-4b6c-8d7e-9f0a1b2c3d4e": { "ads": "<base64>", "savedAt": "2026-10-09T18:30:11Z" } },
  "docks": { "trackmap": { "state": "AutoHide" }, "messages": { "state": "Closed" } },
  "dialogs": { "lapEditor": { "geometry": "<base64>" }, "mathsEditor": { "geometry": "<base64>", "splitter": [220, 640] } } }
```

Perspectives and `active.workbook` are keyed by `WorkbookFile::id` (D-019), never by name, so renaming a workbook keeps its layout. Dock ids are `data`, `channels`, `values`, `trackmap`, `channelstatus`, `messages`; states are `Docked`, `Floating`, `AutoHide`, `Closed`. Never shared, never archived, never undoable; written atomically on workbook switch, Reset Dock Windows, project close and exit, never by autosave; a missing or invalid file resets docks to the default perspective. Zoom, cursor and datum are not persisted (view state is transient; the worksheet opens at the Main range, W).

### 6.8 Write-ahead journal (`.journal/<seq>.json`, `journal-1`)

```json
{ "$schema": "urn:esses:journal-1", "schemaVersion": 1, "seq": 17, "at": "2026-10-09T21:16:00+10:00", "session": "b7e3…",
  "entries": [
    { "path": "workbooks/General.workbook.json", "baseHash": "xxh3:…", "hash": "xxh3:…", "document": { "...full canonical document..." : 0 } },
    { "path": "channels/overrides.json", "baseHash": "xxh3:…", "hash": "xxh3:…", "document": { } },
    { "path": "maths/user.maths.json", "baseHash": null, "hash": "xxh3:…", "document": { } } ] }
```

1. Every 2 minutes (project option) and on explicit request, `ProjectSession` appends one journal file containing the **full canonical document** of every dirty project file (documents are small; whole copies make replay trivial and diffable). Project files are not rewritten by autosave.
2. Explicit Save (CTRL+S, on-exit prompt): write every dirty document atomically, copy the previous version of each rewritten file to `backup/`, then delete `.journal/*` — in that order, so a crash mid-save leaves a consistent recoverable state.
3. On open, a non-empty `.journal/` offers recovery: the newest entry per `path` is applied in memory (checking `baseHash` against the on-disk file to detect edits made by another instance) and the project is marked dirty; "discard" deletes the journal.
4. `seq` is monotonic per session; files from a different `session` id are also replayed (ordered by `seq`, then `at`).
5. Maths modules additionally save atomically when the Maths Editor closes (i2 behaviour); schemes save on OK.

### 6.9 Remaining documents

**Maths module (`maths/<module>.maths.json`, `maths-1`)** — the document is owned by 03 §4.1 (items and kinds in 03 §4.1/§4.4); this excerpt uses its keys (`resultUnit`, `display{unit, dps, colourIndex}`, `rate: "auto"`, object enumeration entries, precision `single | double`):

```json
{ "$schema": "urn:esses:maths-1", "schemaVersion": 1, "id": "user", "enabled": true,
  "condition": "'Vehicle Id' == \"GT3-07\"", "precision": "single",
  "constants": [ { "name": "Track Length", "value": { "v": 6213, "unit": "m" } } ],
  "enumerations": [ { "name": "Gear Names", "entries": [ { "name": "N", "value": 0 }, { "name": "1st", "value": 1 }, { "name": "2nd", "value": 2 } ] } ],
  "items": [ { "kind": "expression", "name": "AFR", "enabled": true, "expr": "'Lambda 1' * 'Stoich Ratio'",
               "rate": "auto", "quantity": "Ratio", "resultUnit": "ratio",
               "display": { "unit": "ratio", "dps": 2, "colourIndex": 0 }, "interp": "Linear" } ] }
```

**Scheme (`~/.config/esses/schemes/<name>.scheme.json`, `scheme-1`)**:

```json
{ "$schema": "urn:esses:scheme-1", "schemaVersion": 1, "name": "Esses Dark", "background": "dark",
  "roles": { "cptBackground": "#141517", "cptForeground": "#e6e6e6", "dataGrid": "#33363a", "cursorMain": "#ff3b30", "cursorDatum": "#ffd60a",
             "cursorZoom": "#4c9aff", "rangeMark": "#6e7681", "sectionCorner": "#5a3d7a", "sectionStraight": "#2f5f3a",
             "markerBand": "#2b2d31", "annotationMark": "#ffb000", "mainSingleColour": "#ffffff", "overlay0": "#bdbdbd",
             "pendingHatch": "#3a3d41", "trackLine": "#8e8e93" },
  "channelPalette": ["#ff5252", "#40c4ff", "#69f0ae", "#ffd740", "#ea80fc", "#ffab40", "#18ffff", "#b2ff59", "#ff80ab", "#8c9eff", "#ccff90", "#ffe57f"],
  "overlayPalette": ["#4fc3f7", "#aed581", "#ffb74d", "#ba68c8", "#4db6ac", "#f06292", "#fff176", "#90a4ae", "#a1887f"],
  "overlayStyle": { "colourSource": "channel", "dash": "solid", "lightness": 0.0, "widthScale": 1.0 },
  "fonts": { "axis":        { "family": "Noto Sans", "pointSize": 8, "weight": 400, "italic": false },
             "caption":     { "family": "Noto Sans", "pointSize": 9, "weight": 600, "italic": false },
             "key":         { "family": "Noto Sans", "pointSize": 8, "weight": 400, "italic": false },
             "reportTable": { "family": "Noto Sans", "pointSize": 9, "weight": 400, "italic": false } },
  "print": { "roles": { "cptBackground": "#ffffff", "cptForeground": "#000000" } } }
```

This is an excerpt; the canonical `scheme-1` shape, the complete role list (`dataGrid`, `rangeMark`, `sectionCorner`, `sectionStraight`, `annotationMark`, `pendingHatch`, `trackLine`, …) and every font role are those of 06 §16. `cursorMain` is the red Main key colour (06 §5.3 item 3). `print` overrides any subset of `roles`/palettes for printing and clipboard export; `overlayStyle` is the D-017 hook (`colourSource: channel | overlayPalette`, `dash`, `lightness`, `widthScale`). The shipped files are `data/schemes/light.scheme.json` and `data/schemes/dark.scheme.json` (08 §1).

**Keymap (`keymap-1`)** — the canonical shape is 06 §14.3; this is a user file. Rows are `{command, keys, context, args?}` with snake_case command ids (06 §14.4) and contexts `global | worksheet | component | graph | summary | track | data_window | channels_window | values_window`; the shipped `data/keymaps/default.json` additionally carries `status`, `milestone` and `doc` on every row. F9 is a Global-context binding (06 §3.3):

```json
{ "$schema": "urn:esses:keymap-1", "schemaVersion": 1, "name": "user", "inherits": "default",
  "bindings": [ { "command": "data.toggle_domain",   "keys": ["F9"],               "context": "global" },
                { "command": "data.select_main",     "keys": ["Return", "Enter"],  "context": "data_window" },
                { "command": "cursor.insert_marker", "keys": ["Shift+F7"],         "context": "graph" },
                { "command": "edit.undo",            "keys": ["Ctrl+Z"],           "context": "global" } ],
  "unbind": [ { "command": "file.print", "context": "global" } ] }
```

**Per-user settings (`<config>/settings.json`, `settings-1`; D-055)** — owned by this document; its content is the union of 01 §9 and 06 §18. Per user, never in a project, written atomically on change (coalesced 1 s):

```json
{ "$schema": "urn:esses:settings-1", "schemaVersion": 1,
  "memory": { "chunkCacheBudgetBytes": 4294967296 },
  "timeFormat": "m:ss.fff",
  "scheme": { "active": "Esses Dark", "followOs": true },
  "keymap": "user", "language": "en",
  "mru": { "projects": ["C:/Users/me/Documents/esses/Projects/Bathurst 2026"], "logs": ["D:/Logs/bathurst_r1.ld"] },
  "openDialog": { "lastFolder": "D:/Logs", "paneState": "<base64>" },
  "importerOverrides": [ { "extension": ".csv", "fingerprint": "xxh3:5e1f…", "importer": "text.profile:efi-ascii" } ],
  "ui": { "toolbarVisible": true, "statusBarVisible": true, "doNotShowAgain": [] },
  "paletteHistory": ["#ff5252", "#40c4ff"],
  "commandPalette": { "recent": ["zoom.full", "data.select_main"] },
  "plugins": { "disabled": [] },
  "window": { "geometry": "<base64 geometry of the no-project window>" } }
```

Keys: `memory.chunkCacheBudgetBytes` (default 25 % of RAM, minimum 1 GiB; 01 §7.2), `timeFormat` (default for new projects; `project.json` `options.timeFormat` wins), `scheme.{active, followOs}`, `keymap`, `language`, `mru.projects` (10) and `mru.logs` (20), `openDialog.{lastFolder, paneState}`, `importerOverrides[]` keyed by `(extension, preamble fingerprint)` (01 §4.1 step 1), `ui.{toolbarVisible, statusBarVisible, doNotShowAgain[]}`, `paletteHistory`, `commandPalette.recent` (last 20 ids, 06 §15), `plugins.disabled` (05 §10.4), `window.geometry` for the no-project state. Window geometry and dock state of an open project live in `.local/layout.json` (§6.7); unknown members are preserved.

**Profile (`data/profiles/circuit.profile.json`, `profile-1`)**:

```json
{ "$schema": "urn:esses:profile-1", "schemaVersion": 1, "id": "circuit", "name": "Circuit",
  "rangeKinds": ["Outing", "Lap", "Section"], "primaryRanges": "Outings:Laps", "primaryMarkers": "Beacons",
  "rules": ["circuit.detail-groups", "circuit.whole-outing", "circuit.beacon-pair", "circuit.track-sections"],
  "dataViews": [ { "title": "Laps", "default": true, "tree": ["Events", "Vehicles", "Sessions", "Outings", "Outings:Laps"], "outingDetail": "'File Name'" },
                 { "title": "Outings", "tree": ["Events", "Vehicles", "Sessions", "Outings"] } ],
  "reportHeadings": ["Events", "Vehicles", "Sessions", "Driver", "Vehicle Id", "Vehicle Number", "Outings", "Outings:Laps",
                     "Outings:Laps:Track Sections:Default", "Outings:Laps:Track Sections:Default:Corners",
                     "Outings:Laps:Track Sections:Default:Straights"],
  "defaultMaths": ["esses-circuit"], "defaultWorkbook": "circuit-default",
  "details": [ { "id": "Driver", "type": "string" }, { "id": "Venue", "type": "string" },
               { "id": "Venue Length", "type": "number", "unit": "km", "dps": 3 }, { "id": "Start Lap", "type": "number", "dps": 0 },
               { "id": "Vehicle Wheelbase", "type": "number", "unit": "m", "dps": 3 }, { "id": "Vehicle Track", "type": "number", "unit": "m", "dps": 3 },
               { "id": "Vehicle Weight", "type": "number", "unit": "kg", "dps": 0 }, { "id": "Diff Ratio", "type": "number", "unit": "ratio", "dps": 2 },
               { "id": "Stoich Ratio", "type": "number", "unit": "ratio", "dps": 1, "default": 14.7 } ],
  "terms": { "range": "Lap", "ranges": "Laps", "editor": "Lap Editor" } }
```

**Track map (`trackmaps/<venue>.track.json`, `track-1`; owned by `esses_analysis::TrackMap`, wrapped by `TrackMapFile`)**:

```json
{ "$schema": "urn:esses:track-1", "schemaVersion": 1, "name": "Mount Panorama", "venue": "Mount Panorama",
  "origin": { "lat": -33.4479, "lon": 149.5520, "alt": 862 }, "rotationDeg": 0, "direction": "cw", "closed": true,
  "length": { "v": 6213, "unit": "m" }, "generatedFrom": { "method": "gps", "source": "xxh3:9f3c…", "lap": "5d2c…" },
  "lines": { "centre": [[0, 0, 0], [12.4, 3.1, 12.8]], "inner": [], "outer": [] },
  "sections": [ { "id": "t1", "set": "Default", "name": "Hell Corner", "startDist": { "v": 180, "unit": "m" }, "endDist": { "v": 260, "unit": "m" }, "category": "corner", "userDefined": false },
                { "id": "u1", "set": "Driver Notes", "name": "Conrod fast part", "startDist": { "v": 4300, "unit": "m" }, "endDist": { "v": 5100, "unit": "m" }, "category": "Fast", "userDefined": true } ] }
```

Coordinates are ENU metres about `origin`; `centre` rows are `[x, y, cumDist]`; sections must be ≥ 10 m. `set` names the section set (`Default` for generated Turns/Straights; user sets may overlap other sets) and `category` is `corner`, `straight` or any user category; together they produce the `Outings:Laps:Track Sections:<set>[:<category group>]` groups of §4.5.

---

## 7. Undo/redo coverage (D-032)

One `QUndoStack` per `ProjectSession` (`esses_qtmodel`); every command wraps a Qt-free document mutation from this document and republishes the affected snapshot. Nothing in `esses_project`/`esses_model` knows about undo.

| Document / object | Undoable commands | Not undoable |
|---|---|---|
| `WorkbookFile` | add/remove/move/resize/z-order component; property edits (`body`, `links`, caption); worksheet CRUD, rename, reorder, `linkTo`/`domain`; workbook CRUD | active worksheet/workbook, dock layout |
| `ChannelOverrides` / `Aliases` / `Mappings` / unit maps | set/clear override field; alias CRUD and candidate order/enable; mapping CRUD | — |
| Maths modules | item/constant/enumeration CRUD, enable, condition, module order | evaluation state |
| `SidecarDocument` | detail edit; user marker insert/delete/move; `MarkerEdit` offset/ignore/restore; range override trust/offset; annotation CRUD; video link edit; local maths items | — |
| `TrackMapFile` | section CRUD, rename, category; regenerate (as one command) | — |
| `Scheme` (per user) | role/palette/font edits | scheme selection |
| `ProjectFile` options | autoscale flags, time formats, stretch tolerance, compat flags, report options | selection, filters, MRU |
| View state | — | selection, cursor, datum, zoom, domain, offsets axis on/off, file open/close |

Rules: a sidecar command's `redo()/undo()` both end with `ProjectSession::republish(source)` (new `DataSource` version, synchronous phase-1 range rebuild, phase-2 task submitted (§4.4 rule 13), debounced sidecar write); commands coalesce by `id()` for slider-style edits (Apply in a modeless dialog pushes one command per Apply); the stack is cleared when the project closes; CLI builds have no stack.

---

## 8. Acceptance criteria

1. `Rate` property test: for 10⁶ random `(num, den, i)` with `num, den ≤ 2³² − 1`, rate ≤ 1 MHz and `0 ≤ i ≤ 2⁴⁰`, `indexAt(timeAt(i)) == i`, `indexAt(timeAt(i+1) − 1) == i` and no intermediate overflows (checked against a big-integer reference); fixed cases `{512,25}`, `{30,1}`, `{1000000,33333}` and `{100,3}` at `i ∈ {0, 1, 2, 10⁹}`; `t < t0` returns negative indices with floor semantics; `{100,3}` over 24 h drifts 0 µs; `Rate{1,2} < Rate{1,1}` and `Rate{2,2} == Rate{1,1}`. The same tests run on MSVC x64 (intrinsics path) and GCC/Clang (`__int128` path).
2. `UnitRegistry`: loads `data/units.json` with no duplicate symbol or alias across quantities; every unit spelling used by the shipped dictionaries, text profiles, default maths and maths case files resolves; delta/absolute conversions refused; AFR units resolve to lambda. A private, uncommitted check (U-13) confirms that the spellings in the user's own i2 units file resolve as symbol or `compat` alias.
3. `Chunk`: `layoutBytes(65536, F32, true) == 262144 + 8192 + 174760` (+ alignment); `seal()` on NaN-sprinkled data downgrades to `Bitmap`; `MinMaxPyramid::query` equals brute force on 10⁴ random spans; `SummaryPyramid::minmax` equals brute force at bucket boundaries (F32 columns; for F64 and I32 columns both enclose brute force within the directional rounding of §3.1).
4. `ChunkCache`: pinned chunks survive `setBudgetBytes(0)` + eviction pass (M5, D-043); `Stats.overBudget` raised in M0–M4 without eviction; TSan clean on concurrent `require`/`tryPin`/`prefetch` of the same column, with chunk memory freed only under the cache lock at pin count zero; an `insertGroup` group is evicted as a unit and never while a member is pinned. `buildSummary` over a whole source leaves `Stats.residentBytes` unchanged and produces a `SummaryPyramid` equal to one built from resident chunks.
4a. `MappedFile`: a writer replacing the file by rename-over succeeds while the view keeps the old bytes; an in-place truncating writer gets a sharing error; a fault injected inside `guarded()` returns `Io` and the chunk is sealed `AllInvalid`; `changedOnDisk()` flips after a replace.
5. `Resampler`: boundary and monotonicity property tests (§3.3 rule 16) pass for all four policies.
6. `ChannelId`/`MarkerId`/`RangeId` derivations are reproduced bit-exactly by `esses-cli channels --json` / `laps --json` on three OSes for every fixture (golden JSON).
7. Beacon insert/offset/ignore on a 500-marker layer rebuilds `Outings:Laps` (phase 1, GUI thread, no sample read, asserted by a counting provider) in < 5 ms; trust flags and offsets keyed by `(group, startMarker)` survive insertion before, inside and after the lap (M2 acceptance); a phase-2 result for a stale `markerSetHash` is discarded; a lap containing a 20 s stop keeps `trusted = true`, its class and its number and gets `pitStop = true`; stints do not split on ordinary beacon intervals; `Start Lap = 5` numbers the first flying lap 5.
7a. Range groups: a fixture with a track map yields `Outings:Laps:Track Sections:Default`, `…:Default:Corners`, `…:Default:Straights` and a user set's category children; `range_is("Outings:Laps:Track Sections:Default:Corners")` resolves; the detail-group rule yields `Events`, `Vehicles`, `Sessions`, `Driver`, `Vehicle Id`, `Vehicle Number` ranges named from details, and two open sources with the same `Event` merge under one heading in report order by `Log Date`/`Log Time`.
8. `ChannelResolver`: trace strings for mapping, alias, alias→alias, case-fold, logged-name and missing paths match goldens; an alias candidate spelled like the alias reaches the underlying channel; a cycle `A → B → A` yields `Error: alias cycle` without looping; a candidate with status `Error` is skipped for the next one; `esses_model` builds and links without `esses_project`.
9. Sidecar: `load(save(doc))` byte-identical; merge precedence tests (file-only, companion-only, both, conflicting detail) produce the expected `prov`/`original`; orphaned edits preserved across save.
10. Every document kind in §6 passes the round-trip rule (§6.5.3), validates with `esses-cli validate`, and survives a registered no-op migration without byte change; a `workbook-1` with an unknown component type round-trips verbatim.
11. Journal: kill −9 during autosave leaves either the previous journal set or a complete new entry (temp+rename); recovery replays the newest entry per path and reports `baseHash` conflicts.
12. Undo: every row of §7 has a Qt Test that performs the command, undoes, redoes, and compares the canonical document bytes at each step.

---

## 9. Proposed decisions raised by this document (for DECISIONS.md Addendum A)

1. **D-008 `indexAt`.** D-008's literal `floor((t − t0)·num / (1'000'000·den))` is not the inverse of its rounded `timeAt` for non-integer periods (`{512,25}`, `{30,1}`); §1 behaviour 1 replaces it with the exact inverse and fixes 128-bit, floor-toward-−∞ arithmetic and value ordering of `Rate`.
2. **D-014 pit/stint.** Stint and pit classification become phase-2 annotations (`Range::stint`, `Range::pitStop`) that never change class, number or trust; `LapClass::Pit` stays reserved; default trust is in/out/partial untrusted, everything else trusted (§4.4 rules 12–13, §4.5).
3. **D-014 groups.** `track-sections` produces `…:Track Sections:<set>` plus per-category children (`…:Default:Corners`); a `detail-group` rule kind produces `Events`, `Vehicles`, `Sessions`, `Driver`, `Vehicle Id`, `Vehicle Number` (§4.5).
4. **D-031 lap rebuild.** "Synchronous on the GUI thread" applies to phase 1 (markers and details only); phase 2 is a worker task (§4.4 rule 13).
5. **D-013 aliases.** Order unchanged (mappings → aliases → canonical → logged); alias→alias chains allowed with cycle detection and fall-through on `Error`/`Missing` candidates (§4.6 rule 17).
6. **D-011/D-013/D-015 source identity.** `SourceId` is the quick content hash of 05 §1.1; the full content hash is a Background provenance field stored in the sidecar (§4.2, §5.3 rule 1).
7. **D-019 / outline conventions.** One `$schema` form `urn:esses:<name>-<major>` + `schemaVersion` for every file (D-072), and one canonical JSON algorithm (sorted keys with `$schema`/`schemaVersion` first, no non-finite numbers) (§6.5, §6.6).
8. **D-010 units table.** Sized by need from independent physical definitions; i2 spellings flagged `compat` (§2.3).
