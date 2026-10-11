# 04 — Rendering and components: `esses_plotcore`, `esses_qtrender`, component behaviour

**Scope.** This document specifies the Qt-free plot core (`esses_plotcore`: `ViewState`, `ChangeMask`, `SelectionSnapshot`, `DomainMapper`, `TraceBuilder`/`M4Column`, layout builders, `HitTester`, the `DrawList` IR and `IRenderBackend`), the Qt Gui renderer (`esses_qtrender`: `RasterBackend`, `LayerCache`, `OffscreenRenderer`, `SchemeFonts`), the GL escalation rule, HiDPI rules and golden-image testing, and then the behaviour contract of every MVP component (`time_distance`, `outing`, `scatter`, `histogram`, `channel_report`, `time_report`, `track_position`, `values_list`, `gauge_numeric`, `gauge_bar`) plus design notes for the v1.0/later components. Behaviour text expands `background/design-panel/parity-first.md` §5.2–§5.10 and §7.5; the data path follows `DECISIONS.md`; the i2 feature inventory (`background/research/features_compact.md`, `features_components.md`) is ground truth. Shell, docks, dialogs, keymap binding, the store, analysis algorithms and persistence schemas are specified in their own documents and are only referenced here.

**Related decisions.** D-007 (Qt-free line), D-009 (validity), D-010 (units), D-011 (chunks and pyramid), D-012 (exact M4), D-013 (channel names), D-014 (ranges), D-015 (sidecar), D-016 (distance model, F9), D-017 (selection and colours), D-018 (view state and linking), D-019 (workbook persistence), D-022 (maths pending state), D-023 (variance), D-025 (plot engine), D-027 (component set, properties UI), D-031 (threading), D-032 (undo), D-033 (golden images), D-035 (MVP scope).

---

## 1. Ownership and threading recap

1. `esses_plotcore` contains no Qt and no text rasterisation. It produces `DrawList`s whose text is UTF-8 *runs* with a style id; glyphs exist only in `esses_qtrender` (D-007).
2. Builders in `esses_plotcore` run on `TaskPool` workers at the priorities of 01 §5.2, the single priority table: trace and geometry builds, summary components' geometry included, at `Priority::Visible`; report cells at `Priority::Background`. Widgets never submit to `TaskPool` themselves (06 §1): they go through the `esses_qtmodel` facade `RenderService::submitBuild(key, priority, build, onGui)`, which treats the key as a replace-key (01 §5.4 rule 2: at most one running and one pending build per key; a new submit replaces the pending one and never cancels the running one) and delivers the result through `Dispatcher::post(context, …)`; `OffscreenRenderer` (no widgets) uses `TaskPool` directly (§6.7). Builders read only immutable snapshots: `std::shared_ptr<const DataSource>`, `ChunkHandle`-pinned chunks, copied `ViewState`, `SelectionSnapshot`, `ComponentSpec`, `std::shared_ptr<const ITextMetrics>` (D-031).
3. The GUI thread owns the Static `QImage`, every widget and the `LayerCache`; it builds the Dynamic and Chrome `DrawList`s synchronously and paints them directly onto the widget (§6.2), and rasterises Static layers from worker-built `DrawList`s delivered by `Dispatcher::post`.
4. The GUI thread never waits on a `Future`; a component whose Static build is in flight keeps showing its previous Static layer with a pending stripe in Chrome, and repaints on completion. "GUI thread" in blocking rules means a thread carrying the `esses::ThreadRole::NoBlock` flag (a `thread_local` set only by `esses_app` on its GUI thread); the debug assertion on `Future::get`/`Column::require` checks `ThreadRole::isNoBlock()`, so `esses-render` and the golden tests may block on their calling thread (§6.7).
5. Text measurement needed for layout (key column width, tick-label collision, caption eliding) goes through `ITextMetrics` (§5.4), passed to builders as `std::shared_ptr<const ITextMetrics>` captured by the task (so a deleted component never leaves a dangling reference). Implementations must be safe for concurrent `const` calls from any number of workers: the Qt implementation `QtTextMetrics` holds only immutable font descriptions and builds `QFont`/`QFontMetricsF` per thread (§5.4); tests use a fixed-advance implementation.

---

## 2. View state (`esses_plotcore`)

### 2.1 `ViewState`

```cpp
namespace esses::plot {

enum class Domain : std::uint8_t { Time, Distance };

// Half-open visible span [a,b) in domain units: microseconds (Time) or metres (Distance),
// measured from the Main range start. The span may start before 0 or end after |Main|: it ranges over the
// Main source's whole outing (DomainMapper::outingExtent(), §3.1). Alignment offsets are NOT folded into the span.
struct XSpan { double a = 0.0, b = 0.0; double length() const { return b - a; } };

struct ViewState {
    Domain                 domain     = Domain::Time;  // per worksheet (F9)
    XSpan                  zoom;                       // workspace-shared; W sets {0, |Main|}, F2 the outing extent
    Micros                 cursorT    = 0;             // workspace-shared; µs since Main.start (negative or ≥ |Main| in another lap of the outing)
    std::optional<Micros>  datumT;                     // workspace-shared; same base as cursorT
    bool                   offsetAxis = false;         // per worksheet (O)
    std::uint64_t          version    = 0;             // bumped on every change
};
}
```

Behaviours:

1. `cursorT` and `datumT` are stored relative to the Main range start ("Main-lap time", D-018). Absolute Main-source time is `main.start + cursorT`. When Main changes (N/P, ENTER, Q) the cursor and datum keep their position **in the worksheet's current domain**: in time mode the relative times are kept; in distance mode they are converted through the old and new Main mappers (alignment offsets excluded) so the cursor stays at the same lap distance: `x = mapOld.xOf(oldMain.start + cursorT)`, `cursorT' = mapNew.timeAt(x) − newMain.start` (same for `datumT`; i2 links the datum by distance). Both are then clamped to the new Main source's outing extent, not to the Main range (the cursor may sit in another lap; `W` pulls it back, §8.6). This is what makes N/P preserve the cursor position at an apex in either domain.
2. `zoom` is stored in the domain it was last set in (`zoomDomain`); `WorkspaceViewModel` additionally keeps `zoomFrac {fa, fb}` (fractions of the Main extent in that domain, < 0 or > 1 outside the Main range), recomputed on every zoom change, for the F9 fallback rule (§3.4).
3. **One shared view per workspace, domain per worksheet** (i2: one current zoom level, shown in the zoom summary bar above all worksheets; only the time/distance mode is remembered per worksheet). `zoom`, `zoomFrac`, `cursorT` and `datumT` live in the single `WorkspaceViewModel` (`esses_qtmodel`, beside `DataSelectionModel`); `domain` and `offsetAxis` live in each `WorksheetViewModel`. Each worksheet's `ViewState` is composed from both; when the active worksheet's `domain` differs from `zoomDomain`, the shared zoom is converted by the §3.4 rule on worksheet switch (cursor and datum are times and need no conversion). This amends D-018's "one per worksheet" (proposed, DECISIONS.md Addendum A); whether i2 also keeps cursor and datum across worksheet switches is a D-005 (3) verification item. `ViewState` remains a value type; workers receive copies.
4. `version` is compared on completion: a worker result built for an older `version` is discarded unless its `ChangeMask` subset still matches the current state (the Static layer of a cursor-only change is reusable).

### 2.2 `ChangeMask`

```cpp
enum class Change : std::uint16_t {
    Cursor = 1, Datum = 2, Zoom = 4, Domain = 8, Selection = 16,
    Offsets = 32, Data = 64, Scale = 128, Size = 256, Scheme = 512 };
using ChangeMask = std::uint16_t;   // OR of Change bits
constexpr ChangeMask operator|(Change a, Change b) noexcept { return ChangeMask(ChangeMask(a) | ChangeMask(b)); }
constexpr ChangeMask operator|(ChangeMask m, Change b) noexcept { return ChangeMask(m | ChangeMask(b)); }
constexpr bool has(ChangeMask m, Change b) noexcept { return (m & ChangeMask(b)) != 0; }
```

| Bits | Layers invalidated | Typical cost (20 traces × 3,800 px) |
|---|---|---|
| `Cursor`, `Datum` | Dynamic | ≤ 4 ms GUI thread |
| `Zoom`, `Offsets`, `Scale`, `Size` | Static + Dynamic | ≤ 16 ms wall (worker build + raster) |
| `Domain`, `Selection`, `Data`, `Scheme` | Static + Dynamic + Chrome | ≤ 50 ms |

Hover feedback (measurement position line, band hover text) is a widget-local Dynamic repaint and is not a `Change` bit. Summary components (`scatter`, `histogram`, later `spectrum`) recompute on `Zoom | Selection | Data | Offsets`.

### 2.3 `SelectionSnapshot`

```cpp
struct SelectionSnapshot {
    RangeRef                main;
    std::optional<RangeRef> overlay0;
    std::vector<RangeRef>   overlays;          // overlays[i] is "Overlay i+1"
    bool overlaysHidden    = false;            // F4
    bool singleColourMain  = false;            // J
    std::uint64_t version  = 0;
    std::optional<RangeRef> referenceOverlay() const;   // overlay0, else overlays.front()
    std::vector<Slot> visibleSlots() const;              // Main first; overlays omitted when hidden
};
enum class SlotKind : std::uint8_t { Main, Overlay0, Overlay };
struct Slot { SlotKind kind; int number; RangeRef range; };          // number: 0 for Main/Overlay0, 1..n otherwise

Rgba traceColour(const Scheme&, const SelectionSnapshot&, const Slot&, int channelColourIndex);
```

Colour resolution (D-017): Main → `scheme.channelPalette[idx]`, or `scheme.roles.mainSingleColour` when `singleColourMain`; Overlay 0 → `scheme.roles.overlay0`; Overlay n → `scheme.overlayStyle` applied to the channel colour, default `{ "colourSource": "channel", "dash": "solid", "lightness": 0.0, "widthScale": 1.0 }` (`scheme-1` keys and role names are those of 06 §16), i.e. plain channel colours until verified on i2. Overlays are numbered in selection order and renumbered compactly on removal. Draw order is overlays n…1, Overlay 0, Main (Main on top); within a slot the active channel is drawn last.

### 2.4 Links

Every component record carries `links { zoom, cursor, datum }` (default all `true`). The worksheet is the link group; linked components read the shared view through their `WorksheetViewModel` (§2.1 item 3). A component with `links.cursor = false` keeps a private `cursorT` (and `datumT` when `links.datum = false`) initialised from the worksheet state at creation and not persisted; with `links.zoom = false` it keeps a private `zoom`. `domain` is always shared (F9 is per worksheet). `outing` ignores `links.zoom = false` because it is the zoom control.

---

## 3. `DomainMapper`

### 3.1 Contract

```cpp
struct RangeGeom { Micros start, end; Micros offsetUs; double offsetM; double stretch; };  // from Range (D-014)
struct OutingGeom { Micros start, end; };                                                  // the slot source's outing containing R

// Outing-level distance: the laps of R's outing in order, each with its own DistanceIndex and stretch,
// concatenated; origin at R.start (or at the outing start when R is an Outing/Run/Stage range).
struct LapPiece { Micros start, end; std::shared_ptr<const DistanceIndex> idx; double stretch; };   // null idx = lap without index
class DistanceChain {
public:
    DistanceChain(std::vector<LapPiece> pieces, Micros origin);
    bool   complete() const;                    // every piece has an index
    double distAt(Micros t) const;              // metres from origin; negative before it
    Micros timeAt(double d) const;              // inverse; clamped to the outing
    XSpan  extent() const;                      // [dist(outing.start), dist(outing.end)) relative to origin
};

class DomainMapper {
public:
    DomainMapper(Domain d, RangeGeom r, OutingGeom o, std::shared_ptr<const DistanceChain> chain /*nullable*/, Rate rate, Micros seriesT0);
    // holds shared_ptr<const …> only: no reference or raw pointer to GUI-lifetime state reaches a worker (01 §5.3 rule 9)
    bool   hasDistance() const;                 // false ⇒ distance mode draws the banner for this slot
    double xOf(Micros tAbs) const;              // domain units from R.start, alignment offset applied
    Micros timeAt(double x) const;              // inverse of xOf
    double sampleAt(double x) const;            // fractional sample index in this trace's series
    double xOfSample(double i) const;
    double extent() const;                      // |R| in domain units (time: end-start; distance: chain->distAt(R.end))
    XSpan  outingExtent() const;                // time: [o.start−R.start, o.end−R.start); distance: chain->extent()
};
```

Formulas (all exact; time arithmetic in integers per D-008):

| Domain | `xOf(t)` | `timeAt(x)` | `sampleAt(x)` |
|---|---|---|---|
| Time | `(t − R.start) + R.offsetUs` | `R.start + x − R.offsetUs` | `(timeAt(x) − t0) · num / (1e6 · den)` |
| Distance | `chain.distAt(t) + R.offsetM` | `chain.timeAt(x − R.offsetM)` | as Time on the result |

`DistanceIndex` (D-016) is per `(source, lap)`: `distAt(t)` returns metres since the lap start from the `Corr Dist` F64 column (`CorrDist(t) − CorrDist(lap.start)`), `timeAt(d)` returns seconds since lap start from the `tAtD[]` table at 0.25 m (first time each distance is reached). Both are O(1). `DistanceChain` (proposed addition to D-016, DECISIONS.md Addendum A) composes them over the outing: piece `i` has stretched length `ℓ_i = idx_i.lengthM · stretch_i` and cumulative start `D_i = Σ_{j<i} ℓ_j`; with the origin in piece `m`, `distAt(t) = D_p − D_m + idx_p.distAt(t) · stretch_p` for the piece `p` containing `t`, and `timeAt(d) = p.start + llround(idx_p.timeAt((d + D_m − D_p)/stretch_p) · 1e6)` with `p` found by binary search on `D` (O(log laps); `TraceBuilder` keeps a monotone piece cursor). Inside the origin lap the chain equals that lap's `DistanceIndex` exactly, so lap-only behaviour is unchanged; outside it the chain is what lets F2, panning into the previous/next lap, range-band zoom on an adjacent lap and Main = Outing work in distance mode. Laps partition `[SOL, EOL)` (D-014 beacon-pair rule), so pieces are contiguous. A mapper whose source has no usable `Corr Dist` has `hasDistance() == false`; in distance mode the slot draws nothing and the panel shows the "no distance data" banner for that source; a chain with some pieces lacking an index draws only the indexed laps.

### 3.2 Per-slot mapping

For a worksheet with Main `M` and visible slots `k`, each trace in slot `k` gets its own mapper `map_k` built from `k`'s `RangeGeom`, its outing and its `DistanceChain`. The visible span `zoom` is the same `XSpan` for every slot (lap-relative), so overlays align at lap start in time mode (i2 behaviour; the status bar hints to switch to distance when overlays are selected in time mode) and at equal lap distance in distance mode.

Cursor position for slot `k`: `x_c = map_M.xOf(M.start + cursorT)`; `t_k = map_k.timeAt(x_c)`; key values read `Resampler::valueAt(series_k, t_k, interp)`. This is the single conversion rule for every cursor-bearing component.

### 3.3 Alignment offsets (O axis)

1. `offsetUs`/`offsetM` are per-range constants stored in the sidecar `rangeOverrides` (D-015) and surfaced in the Data window.
2. `O` toggles `ViewState.offsetAxis`; turning it off writes zero offsets for every selected range (one undo command "Clear alignment offsets").
3. `CTRL+SHIFT+LEFT/RIGHT` moves Main's offset, `ALT+SHIFT+LEFT/RIGHT` the reference overlay's offset, by one **offset step** backward/forward; `CTRL+SHIFT+DOWN/UP` and `ALT+SHIFT+DOWN/UP` move by ten steps. The step is zoom-independent: in time mode one sample period of the active channel in that slot's source (`1e6·den/num` µs, rounded to whole µs, minimum 1 µs); in distance mode the `DistanceIndex` step (0.25 m, D-016). Same definition in 06 §14.4 and 07 AC-M3-09. Dragging a sub-axis moves continuously.
4. During a drag or key repeat the offset lives in `WorksheetViewModel::pendingOffsets` (keyed by `RangeRef`) and emits `Change::Offsets`; on release or 250 ms after the last key it is committed to the sidecar as one undoable range-override edit, which publishes a new `DataSource` snapshot.
5. Selecting a range with a non-zero stored offset sets `offsetAxis = true` automatically.
6. The status strip and Data window show offsets as `+0.120 s` / `−3.5 m` in the current domain.

### 3.4 F9 domain switch (`WorksheetViewModel::setDomain`)

```
R  = Main range; chain = DistanceChain(Main) or null       // §3.1; Main mapper with alignment offsets excluded
if chain covers [R.start + a, R.start + b):
    Time→Distance:  a' = chain.distAt(R.start + a),  b' likewise
    Distance→Time:  a' = chain.timeAt(a) − R.start,   b' likewise
else:
    a' = fa · extent'(R),  b' = fb · extent'(R)            // zoomFrac rule, D-016 (fa < 0 or fb > 1 outside the Main range)
clamp [a', b') to outingExtent'(R)
cursorT, datumT unchanged (lap-relative time is domain-independent)
emit Change::Domain
```

`extent'(R)` in distance mode without an index is the bound track map's `length` when the Main source has one, else 1.0 with the x-axis labels suppressed (banner shown). Because `zoomFrac` is maintained continuously, Time→Distance→Time round-trips are lossless in both branches. The same conversion runs when the active worksheet changes to one whose `domain` differs from the shared `zoomDomain` (§2.1 item 3). Pasted components keep their own `domain`-independent body; the worksheet's `domain` applies.

---

## 4. `TraceBuilder` and exact M4 (D-012)

### 4.1 Pyramid layout used (from `esses_store`, D-011)

A `Chunk` holds `count ≤ 65,536` samples, an optional 8 KiB validity bitmap, and a complete factor-4 `MinMaxPyramid`: `level(k)` has bucket size `B_k = 4^(k+1)`, k = 0…7 (4, 16, 64, 256, 1,024, 4,096, 16,384, 65,536). Entries are `MinMax{float lo, float hi}` for every dtype, as declared in 02 §3.1 (D-054, 01 §7.1): F32 buckets are exact; entries of wider dtypes (F64 such as GPS lat/lon, I32 bitfields above 2²⁴) are rounded directionally (`lo` toward −∞, `hi` toward +∞), so their buckets are conservative enclosures and the exactness claims of §4.5 and §4.9 hold for those dtypes only as an enclosure (raw-sample reads stay exact). An all-invalid or empty bucket is `{+inf, −inf}`; NaN samples never enter a bucket. The last chunk may be partial: its trailing buckets cover fewer samples and remain exact. `SummaryPyramid` holds the levels with `B ≥ 256` for the whole column and is always resident.

### 4.2 Types

```cpp
struct M4Column {                       // one per device-pixel column
    float first, last, lo, hi;          // display units RELATIVE TO the trace origin (see decimate)
    std::uint8_t flags;                 // Empty=1, HasInvalid=2, Clipped=4, RawRegime=8, Approx=16 (summary path, §4.8)
};
struct TraceSpec {
    const Column* column; Rate rate; Micros t0; Interp interp; SampleKind kind;
    const Column* timesUs;              // non-null for a TimestampedSeries (§4.8)
    Converter toDisplay;                // y = a·base + b, applied while writing (D-010)
    TraceStyle style;                   // Line | Points
    Rgba colour; float widthLogicalPx;
};
struct YMap { float yTopPx, yBottomPx; double vMin, vMax; float yOf(double v) const; };   // computed in double

class TraceBuilder {
public:
    // Fills exactly widthPx columns for the visible span through `map`; pins chunks progressively (§4.5).
    // Returns the trace origin: the display value (double) of the first valid sample read, 0 if none.
    // Columns hold float offsets from it, so F64 channels (lat/lon near −33.4479°) keep sub-pixel precision.
    static Result<double> decimate(const TraceSpec&, const DomainMapper& map, XSpan visible, int widthPx,
                                   std::span<M4Column> out, CancelToken);
    static void emit(DrawList&, std::span<const M4Column>, double origin, const TraceSpec&, const YMap&,
                     float xOriginPx, float dpr);   // y = yMap.yOf(origin + double(col.v))
};
```

### 4.3 Column spans

For column `c ∈ [0, W)`: `x_c = visible.a + c·visible.length()/W`; `s_c = map.sampleAt(x_c)`; the integer span is `[i0, i1) = [ceil(s_c), ceil(s_{c+1}))` clamped to `[0, column.size())`; `n_c = i1 − i0`. In time mode `n_c` is constant ± 1; in distance mode it varies per column (slow corners have more samples per metre), which is why the regime is chosen per column.

### 4.4 Regimes (per column)

| `n_c` | Regime | Reads | Output |
|---|---|---|---|
| `< 2` | **A raw-joined**: every sample in `[i0,i1)` becomes a vertex at its exact `map.xOfSample(i)`; samples are joined across empty columns; point markers when overall spp `< 0.25` or style `Points` | `n_c` | 0–1 vertices |
| `2 … 31` | **B raw M4**: scan `[i0,i1)`, track first/last valid, lo/hi over valid | `n_c` | 4 vertices |
| `≥ 32` | **C pyramid M4** (§4.5) | ≈ 8–32 whole buckets + ≤ 2 × 27 | 4 vertices |

### 4.5 Pyramid path with partial-bucket descent

```
L  = largest level with B_L ≤ n_c / 8                      // ≥ 8 whole buckets per column
j0 = ceil(i0 / B_L); j1 = floor(i1 / B_L)                  // whole buckets [j0, j1)
lo/hi = fold over level(L) buckets j0..j1-1 (chunk-local index: (j·B_L mod 65536)/B_L; a bucket never
        crosses a chunk because 65,536 is a multiple of every B_k)
left  = refine([i0, j0·B_L), L-1);  right = refine([j1·B_L, i1), L-1)
first = column.at(i0); last = column.at(i1-1)             // raw samples; NaN ⇒ HasInvalid, use first/last valid of the scanned edges if any

refine([p,q), ℓ):                                           // q - p < 4·B_ℓ by construction
    if ℓ < 0: fold raw samples p..q-1 (≤ 3)                 // NaN-aware
    else:     whole buckets of size B_ℓ inside [p,q) (≤ 3): fold level(ℓ)
              straddles at the unaligned end(s): refine(…, ℓ-1)
```

A left partial range is aligned at its right end and a right partial range at its left end, so each edge has one straddle per level: cost per edge ≤ 3 reads × (L+1) levels + 3 raw reads ≤ 27 at L = 7. Total per column ≤ 32 + 54 + 2 reads; `min`/`max` are exact over `[i0, i1)` because every sample is covered exactly once by a whole bucket of some level or a raw read (for F64 and I32 columns, whose bucket entries are directionally rounded `float`s, §4.1, the result is a conservative enclosure). Columns that span two chunks run the fold per chunk.

**Progressive pinning.** `decimate` walks the columns left to right and holds `ChunkHandle`s only for the chunk(s) under the current column (at most 2, for a column straddling a chunk boundary): on entering chunk `n+1` it pins it with `Column::require(first(n+1), last(n+1), Visible, cancel)`, releases the handle of the chunk it has passed, and requests chunk `n+2` with the non-blocking `Column::prefetch(n+2, Visible)` (materialise without pinning; XREF 02 §3.2). A `TimestampedSeries` additionally pins the matching `timesUs` chunk. Pinned bytes are therefore bounded by workers × 2 chunks per column type (≈ 7 MB of F32 data on 8 workers) whatever the zoom; materialised but unpinned chunks stay resident (M0–M4) or evictable (M5, D-043). Pinning the whole visible span once would make pinned bytes scale with zoom-out (RW-4 at ≈ 2,000 spp over 3,840 px ≈ 6 GB) and trip `OutOfBudget` (01 §7.2). `CancelToken` is checked every 256 columns and at every chunk transition.

### 4.6 Validity and gaps

1. A column whose whole buckets are all empty and whose edge reads find no valid sample is `Empty`; the polyline breaks there (D-009 "rendering splits polylines at gaps").
2. In regimes A/B an invalid sample breaks the line exactly at that sample.
3. Partial invalidity inside a regime-C column (some valid samples) does not break the line: the gap is sub-pixel.
4. `HasInvalid` is set whenever an invalid sample was seen; the status strip and measurements ignore the flag.

### 4.7 Vertex emission

Per non-empty column at pixel `c` (device px, after `xOriginPx` and `dpr`): `(c, first)`, `(c+0.5, lo)` and `(c+0.5, hi)` in the order `lo,hi` when `first ≤ last` else `hi,lo` (the in-column order is unknown from a pyramid; the vertical extent is exact, the sub-pixel order is invisible), `(c+1, last)`. Consecutive columns join `last → next first`; an `Empty` column ends the current `Polyline` command and starts another. Y values are clamped to `[yTop − 2w, yBottom + 2w]` (w = line width) with `Clipped` set so off-scale samples keep correct slopes under the panel `Scissor`. `Interp::Previous` channels (int/enum/bool/gear) in regime A emit the step `(x_{i+1}, v_i)` before `(x_{i+1}, v_{i+1})`. `Points` style: regimes A/B emit a `Points` command (one vertex per raw sample); regime C emits one vertical `Lines` segment `lo→hi` per column.

### 4.8 Timestamped series and the summary path

A `TimestampedSeries` trace is decimated without resampling: the column boundaries are found directly in sample indices by `i_c = lower_bound(timesUs, map.timeAt(x_c))` (one binary search for column 0, then a monotone forward cursor), and the values column's own per-chunk pyramid then applies unchanged because the pyramid indexes samples, not time; regime A places each vertex at `map.xOf(timesUs[i])`. The result is exact M4 with no resampled copy and no extra pyramid (a median-rate resampled view would drop extrema in bursts denser than the median rate and allocate memory no budget counts).

Full-extent views (`outing`, `F2` on a very long file where `n_c ≥ 2,048` for every column) take the **summary path**: they read only `SummaryPyramid` levels (`B ≥ 256`) and never block. The descent stops at `B = 256`; each edge's residual ≤ 255 samples is refined through the chunk pyramid only when the chunk can be pinned without blocking (`Column::tryPin(chunkNo) → std::optional<ChunkHandle>`, XREF 02 §3.2); otherwise the whole straddling 256-sample bucket is folded in, `first`/`last` fall back to that bucket's midpoint and the column is flagged `Approx`. There is no raw-pointer `tryChunk`; every reader, GUI or worker, pins through `tryPin` (01 §5.3 rule 5). Bound: `lo/hi` enclose the exact reference, and every extra sample lies within one 256-sample bucket (≤ 1/8 column at `n_c ≥ 2,048`) of the column boundary; this approximation is allowed only on the summary path.

### 4.9 Cost and tests

Reads per trace ≤ 3,800 × 88 ≈ 335 k; measured target ≤ 0.5 ms per trace per core, 20 traces on 4 workers ≤ 3 ms wall. Property test (`tests/plotcore/m4_property.cpp`): for random F32, F64 and I32 columns (uniform and timestamped) with NaN sprinkles and partial last chunks, at every zoom from spp 0.1 to 10⁶ and every offset, `decimate` on the chunk path (regimes A, B, C) equals a brute-force per-column reference bit-for-bit on `lo/hi/first/last/flags` (the reference applies the same `float(v − origin)` conversion); for F64 and I32 columns, `lo/hi` of columns that fold pyramid buckets enclose the reference instead, widened by at most the directional rounding of §4.1. The summary path is tested separately against its stated bound (§4.8): `lo/hi` enclose the reference and every extra sample lies within one 256-sample bucket of the column boundary, with and without resident chunks. Further tests: distance mode with a synthetic `DistanceIndex` whose plateaus (stopped car) produce zero-length columns, and a `DistanceChain` spanning three laps with different stretches (continuity at lap joins, round trip `timeAt(distAt(t)) = t` within one 0.25 m step); an F64 latitude column at −33.4479 ± 1e-6° over a 1,000 px panel renders monotone steps at 1 px resolution when the view reads raw samples (pyramid levels round F64 to float, about 3.8e-6° or 0.4 m, so zoomed-out views are checked as enclosures only); RW-4 at ≈ 2,000 spp builds with `ChunkCache::Stats.pinnedBytes ≤ 32 MB` (bench `decimate`).

---

## 5. `DrawList` IR

### 5.1 Types

```cpp
using Rgba = std::uint32_t;                       // 0xRRGGBBAA, persisted as "#rrggbbaa"
enum class LayerKind : std::uint8_t { Static, Dynamic, Chrome };
enum class CmdKind : std::uint8_t { Clear, Scissor, Polyline, Points, Lines, Quads, FilledRect, TextRun };
enum class DashId  : std::uint8_t { Solid, Dash, Dot, DashDot };
enum class TextStyleId : std::uint8_t { Axis, Caption, Key, KeyValue, Band, Measurement, Banner, Gauge, GaugeUnits, Table,
                                        Equation, ColourBand, TrackLabel };   // font roles: §6.1 item 4
enum class TextAnchor : std::uint8_t { TopLeft, TopCentre, TopRight, MiddleLeft, Centre, MiddleRight, BottomLeft, BottomCentre, BottomRight };
struct RectPx { float x, y, w, h; };

struct TextRun {
    std::uint32_t byteOffset, byteLen;  // into DrawList::text (UTF-8)
    float x, y;                         // device px anchor point
    TextStyleId style; TextAnchor anchor; std::uint8_t quarterTurns;   // 0 or 1 (vertical axis titles)
    Rgba colour; float maxWidthPx;      // > 0 ⇒ elide with "…"
    Rgba background;                    // 0 ⇒ none (key value boxes use the panel colour)
};
struct DrawCmd {
    CmdKind kind; std::uint32_t vertexOffset, vertexCount;   // Polyline/Points/Lines/Quads
    Rgba colour; float widthPx; DashId dash; float pointSizePx;
    RectPx rect;                        // Scissor, FilledRect
    std::uint32_t textIndex;            // TextRun
};
struct DrawList {
    int widthPx, heightPx; float dpr;
    std::vector<DrawCmd> cmds; std::vector<float> verts;    // interleaved x,y device px
    std::vector<TextRun> texts; std::string text;
    void append(const DrawList&, float dxPx, float dyPx);   // sub-list placement (panels)
};
```

Rules: all coordinates are device pixels, produced by builders that receive a `LayoutContext { int widthPx, heightPx; float dpr; }` and multiply logical sizes by `dpr` themselves; `Scissor` nests by replacement (a `Scissor` with `w = 0` restores the full layer); `Quads` consume four vertices each with the command colour (per-vertex colour, needed by `rainbow_track`, is a v1.0 extension `QuadsRgba` with a parallel `vertRgba` stream); `Clear` fills the layer with the command colour; text is never converted to geometry in `esses_plotcore`.

### 5.2 Layer contents (D-025)

| Layer | Contents | Rebuilt by |
|---|---|---|
| Static | background, grid, scale lines, bands, traces, variance traces, status strip, axes and tick labels, key labels | worker build + GUI raster into the one cached `QImage` on `Zoom/Offsets/Scale/Size/Domain/Selection/Data/Scheme` |
| Dynamic | cursor line(s), datum line, cursor sample markers, key values and Δ, measurement hover line, drag feedback, zoom rubber band, offset sub-axes while dragging, pending "…" | `DrawList` built on the GUI thread on `Cursor/Datum` and hover, painted directly onto the widget (no image, §6.2) |
| Chrome | caption bar, focus border, active-channel square, pending stripe, "no distance data" banner | `DrawList` built on the GUI thread on focus/caption/pending changes, painted directly |

### 5.3 `IRenderBackend`

```cpp
struct LayerHandle { std::uint32_t id = 0; };
struct Bitmap { int w, h; std::vector<std::uint8_t> rgba; };          // straight alpha, row-major

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual void        beginLayer(LayerKind, int widthPx, int heightPx, float dpr, Rgba clear) = 0;
    virtual void        draw(const DrawList&) = 0;                 // may be called several times per layer
    virtual LayerHandle endLayer() = 0;
    virtual void        compose(std::span<const LayerHandle> inOrder, LayerHandle target) = 0;
    virtual Bitmap      readback(LayerHandle) = 0;                 // goldens, clipboard, print
    virtual void        release(LayerHandle) = 0;
    virtual std::shared_ptr<const ITextMetrics> textMetrics() const = 0;   // immutable snapshot for the current scheme
};
```

### 5.4 `ITextMetrics`

```cpp
class ITextMetrics {                       // esses_plotcore; implemented in esses_qtrender and by tests
public:                                    // contract: every const member is safe to call concurrently from any thread
    virtual float advancePx(TextStyleId, std::string_view utf8, float dpr) const = 0;
    virtual float lineHeightPx(TextStyleId, float dpr) const = 0;
    virtual float ascentPx(TextStyleId, float dpr) const = 0;
    virtual std::uint64_t generation() const = 0;      // scheme/font generation the snapshot was made for
};
```

`FixedTextMetrics` (0.6 em advance, 1.2 em line height, em = style point size × dpr × 96/72) is the test implementation; layout tests assert geometry against it, golden images use the Qt implementation.

`QtTextMetrics final : ITextMetrics` (`esses_qtrender`) is constructed on the GUI thread from the active scheme **after** every application font has been registered (`QFontDatabase::addApplicationFont` runs at start-up and in `OffscreenRenderer` set-up, before any worker measures). It holds only plain values: one font description per `TextStyleId` (family, `pointSizeF`, weight, italic, strikeOut) and the generation. `QFont` and `QFontMetricsF` are reentrant, not thread-safe (a shared `QFont` lazily resolves its engine), so no instance is ever shared between threads: each call uses a `thread_local` cache of `QFont` + `QFontMetricsF` keyed by `(generation, style, dpr)`, built on first use in that thread; there is no shared mutable memo (a later shared advance cache must sit behind `std::shared_mutex`). A scheme change creates a new snapshot with a new generation; in-flight builds keep the old one alive through their `shared_ptr`. Test: `tests/qtrender/text_metrics_concurrency.cpp` measures the same strings from 8 threads while the GUI swaps schemes and compares against single-threaded results; it runs under the Linux TSan build with Qt instrumented (XREF 08 §4.1, which today limits TSan to the Qt-free suites).

### 5.5 Layout builders

`AxisLayout` (tick placement: "nice" steps 1/2/5 × 10ⁿ in the axis unit; minimum tick spacing 6 × average label advance; time labels in the project time format `s.mmm`, `m:ss.mmm` or `h:mm:ss`; distance labels in the `Corr Dist` display unit), `BandLayout` (range/section/marker bands, §8.11), `KeyLayout` (key column width = max label advance + value column, clamped to 12–40 % of the component width), `HitTester` (pixel → `{kind, group, trace, slot, band, markerId, handle}` for click/hover/drag; built alongside the Static `DrawList` and kept with the layer). Each builder is a pure function of `(ComponentSpec, ViewState, SelectionSnapshot, data snapshot, LayoutContext, const ITextMetrics&)`, where the reference comes from a `std::shared_ptr<const ITextMetrics>` captured by the task, and is unit-tested without a window.

---

## 6. `esses_qtrender`

### 6.1 `RasterBackend`

1. Layers are `QImage(Format_ARGB32_Premultiplied)` of `widthPx × heightPx` device pixels with `setDevicePixelRatio(dpr)`; the backend owns a free-list keyed by size so a zoom step allocates nothing.
2. `QPainter` with `Antialiasing` on for `Polyline`/`Lines` of width > 1 px and off for width ≤ 1 px (cosmetic 1-px lines are drawn with integer-snapped `QPoint` arrays, which QPainter rasterises markedly faster); `TextAntialiasing` on, `HighQualityAntialiasing` off.
3. `Polyline` commands are drawn with `drawPolyline(const QPointF*, n)` in slices of ≤ 4,096 points; `Points` with `drawPoints`; `Quads`/`FilledRect` with `fillRect`/`drawPolygon`; `Scissor` with `setClipRect`.
4. `TextRun` → `QStaticText` cached by `(text, styleId, dpr)` (LRU 4,096 entries, GUI thread only); fonts from `SchemeFonts` (scheme JSON `fonts.*` → `QFont` at `pointSize`, `QFont::NoSubpixelAntialias` for determinism); anchors resolved with `QFontMetricsF`; elision via `QFontMetricsF::elidedText`. `TextStyleId` → `scheme-1` font role (06 §16): `Axis` → `axis` (`axisRotated` when `quarterTurns = 1`), `Caption` and `Banner` → `caption`, `Key` and `KeyValue` → `key`, `Band`, `Measurement` and `GaugeUnits` → `small`, `Gauge` → `gauge`, `Table` → `reportTable`, `Equation` → `equation`, `ColourBand` → `colourBands`, `TrackLabel` → `trackLabel`.
5. `compose` blits in order with `CompositionMode_SourceOver`; `readback` converts to straight alpha. Widgets do not compose images: `RasterBackend::paintList(QPainter&, const DrawList&)` (Qt-level entry, not part of `IRenderBackend`) paints a Dynamic or Chrome list directly onto the widget painter (§6.2); `compose`/`readback` serve `OffscreenRenderer`, goldens and Print to Clipboard.
6. The backend is the print, clipboard and golden path (D-025) and never changes after `GlBackend` exists.

### 6.2 `LayerCache`

```cpp
class LayerCache {                                  // one per ComponentWidget, GUI thread only
public:
    void   resize(QSize logical, qreal dpr);        // reallocates the Static image on change; marks all dirty
    void   invalidate(ChangeMask);                  // table in §2.2
    bool   dirty(LayerKind) const;
    void   storeStatic(QImage);                     // worker DrawList rasterised by RasterBackend (image shared)
    void   setDynamic(DrawList); void setChrome(DrawList);   // GUI-built; kept as lists, never as images
    QRegion dynamicDamage(const DrawList& before, const DrawList& after) const; // old ∪ new cursor/datum strips + key-value column
    void   paint(QPainter& widgetPainter, const QRegion& exposed) const;      // paintEvent, see below
    std::size_t bytes() const;                      // Static image only
};
```

`paint` blits only the exposed part of the Static image (`p.drawImage(r, staticImg, r · dpr)` per exposed rect) and then paints the Dynamic and Chrome lists with `RasterBackend::paintList` under the same clip. A cursor or datum move calls `QWidget::update(dynamicDamage(old, new))`, i.e. the old and new cursor strips (`x ± (w + 2)` device px over the trace area) plus the key-value column, so Qt clips both the composition and the backing-store flush to a few thousand pixels; a full-size Dynamic image would cost ≈ 200 MB of memory traffic per move at 3,840 × 2,160 (clear, Static blit, two full blends), 15–25 ms against the 4 ms budget.

Memory rule: cached layer bytes per component ≈ 1 × 4 B × device area (Static only; ≈ 5.8 MB at 1,600 × 900, ≈ 31 MB at 3,840 × 2,000); a component on a hidden worksheet starts a 30 s single-shot timer on `hideEvent` and releases its Static image only if it is still hidden when the timer fires; `showEvent` cancels the timer (the image is reused) or rebuilds a released image (D-058, 01 §7.1); `LayerCache::bytes()` is summed into the Messages budget line.

### 6.3 Static rebuild pipeline

1. `ComponentWidget::onViewChanged(ViewState, ChangeMask)` → `LayerCache::invalidate(mask)`; if Static is dirty, snapshot inputs and submit `RenderService::submitBuild(key = {worksheetId, componentId}, Priority::Visible, build, onGui)` (`esses_qtmodel` facade over `TaskPool`: the key is a replace-key with latest-wins on the queue, 01 §5.4 rule 2, so a burst such as a wheel zoom keeps at most one running and one pending build; the running build is never cancelled by a newer submit; `onGui` is delivered through `Dispatcher::post(component, …)`); `esses_widgets` never calls `TaskPool` directly (06 §1).
2. Worker: mappers → `TraceBuilder::decimate` per trace (sub-tasks, parallel) → `emit` → `AxisLayout`/`BandLayout`/`KeyLayout`/`VarianceTable` → `DrawList` + `HitTester` → `Dispatcher::post`.
3. GUI: if the result's `viewVersion` matches (or differs only in `Cursor|Datum` bits), `RasterBackend` draws it into the Static image, `LayerCache::storeStatic`, `update()`. A superseded result (a newer build is pending) is still displayed as an interim frame, with the integer shift or scale of step 5 applied until the pending build lands, so a continuous zoom or drag keeps producing frames.
4. Dynamic layer: a `DrawList` built synchronously on the GUI thread and painted under the damage region of §6.2, from `ViewState` + `HitTester` + `Resampler::valueAt` over chunks pinned with `Column::tryPin(chunkNo, Visible)` into the paint's pin arena (01 §5.3 rule 5; an absent chunk is requested at `Visible`, the priority 01 §5.2 gives GUI key values); values whose chunk is absent or whose maths is Pending render "…" and the completion (`ChannelReady`) triggers a Dynamic repaint.
5. Pan at constant zoom may shift the previous Static image by the integer pixel delta and show it until the rebuild lands (optimisation slot, not required for budget).
6. Resize events are coalesced to 30 Hz with a `QTimer`; the final size always rebuilds.

### 6.4 MVP budgets and measurement (D-025)

| Budget (absolute limit, median of ≥ 5 runs on the reference machine, D-042) | Limit |
|---|---|
| Cursor move, `time_distance`, 20 traces × 3,800 px, DPR 1 and 2 (bench `dynamic` also at 3,840 × 2,160 device px, damage-clipped blit + flush included) | ≤ 4 ms GUI thread (Dynamic only) |
| Zoom/pan step, same workload, worker build + raster | ≤ 16 ms wall |
| Full relayout (Domain/Selection) | ≤ 50 ms |
| Resize | coalesced to 30 Hz |
| Open 100 MB `.ld` → first paint, 8 channels | ≤ 500 ms |
| Values refresh, 250 channels | ≤ 2 ms |
| Layer memory, visible worksheet | ≤ 1 × 4 B × device area per component (Static image only) |

`bench/plot_bench.cpp` (Google Benchmark, JSON baseline) runs on synthetic 1 kHz noise with 1 % NaN, 20 traces in 4 groups, at widths 1,900 and 3,800, DPR 1 and 2, reporting `decimate`, `raster_static`, `dynamic` and `relayout` separately so a regression names its stage.

### 6.5 GL escalation rule

1. The MVP ships `RasterBackend` only; `GlBackend` is not started on schedule.
2. Trigger: the bench job exceeds a budget in §6.4 on two consecutive runs on the reference machine (absolute limits, median of ≥ 5 runs, D-042), or the v1.0 nightly fails RW-4 (`cursor ≤ 2 ms CPU`, `zoom rebuild ≤ 33 ms hard`), or the user's hardware (open question Q-07) fails the MVP budgets.
3. Before GL, the ordered remedies are: integer-snapped polylines for width ≤ 1 px, batching commands by pen, `QImage` reuse, skipping the Chrome re-render, per-trace parallel `decimate`. Only after these fail does the GL work item open in the milestone where the trigger fired (M4 for MVP budgets, M5 for v1.0).
4. Granularity (one `QOpenGLWidget` per component vs one canvas per worksheet with viewports) is decided by `bench/gl_granularity` measuring cursor-move wall time for 10 components, resize behaviour and mixed-DPI floating docks; the `DrawList` IR supports both.
5. `GlBackend` must reproduce every raster golden at ≤ 2 % differing pixels, implements the same `IRenderBackend`, and text stays on the QPainter path (Chrome and `TextRun` drawn through `QOpenGLPaintDevice` or a composed QImage).
6. `RhiBackend` (`QRhiWidget`, Qt ≥ 6.7, no BC guarantee) is a post-v1.0 option behind the same seam.

### 6.6 HiDPI rules

1. Layout is computed in logical pixels and converted once in the builders (`× dpr`); widths, point sizes and margins are logical and multiply by `dpr`; fonts are specified in points.
2. The Static `QImage` (and every offscreen layer) is allocated at device size and tagged with the DPR; `LayerCache::paint` and `compose` draw at the widget's logical size.
3. `ComponentWidget` listens to `QWindow::screenChanged`/`devicePixelRatioChanged` and the `WorksheetCanvas` emits `Change::Size` with a new `dpr` → full rebuild.
4. Fractional DPR (1.25, 1.5) is supported: sizes are rounded to device pixels only at command emission (`std::lround`), never in intermediate layout values.
5. Floating docks on a second monitor with a different DPR render independently; CI covers DPR 1, 1.5 and 2 for goldens of `time_distance` and `values_list` (full set at 1 and 2).
6. Print to Clipboard renders at the widget's current DPR; `esses-render --dpr` selects the DPR explicitly.

### 6.7 `OffscreenRenderer` and golden images (D-033)

```cpp
class OffscreenRenderer {
public:
    QImage render(const WorkbookFile&, int worksheet, std::span<const std::shared_ptr<const DataSource>>,
                  const SelectionSnapshot&, const ViewState&, QSize logicalPx, qreal dpr, const Scheme&);
    QImage renderComponent(const ComponentSpec&, /* same inputs */ ...);
};
```

1. Runs with `QT_QPA_PLATFORM=offscreen`; submits the same builders to `TaskPool` as the widgets do (per-trace sub-tasks, §6.3; `esses_qtrender` has no widgets, so the `RenderService` rule of 06 §1 does not apply) and waits on the futures on its calling thread, which is allowed because only the interactive app's GUI thread carries `ThreadRole::NoBlock` (§1 item 4); `Column::require` therefore runs on workers exactly as in the app. Dynamic and Chrome lists are built on the calling thread and painted over the Static image. The interactive Print to Clipboard does **not** use this class: it paints the component's current layers (§7.2 item 9) and never blocks.
2. Determinism: fonts come only from `tests/golden/fonts/` (Noto Sans, the shipped schemes' font role family, OFL-1.1; registered with `QFontDatabase::addApplicationFont` before any `QtTextMetrics` exists; system fonts disabled by `QT_QPA_FONTDIR`; outside the shipped `data/` tree and REUSE-annotated `OFL-1.1`, XREF 08 §6.4/§10), grayscale text antialiasing, scheme fixed to `data/schemes/light.scheme.json`/`dark.scheme.json` (06 §16), no animation, `std::sort` stability for tick labels.
3. Fixtures: `tests/golden/<type>/<case>@<dpr>x.png` with `<case>.json` (worksheet spec, source fixture ids, selection, view state). Minimum set: every MVP component × {light, dark} × {1×, 2×}; `time_distance` additionally at most 12 named cases (for example time and distance × panels and overlapped, datum, measurements, variance, offset axis, no-distance banner, F2 outing view), not the cross product of those toggles (08 §6.4). Pixel goldens are authoritative on ubuntu only; Windows and macOS run the `DrawList` JSON goldens and render smoke tests (08 §6.4, D-127).
4. Comparison: a pixel differs when any channel differs by > 8/255; the image fails when > 0.5 % of pixels differ; the diff image is uploaded as a CI artifact. `--update-goldens` rewrites after review, from the ubuntu CI artefacts only, never from a local render (08 §6.4).
5. `esses-render <workbook.json> <log…> --worksheet N -o out.png [--dpr 2] [--component K] [--selection fastest]` is the CLI around the renderer and is also used by the docs build.

---

## 7. Component framework (`esses_widgets`, shared behaviour)

### 7.1 `ComponentWidget`

The single declaration (this section owns it; 06 §4.4 refers here). String types are Qt types because `esses_widgets` sits above the `QString` line (06 §1 rule 1); conversion from UTF-8 happens in the view-models.

```cpp
class ComponentWidget : public QWidget {
  Q_OBJECT
public:
    enum class OverlayPolicy { MainOnly, MainAndReference, AllOverlays };
    static ComponentWidget* create(const esses::ComponentSpec&, WorksheetContext&);   // factory by type string
    virtual const char* type() const = 0;                      // JSON type string
    const esses::ComponentSpec& spec() const;
    virtual void applySpec(const esses::ComponentSpec&);       // after undo/redo or properties Apply; never edits in place
    virtual void onViewChanged(const esses::ViewState&, esses::ChangeMask);
    virtual void onSelectionChanged(const esses::SelectionSnapshot&);
    virtual void onDataChanged(esses::SourceId, std::uint64_t version);
    virtual void onSchemeChanged(const esses::Scheme&);
    virtual OverlayPolicy overlayPolicy() const;
    virtual QStringList channelNames() const;                  // Values window "focused component" mode (06 §8)
    virtual std::optional<QString> activeChannel() const;
    virtual bool acceptsChannelDrop() const;                   // Channels window ENTER / drag target
    virtual void addChannel(const QString& name, bool newPanel);   // ENTER / CTRL+ENTER / drop
    virtual QList<CommandId> contextCommands() const;          // subset of the Component menu (context menu = same ids)
    virtual bool exportData(ExportSink&, const ExportOptions&) const;  // CSV always; false = cancelled or failed
    virtual QImage renderToImage(qreal dpr) const;             // Print to Clipboard: current layers at dpr (§7.2 item 9)
    virtual void openProperties(int tabIndex = 0);             // F5 / CTRL+N
signals:
    void specEditRequested(esses::ComponentSpec newSpec, QString undoText);   // canvas wraps it in a QUndoCommand
    void focusRequested();
};
```

`WorksheetContext` (06 §4.4) supplies `WorksheetViewModel&`, `DataSelectionModel&`, `MathsService&`, `RenderService&`, `Scheme&` and `CommandRegistry&`.

### 7.2 Shared behaviours (numbered, each with a Qt Test row)

1. Focus: exactly one focused component per worksheet (thick border in Chrome); `TAB`/`SHIFT+TAB` cycle in z-order then reading order; click focuses; `ESC` from a dock returns focus to the last focused component.
2. `F5` opens the modeless tabbed `PropertiesDialog` (Apply, OK, Cancel; every Apply is one undo command); `CTRL+N` opens it at the active channel.
3. Active channel: square beside the name; `PAGE UP/DOWN` select; `CTRL+H` hide, `CTRL+DELETE` remove, `CTRL+SHIFT+M` edit maths, `CTRL+SHIFT+S` status, `CTRL+SHIFT+F` filter, `CTRL+SHIFT+O` scale/offset, `CTRL+SHIFT+Z` zero at cursor; right-click on a label makes it active and opens the channel menu.
4. `L` toggles the channel legend/key, `K` the colour-channel legend (with its context menu), `G` cycles layout, `S` cycles style, `ALT+0..9` toggles colour band `n` (bands beyond the list are no-ops).
5. Add channels: Properties → Channels, Channels window `ENTER`/double-click (`CTRL+` variant → new group/panel), drag with a drop indicator over the target group.
6. Clipboard: `CTRL+X/C/V` cut/copy/paste component records (MIME `application/x-esses-components+json`, payload per 06 §4.3 item 10); pasted records keep their body including domain-dependent settings and manual scales; `DELETE` removes (undoable).
7. Geometry: free fractional rectangles `[l,t,r,b]` with 1 % snap; `CTRL+click` multi-select move/resize; double-click the caption to maximise/restore; z-order via "Send to background"/"Bring to front" (persisted as `z`, §7.3).
8. Export Data: Component menu → CSV with the MoTeC-dialect header in MVP (`.ld`/`.mat` v1.0); only the component's channels; range options "Current zoom", "Selected ranges padded by 2 s", "All data".
9. Print to Clipboard: `renderToImage(dpr)` paints the component's current Static image and its Dynamic and Chrome lists into one `QImage` at the current DPR → `QClipboard` (the previous Static image while a rebuild is in flight; never blocks); `QTableView` components use `QWidget::render`. The result equals `esses-render` at the same DPR because both run the same builders (06 §22 M3).
10. Pending state: any trace/cell/value whose data is `Pending` (D-022) shows "…" or a hatched area; `Error` shows the status text in a tooltip and an italic label.
11. Missing channel: label italic "(missing)" (D-013) or "(not in Lap 9)" for an overlay source lacking the channel; no trace.

### 7.3 Common persistence record (`workbook-1` component header)

```json
{ "id": "c1a0…", "type": "time_distance", "version": 1, "rect": [0.0, 0.0, 0.75, 1.0], "z": 0,
  "caption": { "show": true, "auto": true, "text": "" },
  "links": { "zoom": true, "cursor": true, "datum": true },
  "legend": { "channels": true, "colour": false },
  "activeChannel": "Corr Speed",
  "body": { },
  "x-i2": { } }
```

This section is the canonical component header and §8.14–§17 the canonical bodies (02 §6.4 fixes only the file envelope and refers here; i2's `ModeId` goes to `x-i2`). `id` is the component UUID (undo merge, properties dialog identity, build key). `z` is the paint order, ascending (higher paints on top; equal `z` paints in array order); "Send to background" sets `z = min(z) − 1` and "Bring to front" `z = max(z) + 1`, after which the worksheet's `z` values are renormalised to `0…n−1` in the same command; the array order of `WorksheetDef.components` carries no meaning beyond the tie-break. TAB order is `z` then reading order (§7.2 item 1).

Shared sub-schemas (persisted definitions only, never generated bands; numbers carry units per D-010):

```json
"BandAxis": { "mode": "even", "bandMode": "width", "dataMin": { "v": 0, "unit": "km/h" }, "dataMax": { "v": 300, "unit": "km/h" },
              "bandWidth": { "v": 20, "unit": "km/h" }, "bandCount": 15, "palette": "rainbow", "colour1": "#2040ff", "colour2": "#ff2020",
              "reverse": false, "centred": false, "lessMinBand": true, "greaterMaxBand": true, "continuous": false,
              "bands": [ { "min": { "v": 0, "unit": "km/h" }, "max": { "v": 50, "unit": "km/h" }, "colour": "#2040ff" } ] }
"ColourBands": [ { "min": { "v": 80, "unit": "C" }, "max": { "v": 100, "unit": "C" }, "open": "none", "colour": "#ffb000", "visible": true } ]
```

`BandAxis.mode = "arbitrary"` uses `bands[]` verbatim; `"even"` generates bands from `dataMin/dataMax/bandWidth|bandCount` at load. `ColourBands[n]` is the target of `ALT+n` (`ALT+0` = band 0); `open` ∈ `none | below | above` encodes i2's open-ended bands.

### 7.4 Overlay policy and recompute triggers

| Type | Overlay policy | Honours `links` | Recompute on |
|---|---|---|---|
| `time_distance` | AllOverlays | zoom, cursor, datum | §2.2 table |
| `outing` | Main (+ overlays when `enableOverlays`) | cursor; zoom always (it is the control) | Selection, Data, Size, Scheme; Dynamic on Zoom/Cursor |
| `scatter` | AllOverlays (drawn beneath Main) | zoom, cursor, datum (read-only) | Zoom, Selection, Data, Offsets |
| `histogram` | AllOverlays (outline style) | zoom, cursor | Zoom, Selection, Data, Offsets |
| `channel_report` | all ranges in scope (`excludeOverlays` option) | zoom when `zoomLinked` | Selection, Data, Zoom (if linked) |
| `time_report` | all laps in scope | none | Selection, Data |
| `track_position` | AllOverlays (dots) | cursor, zoom (span highlight) | Static on Data/Scheme/Size; Dynamic on Cursor/Zoom/Domain |
| `values_list` | MainAndReference | cursor, datum | Cursor, Datum, Selection, Data |
| `gauge_numeric`, `gauge_bar` | MainAndReference | cursor | Cursor, Selection, Data, Scale |

---

## 8. `time_distance`

### 8.1 Anatomy (top → bottom; logical px defaults from the scheme)

Caption bar (18 px; auto caption = comma-separated channel list, elided) → range band (14 px) → section band (14 px) → marker band (14 px) → group panels, or one overlapped area, each with: key column (left, §8.4), vertical axis strip (36 px; the active group additionally shows the vertical zoom/pan bar), trace area → status strip (`E`, 10 px per status channel) → variance panel (`F3`, `variance.heightFrac` of the trace area, default 0.15) → x-axis (18 px, labels per §5.5) → offset axis (`O`, 2 × 12 px sub-axes: Main, reference overlay) → horizontal scrollbar (12 px, proportional thumb = `zoom.length()` / outing extent, §8.6). Bands, status strip and variance panel are omitted when hidden; their heights are scheme-tunable.

### 8.2 Groups, panels, overlapped mode

1. A graph holds ordered groups; each group holds ordered traces; `G` cycles `overlapMode ∈ panels | overlapped`.
2. Panels divide the trace area by `edges`: each group's `[top, bottom]` as fractions of the trace-area height (0 at top). In `panels` mode edges are contiguous and resized by dragging the splitter between panels; in `overlapped` mode each group's `[lo, hi]` region is set by dragging its handles at the right edge or by Lo/Hi values in Group Properties (fractions of the area, 0 = top, 1 = bottom; regions may overlap).
3. Like-units grouping: adding a channel joins the first group whose `quantity` equals the channel's (unit family compare, D-010), unless `CTRL` is held → a new group appended at the bottom with the channel's name as its name.
4. Groups are reorderable by dragging the group heading in the key column; double-clicking the heading opens Group Properties (name, scale mode, min/max, scale lines, Lo/Hi).
5. Traces with differing rates are decimated on their own grids (§4); each trace's `DomainMapper` uses its own `Rate`/`t0`.

### 8.3 Scale modes and scale lines

1. `scaleMode = manual`: `scaleMin/scaleMax` (with unit) apply to this group on this graph.
2. `scaleMode = channel` ("Channel/Auto"): each trace's `[min, max]` is its channel override if set, else an autoscale; group `min = min of mins`, `max = max of maxes` across visible traces (a channel with an override contributes its override, others their autoscale — mixed groups combine per channel).
3. Autoscale of a channel = min/max over the **Main source's file**, not the Main range: the union over every trusted lap of the Main source (the whole file when `options.autoscale.includeUntrusted`; a source without laps counts as one trusted range); when `options.autoscale.includeOverlays`, unioned with the same quantity computed for each visible overlay *source* (i2 combines the overlay file's min/max with the main file's). Per range it folds whole `SummaryPyramid` buckets and refines the range edges through chunk pyramids when the chunk can be pinned without blocking (`tryPin`), else folds the straddling 256-sample bucket (a superset by ≤ 255 samples per edge). Results are computed on a worker at `Background` priority and cached per `(channel version, source, trust-set hash)` in `AutoscaleCache` (`esses_plotcore`, one per workspace), so N/P within the Main source never change the scale; changing the Main source, toggling an overlay source, a trust flag or either option does (`Change::Scale`). Both options are project options (Tools → Options → Analysis). Until the cache entry lands the group keeps its previous scale (or the Main range's resident chunks on first draw).
4. Degenerate spans (`max − min == 0`) expand to `±1` display unit about the value; NaN spans (no valid data) draw the panel with no axis labels.
5. Scale lines per group: `lines { zero, min, max }` each `{ enabled, colour }`; `zero` defaults on when the scale crosses 0, `min/max` draw at the channel override limits ("normal operating range").
6. Vertical zoom/pan (`ALT+UP/DOWN`, `ALT+F2`, `SHIFT+UP/DOWN`, dragging the vertical scrollbar ends/thumb, `ALT+rubber band`, double-click the vertical scrollbar = full) acts on the active channel's group and is view state local to the component (not persisted, not linked).

### 8.4 Key column

| Mode | Columns |
|---|---|
| no datum | label · value@cursor (display unit, `dps`) per visible slot (Main first) |
| datum (`D`) | label · value@main · value@datum · Δ |
| measurements (`M`) | adds a row per trace and slot: min · max · avg over the visible span |

Enum channels show the enum label at the cursor; values of Pending maths show "…"; a slot whose source lacks the channel shows "(not in Lap 9)". `key.mode ∈ labels | values | none` and `key.pos ∈ left | right` persist. Hovering a min/max cell draws a vertical line at its position (Dynamic); double-clicking it moves the cursor there (`WorksheetViewModel::setCursor`).

### 8.5 Cursor and datum

1. Click in the trace area places the cursor; the cursor snaps to the nearest Main sample of the active channel (`cursorT = round(sampleAt(x))` mapped back through `xOfSample`).
2. `LEFT/RIGHT` step by one device pixel's worth of samples of the active trace (`max(1, round(spp))`); `CTRL+LEFT/RIGHT` × 10; `HOME/END` to the visible edges; `H` centres the view on the cursor; `options.autoCentreCursor` keeps it centred when stepping.
3. `Y` cycles `cursorStyle ∈ line | crosshair | none` (values still shown).
4. `SPACE` places the datum at the cursor (`datumT = cursorT`); `D` toggles datum visibility (keeps the value); `X` exchanges the positions of the main and datum cursors (`std::swap(cursorT, *datumT)`, emits `Cursor|Datum`; i2 "swap datum and main cursor"), so the arrow keys, which always move the main cursor, now adjust the other point; `Z` zooms to `[min, max]` of the two.
5. The status bar shows `t`, `d`, lap and, with a datum, `Δt`, `Δd` (distance from the Main `DistanceIndex`).
6. With a datum the Values window switches to the active-channel measurement view (value@main, value@datum, Δ, min/max/avg between the cursors, `Δt/Δd`).
7. Animation `A` advances `cursorT` on a `QTimer` at the Data-menu rate (×0.25 … ×4 real time, `options.animation.rate`); it is a `Cursor`-only change.

### 8.6 Zoom and pan

| Action | Hotkey / mouse | Result (`zoom = [a,b)`, `c` = cursor x) |
|---|---|---|
| in / out | `UP` / `DOWN` | span × 0.5 / × 2 about `c`; minimum span = 4 samples of the active trace; maximum = the Main outing extent (`F2` rule) |
| whole Main outing | `F2` | `map_M.outingExtent()` in the current domain: `[outing.start − M.start, outing.end − M.start)` in time, the `DistanceChain` extent in distance (starts below 0 whenever Main is not the first lap) |
| default range | `W` | `[0, |Main|)`; if the cursor lies outside, it is moved to `0` |
| rubber band | double-click → move → click | horizontal span; `ALT` = vertical on the active group; `CTRL` = box (both) |
| scrollbar | double-click | full in that axis |
| wheel | over the trace area | horizontal zoom × 0.8/1.25 about the pointer |
| pan | `SHIFT+LEFT/RIGHT` | by 10 % of span |
| pan page | `F` / `B` | by 90 % forward / back |
| pan drag | drag the x-axis or scrollbar thumb | continuous |
| range band | double-click a range or section entry | zoom to that range |
| zoom summary bar | `SHIFT+click` | extend span to the click |

Pan and zoom are clamped to `map_M.outingExtent()` (never to 0), so panning or zooming out reaches the previous and next laps and a range-band double-click on an adjacent lap zooms to `[xOf(start), xOf(end))` through the Main mapper; the scrollbar thumb is `zoom.length()` over the outing extent; scrollbar offsets survive resize (span kept, not pixels). Every zoom change goes through `WorksheetViewModel::setZoom`, which forwards to the shared `WorkspaceViewModel` (§2.1 item 3), updates `zoomFrac` and emits `Change::Zoom` to every link group.

### 8.7 Measurements (`M`)

Per visible trace and slot over the visible span: `min`, `max` (with their positions) and `avg` of valid samples, computed on the worker from raw samples in regimes A/B and from pyramid `lo/hi` plus a raw pass for `avg` in regime C (the `avg` pass streams the chunks once per trace with the progressive pinning of §4.5; ≤ 1 M samples per trace at 1 kHz × 1,000 s, ≈ 1 ms). Results are drawn in the key column; hover/double-click per §8.4.

### 8.8 Variance panel (`F3`, D-023)

1. Shown only when `variance.show` and at least one overlay is visible; height `variance.heightFrac`; `variance.autoScale` or manual `[min,max]` in seconds (distance mode) or metres (time mode).
2. For each visible overlay `k` vs Main: distance mode `Δt(d) = tAtD_k(d) − tAtD_M(d)`, time mode `Δd(t) = dAtT_k(t) − dAtT_M(t)`, sampled per device-pixel column through `esses_analysis::VarianceTable::build(mainIndex, overlayIndex)`; alignment offsets are applied before lookup; drawn in `k`'s key colour as a `Polyline`. Variance is lap against lap: it is drawn only over the Main range `[0, extent())` and left blank where the visible span extends into adjacent laps (§8.6).
3. Missing `DistanceIndex` on either side → no trace and the banner text "variance needs distance data".
4. Test: equals a brute-force reference within ± 1 ms at every column (M3 acceptance).

### 8.9 Offset axis (`O`)

Per §3.3. The offset axis draws the nominal axis plus two draggable sub-axes (Main in the Main colour, reference overlay in its key colour); the offset amount is written at the right end of each sub-axis.

### 8.10 Status strip (`E`) and device events (`SHIFT+E`)

1. `statusChannels[]` (default: channels with `SampleKind::Bitfield/Bool` whose name matches the profile's status patterns, plus `bit_decode` outputs); one 10 px row per channel, drawn as filled rectangles in the channel colour where the value is non-zero, blank otherwise; hover names the channel and the bit.
2. Device events are ordinary markers `Marker{cls = DeviceEvent, group = "DeviceEvents", comment = message}` (02 §4.3; firmware start, power cycle, logger start/stop, Pi `Burst Trigger`), reported by importers in `probe_json` `markers[]`; there is no separate events list. They are drawn in the marker band as small triangles with the comment as hover text (scheme role `deviceEvent`); `SHIFT+E` toggles `showDeviceEvents`.

### 8.11 Bands, annotations, markers

1. Range band: one cell per visible slot's range covering `[xOf(start), xOf(end))`, labelled with the range name (and source short name when several sources are selected); clicks never move the cursor; double-click zooms.
2. Section band: ranges of `bandGroup` (default `Outings:Laps:Track Sections:Default`) under the Main lap, coloured by `category` (scheme roles `sectionCorner`/`sectionStraight`), labelled when wider than the label advance.
3. Marker band: beacons (`BCN`, `SOL`, `EOL`) as ticks, annotations as rectangles with hover text, device events as triangles; right-click an annotation → Edit/Remove.
4. Annotations are stored in the sidecar at the Main source's absolute time (`annotations[] {tUs, text, channel}`), drawn as a dotted vertical line in the trace area plus the band rectangle, and remain visible when Main is offset (the line follows `xOf(tUs)` through the Main mapper). Component menu → Cursor → Add Annotation (undoable).
5. `SHIFT+F7` inserts a `BCN` marker with `prov = User` at the cursor (sidecar); ranges rebuild synchronously (D-014) and the band updates on the resulting `Data` change.
6. `showMarkers` and `showBands` persist; `bandMode ∈ sections | none`.

### 8.12 Channel label menu

Scale/Offset (`CTRL+SHIFT+O`) and Zero at cursor (`CTRL+SHIFT+Z`) create or update a `localMaths` `scale_offset` item in the sidecar whose output name equals the input name (self-maths, D-022), preserving the channel's `dps`; Filter (`CTRL+SHIFT+F`) creates a transient filter item that is never persisted; Properties (`CTRL+N`), Edit maths (`CTRL+SHIFT+M`), Status (`CTRL+SHIFT+S`), Hide, Remove.

### 8.13 Copy and export

`CTRL+SHIFT+C` copies `name<TAB>value<TAB>unit` rows for every configured channel at the Main cursor (Main slot; with a datum, two value columns and Δ). Export Data writes the graph's channels (CSV in MVP) with options per §7.2 item 8.

### 8.14 Persistence record (`body`)

```json
{ "display": { "traceStyle": "line", "lineWidth": 1, "pointSize": 2, "showGrid": true, "showMarkers": true,
               "showBands": true, "bandMode": "sections", "bandGroup": "Outings:Laps:Track Sections:Default",
               "overlapMode": "panels", "cursorStyle": "line", "cursorSnap": true,
               "key": { "mode": "labels", "pos": "left", "values": true, "stats": false },
               "showStatusStrip": false, "statusChannels": [], "showDeviceEvents": true,
               "variance": { "show": false, "heightFrac": 0.15, "autoScale": true, "min": { "v": -2, "unit": "s" }, "max": { "v": 2, "unit": "s" } },
               "axis": { "showX": true, "showY": true, "showXScrollbar": true, "showYScrollbar": true } },
  "groups": [ { "name": "Speed", "scaleMode": "channel", "edges": [0.0, 0.33], "lo": 0.0, "hi": 0.33,
                "lines": { "zero": { "enabled": true }, "min": { "enabled": false }, "max": { "enabled": false } },
                "traces": [ { "channel": "Corr Speed", "visible": true, "displayUnit": "km/h", "dps": 1 },
                            { "channel": "GPS Speed", "visible": true } ] },
              { "name": "Throttle/Brake", "scaleMode": "manual", "scaleMin": { "v": 0, "unit": "%" }, "scaleMax": { "v": 100, "unit": "%" },
                "edges": [0.33, 0.66], "traces": [ { "channel": "Throttle Pos" }, { "channel": "Brake Pres", "displayUnit": "bar" } ] } ] }
```

`edges` are used in `panels` mode, `lo/hi` in `overlapped` mode; both persist so `G` round-trips. i2 `MoTeC.TimeDistance` attributes map 1:1 (`TraceLineWidth`, `KeyMode/KeyPos`, `ShowKeyValues/Stats`, `BandGroup`, `OverlapMode`, `VariancePos/AutoScale`, `Edges Mode/TopEdge/BottomEdge` per-mille → fractions, `Lines Index 0/1/2` → zero/min/max); `TOD.*` and `TelemetryZoom*` go to `x-i2`.

### 8.15 Acceptance

Golden set per §6.7 item 3; hotkey rows for every entry of §8.5–§8.9 and §8.11; property tests of §4.9; budgets of §6.4; the "Saturday" walkthrough opens an ACC `.ld`, overlays fastest vs chosen lap in distance mode, toggles F9 twice and asserts `zoom` equality within 1 sample. Further Qt Test rows: in distance mode, `N` keeps the cursor at the same lap distance within one 0.25 m step (and the datum likewise); in distance mode `F2` shows the whole outing and `SHIFT+LEFT` from the Main lap start pans into the previous lap with traces drawn; switching to a worksheet in the other domain and back keeps the shared zoom within 1 sample; `X` exchanges cursor and datum positions.

---

## 9. `outing`

1. Shows the whole Main outing (the Main source's `Outings` range, every lap) using the same groups/traces body as `time_distance` (§8.14 minus `variance`, `key.stats`, `statusChannels`), drawn from `SummaryPyramid` only (§4.8), so it never pins chunks and renders within the first-paint budget.
2. Not zoomable: `UP/DOWN/F2/W`, wheel and rubber band are forwarded to the worksheet zoom; `links.zoom` is forced `true`.
3. The current `zoom` is drawn in the Dynamic layer as a translucent rectangle (`scheme.roles.cursorZoom`) across all panels; dragging its left/right ends calls `setZoom`, dragging its bottom edge pans at constant span; the cursor line is drawn through it.
4. `enableOverlays` (default `false`) additionally draws the visible overlays' outings aligned at lap start (time mode) — the only overlay behaviour; distance mode shows Main only with the x-axis in metres from the outing start (the Main source's `DistanceChain` with its origin at the outing start). The zoom rectangle of item 3 is the shared `zoom` shifted by the Main range's position in the outing (`−map_M.outingExtent().a`).
5. Lap boundaries (`BCN` markers) are drawn as ticks in the range band; double-click a lap zooms the worksheet to it.
6. Body adds `{ "enableOverlays": false }`; everything else as `time_distance`.

---

## 10. `scatter`

### 10.1 Behaviours

1. One X channel, N Y traces, optional colour channel Z (`BandAxis` gradient, `K` legend), optional gating channel (sample dropped when gating ≤ 0 or invalid), evaluated over the visible span of each visible slot on a union grid at the highest rate among X/Y/Z/gating with per-channel `Interp` (`ScatterBinner`, worker, `Priority::Visible`); a sample is dropped when X or Y is invalid.
2. Per-trace `yDelayUs` shifts Y relative to X (positive = Y later).
3. `G` cycles `layout ∈ overlapped | tiled | average`; `tiled` uses `tileCols` or auto columns `ceil(sqrt(N))`; `average` plots the per-sample mean of all visible Y traces as one series.
4. `S` cycles `style ∈ points | pointsTrend | trend`; trend per trace and slot: `trend.type ∈ none | linear | poly` with `order` 2–6, `throughZero`, least squares via `esses_analysis::TrendFit` (Eigen QR) on the sample set after gating; equation text (`significantDigits`, default 4) shown for Main only; Component menu → Copy Equation.
5. `intervalAverage { enabled, binWidth }`: mean Y per X bin drawn as a stepped polyline per trace and slot.
6. Scales: `x`, `y`, `z` each `{ mode: channel | manual, min, max }`; manual scaling survives copy/paste; `channel` uses the override or autoscale per §8.3.
7. Cursor: the Main point at the cursor sample is drawn as a ring (Dynamic); a datum point is drawn hollow (read-only); click → nearest Main point in screen space (`HitTester` over a 64 × 64 bucket grid of the point array) → `setCursor` on the link group; `Y` cycles the cursor style.
8. Zoom: box zoom (double-click–move–click) and wheel on the component's own axes (not the worksheet span); double-click an axis restores; the worksheet `zoom` only selects the data span.
9. Overlays are drawn beneath Main in their key colours; `densityMode` switches to a 2D histogram (`densityBins`, default 256 × 256; `Quads` per density level, 16 levels through the scheme gradient) automatically above `densityThreshold` (1,000,000 visible points) or on request.
10. Export CSV: `x,y[,z]` per visible point per trace and slot.

### 10.2 Persistence (`body`)

```json
{ "xChannel": "Corr Speed", "traces": [ { "yChannel": "G Force Lat", "visible": true, "yDelayUs": 0 } ],
  "colourChannel": { "channel": "Throttle Pos", "axis": { "mode": "even", "bandMode": "count", "bandCount": 8, "palette": "rainbow" } },
  "gating": { "channel": "Corner Gate" }, "layout": "overlapped", "tileCols": 0, "style": "points", "pointSize": 2,
  "trend": { "type": "linear", "order": 2, "throughZero": false, "showEquation": true, "significantDigits": 4 },
  "intervalAverage": { "enabled": false, "binWidth": { "v": 5, "unit": "km/h" } },
  "scales": { "x": { "mode": "channel" }, "y": { "mode": "manual", "min": { "v": -3, "unit": "G" }, "max": { "v": 3, "unit": "G" } }, "z": { "mode": "channel" } },
  "cursorStyle": "cross", "density": { "auto": true, "threshold": 1000000, "bins": [256, 256] }, "showGrid": true }
```

i2 `MoTeC.Scatter` attributes map directly (`XChannel`, `ZChannel`, `Trace YChannel/YDelay`, `RegressionType/Param/Zero/Digits`, `IntervalStat.BinWidth`, `TileMode/TileCols`, `ShowDensity/DensityBins`); `CursorExtent/CursorSlope`, `DensityFillColor*` and draw-size-by-channel go to `x-i2` (draw size is a v1.0 addition).

---

## 11. `histogram`

### 11.1 Behaviours

1. Bins from `bins: BandAxis` with `bandMode ∈ width | count`, `dataMin/dataMax` (`auto` = the active channel's scale per §8.3, or manual), `lessMinBand`/`greaterMaxBand` for out-of-range samples; `binLabelStep` thins labels.
2. Per visible slot and trace, over the slot's visible span, at the channel's own rate: `bin = floor((v − min)/width)`; `yMode ∈ count | percent | time` (percent of accepted samples; time = Σ sample periods in seconds); gating (resampled `Previous` to the channel rate, dropped when ≤ 0 or invalid); `excludeUntrusted` skips ranges with `trusted = false`.
3. `S` cycles `style ∈ bar | line | text`; `text` renders a table (rows = bins, columns = traces) of `TextRun`s; `stackLines` offsets line styles vertically.
4. `G` cycles `layout ∈ overlapped | tiled`; the active channel is drawn last/on top.
5. Colour channel (`SecondChannel`): each bar splits into sub-bars by the colour channel's `BandAxis` band at each accepted sample; `K` shows the legend.
6. Y scale `{ mode: auto | manual, min, max }`; auto = max bin over visible traces and slots (gating respected).
7. Cursor: the bin containing the active channel's value at the cursor is highlighted (Dynamic); no datum.
8. Overlays draw as outlined bars (bar style) or lines in key colours beneath Main.
9. Computation: `HistogramBinner` on a worker (`Priority::Visible`), coalesced by component key; one pass over pinned chunks per trace and slot.
10. Export CSV: `bin_lo,bin_hi,<trace@slot>…` rows.

### 11.2 Persistence (`body`)

```json
{ "traces": [ { "channel": "Damper Vel FL", "visible": true } ],
  "bins": { "mode": "even", "bandMode": "width", "dataMin": { "v": -250, "unit": "mm/s" }, "dataMax": { "v": 250, "unit": "mm/s" },
            "bandWidth": { "v": 10, "unit": "mm/s" }, "lessMinBand": false, "greaterMaxBand": false },
  "yMode": "percent", "yScale": { "mode": "auto" }, "excludeUntrusted": true, "stackLines": false,
  "style": "line", "layout": "overlapped", "tileCols": 0, "binLabelStep": 2, "condensedAxes": false,
  "colourChannel": { "channel": "", "axis": null }, "gating": { "channel": "" } }
```

Maps i2 `MoTeC.Histogram` (`HistoMainBins/EvenWidthBandAxis`, `YUnitMode`, `YExcludeUntrusted`, `StackLines`, `BinLabelStep`, `SecondChannel`, `HistoColorBands`, `Tile*`).

---

## 12. `channel_report`

1. Widget: `QTableView` with a frozen first column over a `ReportModel` (`esses_qtmodel`) fed by `esses_analysis::ReportEngine` through `StatCache` keyed `(channelId, version, rangeId, stat)`; cells compute on workers at `Priority::Background`, appear incrementally ("…" while pending); Print to Clipboard uses `QWidget::render`.
2. `ReportSpec`: `scope ∈ selectedRanges | selectedOutings | allOutings`; `headers[]` = ordered range-group paths that divide the data (any order, e.g. `["Outings", "Outings:Laps:Track Sections:Default", "Outings:Laps"]`); `channels[] { name, stats[] }` with `stats ⊆ { avg, min, max, absMax, start, end, change, stdDev, delta }` (`delta = max − min`); `options { excludeOverlays, includeUntrusted, interpolateBoundaries, probeSeconds = 5, zoomLinked }`; `layout ∈ channelsInRows | channelsInColumns`; `highlight { min { on, colour }, max { on, colour } }`; `deltaVsOverlay0`; `timeFormat`.
3. Statistics skip invalid samples; `start/end` probe up to `probeSeconds` inward for a valid sample; `interpolateBoundaries` evaluates the boundary value by the channel's `Interp` at the exact range edge; `stdDev` is computed in base units then converted (display-unit invariance).
4. `zoomLinked = true` clips every range to the worksheet `zoom` span (through each range's mapper); otherwise the component ignores `links`.
5. `deltaVsOverlay0` adds a `Main − Overlay 0` column pair per statistic (paired by header path; missing pairs blank). It uses Overlay 0 only, as the Channel Report help defines the delta: when Overlay 0 is unset (or overlays are hidden with F4) the delta columns are blank even if numbered overlays exist; the lowest-numbered-overlay fallback of D-017's reference overlay does not apply to this column.
6. Multiple outings list in session date/time order; untrusted ranges are excluded unless `includeUntrusted`; header rows show range names and, for section headers, the section name. The header picker of the Report tab offers the profile's `reportHeadings` (02 §6.9): the detail groups `Events`, `Vehicles`, `Sessions`, `Driver`, `Vehicle Id`, `Vehicle Number`, then `Outings`, `Outings:Laps` and the track-section groups with their category children (`…:Track Sections:Default:Corners`, `…:Default:Straights`, `…:<set>:<category>`, 02 §4.5). Ranges of equal name across sources merge under one heading, ordered by `Log Date`/`Log Time`.
7. Export CSV / copy TSV with quoting of `"`, `,`, `%` and newlines; the Excel live link, report charts and Setup Sheet dependency are v1.0.
8. Persistence (`body`): `{ "report": { "scope": "selectedRanges", "headers": ["Outings:Laps"], "channels": [ { "name": "Corr Speed", "stats": ["max", "avg"] } ], "options": { "excludeOverlays": false, "includeUntrusted": false, "interpolateBoundaries": false, "probeSeconds": 5, "zoomLinked": false } }, "layout": "channelsInRows", "highlight": { "min": { "on": true, "colour": "#2060ff" }, "max": { "on": true, "colour": "#ff3030" } }, "deltaVsOverlay0": false, "timeFormat": "project", "splitter": { "mode": "none", "pos": 0.5 } }` — maps i2 `LapReportGen/ReportRange HeaderGroup`, `SummaryGenerator rptsummary.min|max`, `Formula rngmaths.float.*`, `DataExtent` (0/−1/1 → scope).
9. Acceptance: cells equal hand-computed fixtures (M3); 50 laps × 30 channels × 8 stats cold ≤ 300 ms.

---

## 13. `time_report`

1. Rows = laps in scope (`scope ∈ selectedRanges | selectedOutings`); columns = sections of `rowGroup` (default `Outings:Laps:Track Sections:Default`; user section sets; Drag splits in v1.0); cell = `section.end − section.start` of the section range under that lap; last column = lap time; missing section → "—".
2. Summaries: `eclectic` column = min per section with its total (theoretical best); `rollingMin` row = over the chronological concatenation of all section times in scope, the window of `sectionsPerLap` consecutive sections with the minimum sum, shown rotated to section positions; `total` row = sum per column. Summaries and band thresholds use only the rows admitted by item 4.
3. `bands[] { pct, colour }`: a cell is tinted when `cell ≤ fastest(section) × (1 + pct/100)`; multiple bands apply in list order; `ALT+n` toggles `bands[n].visible`.
4. `options.includeUntrusted` (default `false`; i2: reports can include or exclude untrusted laps) decides whether laps with `trusted = false` (in/out, partial, pit by default) are rows at all; when included they are marked italic and feed the summaries like any row. Recomputes on `Selection` and `Data` (beacon offsets, trust edits rebuild ranges synchronously and publish a new source version).
5. **Section origin for laps that do not start at a beacon.** Section ranges come from the track-sections rule (02 §4.5). For a lap whose start is not a `BCN` (out lap `SOL→BCN`, a partial lap after `RESET`) the rule must position sections **backwards from the end beacon**: a section at track distance `s` lies at lap distance `lapLength − (trackLength − s)`; sections that would start before the lap start are omitted ("—"), never placed from the pit-lane start. Laps starting at a beacon (flying, in) measure forward as usual. This keeps bogus out-lap section times out of the eclectic even when untrusted laps are included. (XREF 02 §4.5.)
6. Times in `timeFormat` (`project | s.mmm | m:ss.mmm`); export CSV/TSV.
7. Persistence (`body`): `{ "scope": "selectedOutings", "rowGroup": "Outings:Laps:Track Sections:Default", "options": { "includeUntrusted": false }, "summaries": { "eclectic": true, "rollingMin": true, "total": true }, "bands": [ { "pct": 1.0, "colour": "#30c030", "visible": true }, { "pct": 3.0, "colour": "#c0c030", "visible": true } ], "timeFormat": "project" }` — maps `SectTimesReportGen RowGroupName`, `FilterRanges`/template `ExcludeUntrusted` → `options.includeUntrusted` (negated), `rptsummary.min|near.min|contiguous.min|total`.

---

## 14. `track_position`

1. Data: the `TrackMap` (`trackmaps/<venue>.track.json`) bound by the Main source's Venue detail; absent map → Chrome banner "no track map (Tools → Track Editor)".
2. Static: centreline (`Polyline`, scheme role `trackLine`, 06 §16), optional inner/outer, section colouring (`showSectionColours`, by category), section marks (`showSectionMarks`, ticks at section starts) and labels (`showSectionLabels`); `TrackGeometry` fits the map to the component with `scale { mode: fit | manual, zoom, panX, panY, rotation }`.
3. Dynamic: the visible `zoom` span highlighted as a thicker sub-polyline (role `cursorZoom`) between the track positions of `zoom.a` and `zoom.b` (`showZoomSpan`; it may cross the start line when the span covers adjacent laps); cursor dots per visible slot: Main red dot at the track position of `main.start + cursorT`, overlay dots at that of `k.start + cursorT` in time mode (independent positions), all at the same distance in distance mode (i2 rule); dot sizes from `sizes { cursor, detail, material }`.
4. `PLUS/MINUS`/wheel zoom and drag-pan while focused; double-click resets to fit; "Send to background" lowers `z` so components placed over it stay on top; mouse clicks on a backgrounded map are not consumed.
5. Track position of a time `t`: the lap `p` containing `t` (through the slot's `DistanceChain`, so the cursor may be in another lap than Main) and its stretched `DistanceIndex` distance; for a lap that does not start at a beacon (out lap, partial) the position is measured backwards from its end beacon, `trackLength − (lapLength − d)`, the same origin rule as §13 item 5. When the lap has no index the dot is hidden and the banner shows.
6. Persistence (`body`): `{ "showZoomSpan": true, "showSectionLabels": false, "showSectionMarks": true, "showSectionColours": true, "sizes": { "cursor": 6, "detail": 2, "material": 2 }, "scale": { "mode": "fit", "zoom": 1.0, "panX": 0, "panY": 0, "rotation": 0 }, "background": false }` — maps `MoTeC.TrackPos TrackMapWindow/SizeScheme`.
7. The Track Map dock embeds the same widget with `links` forced to the active worksheet.

---

## 15. `values_list`

1. `QTableView` over `ValuesModel` (shared with the Values window): rows = `channels[]` of the body; columns Name · Main · Reference overlay (header shows its lap name) · Δ (`delta`) · Datum (`datum`, value@datum of Main) · Unit (`inlineUnits`).
2. Values at the cursor through §3.2 with each channel's `Interp`; enum labels; Pending maths "…" with evaluation queued at `Priority::Background` while the row is visible; ≤ 2 ms refresh for 250 rows (values read from chunks pinned with `Column::tryPin`, 01 §5.3 rule 5).
3. `showHeading`, `autoSizeColumns`, column widths persist; fonts from the scheme font roles `valuesHeader`/`valuesItem` (06 §16), the same as the Values window whose `ValuesModel` this component embeds.
4. Rows are added like graph channels (Channels window, drag, Properties); the active row supports the active-channel hotkeys.
5. Persistence (`body`): `{ "channels": [ { "channel": "Engine RPM", "displayUnit": "rpm", "dps": 0 } ], "columns": { "datum": false, "delta": true, "inlineUnits": true }, "showHeading": true, "autoSizeColumns": true, "widths": { "name": 120, "value": 80, "delta": 80, "datum": 80 } }` — maps `MoTeC.List Datum Delta ShowHeading AutoSizeColumns *ColumnWidth`.

---

## 16. `gauge_numeric`

1. One channel; value at the cursor for Main (large, `dynamicFont` scales the point size to fit the rect with a 6 pt minimum) and the reference overlay beneath in its key colour when present (`MainAndReference`); `showUnits` appends the display unit; `dps` from the channel override unless `displayDps` set.
2. `colourBands`: the band containing the value colours the value text (`bandMode = text`) or the background (`bandMode = background`); `ALT+n` toggles bands.
3. Layers: Static = frame and caption; Dynamic = value texts (whole component is cursor-driven).
4. Persistence (`body`): `{ "channel": "Engine RPM", "displayUnit": "rpm", "displayDps": 0, "showUnits": true, "dynamicFont": true, "font": null, "colourBands": [ { "min": { "v": 7000, "unit": "rpm" }, "max": { "v": 0, "unit": "rpm" }, "open": "above", "colour": "#ff3030", "visible": true } ], "bandMode": "text" }` — maps `MoTeC.NumericGauge UseDefaultFont Font DynamicFont ShowUnits`, `GaugeData`, `ColorBandGroup`.

---

## 17. `gauge_bar`

1. One channel; bar from `originStyle ∈ min | zero | max | custom(customOrigin)` to the value; `orientation ∈ vertical | horizontal`; scale `{ mode: channel | manual, min, max }` (channel = override or autoscale §8.3); `scaleDps`, `showScale` tick labels.
2. Main bar in `barColour` (default channel colour); the reference overlay drawn as a thin marker line across the bar in its key colour; value text at the bar end.
3. `colourBands` drawn as band lines (`bandLines ∈ none | lines | fill`) along the scale; a band containing the value may recolour the bar (`bandMode = bar`).
4. Layers: Static = scale, bands, frame; Dynamic = bar and marker.
5. Persistence (`body`): `{ "channel": "Brake Pres", "displayUnit": "bar", "scale": { "mode": "manual", "min": { "v": 0, "unit": "bar" }, "max": { "v": 100, "unit": "bar" } }, "orientation": "vertical", "originStyle": "min", "customOrigin": { "v": 0, "unit": "bar" }, "barColour": null, "scaleDps": 0, "showScale": true, "colourBands": [], "bandLines": "lines", "bandMode": "none" }` — maps `MoTeC.Bar ScaleMode Min/MaximumValue Style OriginStyle CustomOrigin ColorBanding ColorBandLines* BarColor ScaleDPS`.

---

## 18. Deferred components (v1.0 and later)

**`spectrum` (FFT, v1.0).** Traces over the visible span of each slot; `fftPoints` (power of two, default 1,024), `window ∈ rectangular | hann | hamming | blackman | flatTop`, Bartlett averaging of consecutive zero-padded blocks when the span exceeds `fftPoints`, `smoothingPct` adjacent-bin averaging (first/last bins dropped), log/linear axes with auto/manual scales, PSD output in `unit²/Hz` (oracle `pwelch(x, N, 0, N, r)`), overlapped/tiled, cancellable long computations. Depends on `esses_analysis::Spectrum` (pocketfft), `Resampler` to a uniform rate, the summary-component recompute path (§7.4), a `TextRun` axis in Hz.

**`suspension_histogram` (v1.0).** A `histogram` subclass with four damper-velocity channels, a Velocity Calculation tab (built-in `esses.damper_velocity` module: position → filtered derivative, filter window exposed) or user maths, `bumpSign ∈ positive | negative`, a Hi/Lo speed boundary channel-or-constant drawn as a band, and statistics Rebound/Bump Avg, Hi %, Lo % in a text panel. Depends on `histogram`, the maths module registry (D-024), the gating path.

**`mixture_map` (v1.0).** A `scatter` subclass: X = Engine RPM, Y = Lambda (shifted by `lambdaDelayUs` through the resampler), one series per load band from `loadChannel` + `BandAxis` (`centred` bands matching ECU load sites), per-band trend lines toggled by `ALT+n`, gear-change filter (`rpmRateThreshold`, `rpmFilterTimeUs`, `gearChangeDelayUs` exclusion extended on re-trigger) implemented as a derived gating series, gating channel. Depends on `scatter`, `TrendFit`, a `derivative` kernel for the RPM rate.

**`track_report` (v1.0).** `track_position` plus layered contour channels (`BandAxis` per channel, first on top, gaps where a channel is invalid/zero let lower layers show; needs `QuadsRgba` or per-segment `Polyline` colouring), notes per section or on every change of a channel (`FormulaClass` range-stat vs on-change) with statistics `avg/min/max/absMax/start/end/change/stdDev` plus extra channel values at the statistic's instant, section-type filter, automatic placement along the section-midpoint normal with collision nudging, drag-to-move freezing all notes until "Automatic note placement", side-by-side Main/Overlay maps with independent cursors in time mode. Depends on `ReportEngine` (instant-of-statistic output), `TrackGeometry` offsets, `HitTester` for notes, sidecar-free note positions in the body.

**`rainbow_track` (v1.0).** `track_report` with one contour channel and no notes; the track polyline coloured per segment by the channel value through a `BandAxis` LUT (`QuadsRgba` extension or one `Polyline` per band); side-by-side overlays as above. Depends on `track_report`'s contour renderer.

**`gps_track` (v1.0).** Lat/lon path per visible slot, read through `MathsService` as the `esses.gps_corrections` chain head, never from the raw logged columns, so (0, 0) fixes are already invalid and corrected channels are used when the module is enabled (03 §9.2) (F64 columns, equal rates required else the pair is resampled to the slower), projected to ENU about the track map origin or the first valid fix; scale modes `fitSelected | fitZoomed | manual`; heading arrows every `arrowSpacingM` from a heading channel or the path tangent; track map background when the map was generated from GPS; `trackWidth`; tiles later behind an `ITileProvider`. Depends on GPS corrections (Corrected GPS dialog, v1.0), `TrackGeometry` projection helpers, KML export sharing the same projected path.

**`gauge_dial`, `gauge_light`, `gauge_wheel` (v1.0).** Dial: arc scale with `majorDivisions/minorDivisions`, `scaleExponent`, needle per slot (Main + reference), colour bands on the dial or the needle. Light: threshold/`activeLow` or band test → fill colour with `statusText`, later alarm flashing. Wheel: steering-angle rotation of a wheel glyph with `inversePolarity`, colour bands. All are `MainAndReference`, Static frame + Dynamic indicator, and share `ColourBands` and the gauge property tabs.

**`notes` (v1.0).** Free text (`QPlainTextEdit`, body `{ "text", "font" }`), no data binding, excluded from goldens except layout; Print to Clipboard via `QWidget::render`.

**`picture` (v1.0).** Static image from a project-relative path (`{ "path", "fit": "contain | stretch | centre" }`), rendered through `FilledRect`-free direct `QPainter::drawImage` in the Static layer; missing file → placeholder.

**`video` (later).** `IVideoPlayer` over Qt Multimedia bound to a Camera View from the sidecar `videoLinks` (PTS ↔ log-time offset model), follows `cursorT` and animation, padlock unlock for manual sync with frame nudges, only the focused component plays sound, one component per camera, overlay videos when "Show overlays". Depends on `esses_video`, sidecar `videoLinks`, the `Dispatcher`-posted frame clock, sync-point display in `time_distance` (`showVideoSyncPoints`).

---

## 19. Test matrix summary

| Kind | Location | Covers |
|---|---|---|
| Property (Catch2) | `tests/plotcore/` | M4 vs brute force on the chunk path and the summary-path bound (§4.9), `DomainMapper`/`DistanceChain` round trips in both domains, F9 round trip with and without index, `AxisLayout` tick monotonicity, `HitTester` consistency with emitted geometry, `BandAxis` band generation, `AutoscaleCache` trust/overlay unions |
| Concurrency | `tests/qtrender/` | `QtTextMetrics` from 8 threads during a scheme swap (Linux TSan build with Qt, §5.4) |
| Golden images | `tests/golden/` | §6.7 item 3, DPR 1/2 (+1.5 subset), light/dark |
| Qt Test (offscreen) | `tests/widgets/` | one row per hotkey in §7.2, §8.5–§8.9, §8.11, §10, §11, §14; like-units grouping; N/P and F9 preservation (both domains, cursor and datum included); outing-wide F2/pan in distance mode; overlay colour resolution; pending/missing labels |
| Benchmarks | `bench/plot_bench.cpp` | §6.4 budgets per stage; `pinnedBytes` bound of §4.9 |
| Fixture cells | `tests/analysis/reports/` | `channel_report`/`time_report` cells vs hand-computed values (incl. `includeUntrusted` both ways and an out lap's sections); variance ± 1 ms |

Every i2-derived behaviour cited above is recorded in `CLEANROOM.md` with its source category: hotkeys, colour rules, band semantics, report functions, beacon/section definitions → "help page / public release notes, behaviour restated, no MoTeC text reproduced"; the `x-i2` attribute mappings of §8.14–§17 → "vendor-shipped workbook templates, attribute names only, read-only, not redistributed".

---

## 20. Discrepancies and proposed decisions

1. `ITextMetrics` (§5.4) is a new `esses_plotcore` name required by the Qt-free text-run rule; it must be added to `ARCHITECTURE_OUTLINE.md` §1/§4.
2. `cursorT` is defined as Main-lap-relative (µs since Main start); D-018 says "Main-lap time" without fixing absolute vs relative; relative is chosen because it makes N/P preservation and the per-slot conversion rule (§3.2) trivial. On a Main change in distance mode cursor and datum are converted through the old and new Main mappers so they keep their lap distance (§2.1 item 1), and they are clamped to the outing, not the Main range.
3. F9 fallback extent without a `DistanceIndex` (§3.4): track-map length when bound, else 1.0 with labels suppressed — proposed decision, not in D-016.
4. Offset arrow step (§3.3 item 3) = one sample period of the active channel (time) / one 0.25 m `DistanceIndex` step (distance), UP/DOWN = ×10 — proposed (i2 says only "arrow"); aligned with 06 §14.4 and 07 AC-M3-09.
5. M4 in-column `lo/hi` order (§4.7) is an approximation documented here; D-012 claims exactness for `min/max/first/last`, which holds; vertical extent is exact, sub-pixel order is not recoverable from a pyramid.
6. Summary path edge approximation (§4.8): `lo/hi` enclose the exact reference and extra samples lie within one 256-sample bucket (≤ 1/8 column) of the boundary, only for `outing`/full-extent draws when a chunk cannot be pinned without blocking; D-012 otherwise forbids documented bounds. Workers use `tryPin`, never `tryChunk`.
7. Overlapped-mode `lo/hi` are fractions of the trace-area height, 0 = top (research open question).
8. Mixed manual/auto channels in a Channel/Auto group combine per channel (§8.3 item 2) — research open question.
9. Rolling-minimum definition (§13 item 2) is an interpretation of the help text.
10. Gauges and `values_list` show Main plus the reference overlay only (§7.4) — research open question "which gauge types are restricted".
11. Time-of-day axis (`TOD.*`) is v1.0; stored in `x-i2` until then.
12. parity-first §5.1 (16× pyramid steps, 16 spp raw threshold) is superseded by D-012 (factor 4, 32 spp); parity-first's `Renderer` using `QPainterPath`/`QString` is superseded by the Qt-free `DrawList` (D-007/D-025).
13. The outline §4 lists `LayerCache` as three `QImage`s; this document caches only the Static image and paints Dynamic and Chrome lists directly under a damage region (§6.2), because a full-size Dynamic image makes the 4 ms cursor budget unreachable at 3,840 × 2,160; layer memory drops to 1 × 4 B × device area per component (01 §7.1 follows). The hidden-worksheet release rule is D-058's 30 s timer (§6.2). Proposed as an amendment of D-025/D-058 (DECISIONS.md Addendum A, D-137).
14. D-016 defines `tAtD[]` in seconds while all other times are `Micros`; `DomainMapper` converts with `llround(· 1e6)` — the analysis document should confirm the unit.
15. Overlay 1..n default style is plain channel colours (§2.3) pending the D-017 verification on i2.
16. Proposed (DECISIONS.md Addendum A, amends D-018): zoom, cursor and datum are one workspace-level shared view; only `domain` and `offsetAxis` are per worksheet (§2.1 item 3). Ground truth: one current zoom level shown above the worksheets; only the time/distance mode is remembered per worksheet. Whether cursor and datum also persist across worksheet switches is a D-005 (3) verification item.
17. Proposed (DECISIONS.md Addendum A, extends D-016): `DistanceChain` (§3.1) gives an outing-level distance mapping so F2, panning into adjacent laps and Main = Outing work in distance mode; the view is clamped to the outing extent, not `[0, …)`.
18. Autoscale (§8.3 item 3) covers the Main source's file (trusted laps unless `includeUntrusted`) plus overlay sources, per the Options/Laps help, instead of the Main range only; cached so N/P do not rescale.
19. `channel_report` Δ columns use Overlay 0 only (§12 item 5), narrower than D-017's reference-overlay rule, per the Channel Report help.
20. `time_report` gains `options.includeUntrusted` (default false) and the out-lap section origin rule (§13 items 4–5); the track-sections rule in 02 §4.5 must implement the backwards-from-end-beacon origin.
21. New seams named here, to be added to the outline and owning documents: `RenderService` (`esses_qtmodel`), `WorkspaceViewModel` (`esses_qtmodel`), `AutoscaleCache` and `DistanceChain` (`esses_plotcore`), `ThreadRole::NoBlock` (`esses_base`; the 01 §5.3 rule 4 assertion checks it), `Column::tryPin` and `Column::prefetch` (`esses_store`, 02 §3.2), `QtTextMetrics` (`esses_qtrender`).
22. Pyramid entry types (§4.1): resolved for D-054 and 01 §7.1 (`OPEN_QUESTIONS.md` F-2): entries are `MinMax{float lo, float hi}` for every dtype, rounded directionally for wider dtypes; 02 §3.1 and §4.1 here now agree. `M4Column` holds float offsets from a per-trace double origin.
