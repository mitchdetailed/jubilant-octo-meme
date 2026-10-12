# esses_plotcore

Layer 3, Qt-free (01 §2.1, 04 §§2-5; D-012, D-018, D-025). The plot core: view state, change masks, the
selection snapshot, domain mapping, exact M4 trace building, layout builders, hit testing, the `DrawList` IR and
the render-backend and text-metrics seams. No Qt and no text rasterisation: text leaves as UTF-8 runs with a
style id. M1 scope: 07 §4.1 (`BandLayout`, `AutoscaleCache`, the geometry builders of the summary components
later).

Namespace `esses::plot` (01 §1); public headers in `include/esses/plotcore/` (umbrella `plotcore.hpp`). Links
`esses_base`, `esses_units`, `esses_store`, `esses_model`, `esses_analysis`, `esses_project`; never
`esses_maths` (D-048 as amended by D-122; enforced as a forbidden edge). Built with `-ffp-contract=off` on
GCC/Clang so the `DrawList` goldens match MSVC bit for bit.

| Header | Contents | Design |
|---|---|---|
| `view_state.hpp` | `Domain`, `XSpan`, `ViewState` | 04 §2.1 |
| `change_mask.hpp` | `Change`, `ChangeMask`, `operator|`, `has`, `mask`, `kDynamicOnly`, `kStaticRebuild`, `kFullRebuild` | 04 §2.2, 01 §5.5 |
| `selection_snapshot.hpp` | `SlotKind`, `Slot`, `SelectionSnapshot`, `traceColour`, `traceDash`, `traceWidthScale`, `adjustLightness` | 04 §2.3, D-017, D-100 |
| `domain_mapper.hpp` | `RangeGeom`, `OutingGeom`, `ILapDistance`, `LapPiece`, `DistanceChain`, `DomainMapper`; F9: `mainExtentIn`, `outingExtentIn`, `convertZoom` | 04 §3, D-016, D-134, AC-M1-09 |
| `trace_builder.hpp` | `M4Column`, `M4Flag`, `TraceStyle`, `TraceSpec`, `YMap`, `TraceJob`, `DecimatedTrace`, `TraceBuilder` (`decimate`, `decimateSummary`, `decimateMany`, `emit`) | 04 §4, D-012 |
| `draw_list.hpp` | `LayerKind`, `CmdKind`, `DashId`, `TextStyleId`, `TextAnchor`, `RectPx`, `LayoutContext`, `TextRun`, `DrawCmd`, `DrawList` (builder helpers, `append`, `toJson`) | 04 §5.1-5.2 |
| `text_metrics.hpp` | `ITextMetrics`, `FixedTextMetrics` | 04 §5.4 |
| `render_backend.hpp` | `LayerHandle`, `Bitmap`, `IRenderBackend` | 04 §5.3 |
| `axis_layout.hpp` | `AxisKind`, `TimeFormat`, `AxisSpec`, `AxisTick`, `AxisLayout` | 04 §5.5 |
| `key_layout.hpp` | `KeyValue`, `KeyRow`, `KeyGeometry`, `KeyLayout` | 04 §5.5, §8.4 |
| `hit_tester.hpp` | `HitKind`, `HitResult`, `HitTester` | 04 §5.5 |
| `component_build.hpp` | `ResolvedChannel`, `SlotData`, `ComponentBuildInputs`, `ComponentBuild` | 04 §5.5, §6.3 (added) |
| `time_distance_builder.hpp` | `ScaleMode`, `TdTrace`, `TdGroup`, `TimeDistanceBody`, `TdGroupLayout`, `TdLayout`, `TimeDistanceBuilder` (`buildStatic`, `buildDynamic`, `buildChrome`, `layoutOf`) | 04 §8 (added) |

## Behaviour notes

- **Exact M4** (`trace_builder.hpp` has the full contract). Column spans use the exact integer form of 04 §4.3:
  `i_c` is the first sample whose time is `>= map.timeAt(x_c)` (D-129 arithmetic for uniform series,
  `lower_bound` over `timesUs` for timestamped ones), so uniform and timestamped traces share one rule. Regime C
  folds `store::chunkRangeMinMax` from `descentLevel(n_c, 8)` per chunk; `first`/`last` are the first and last
  valid samples (bitmap scans). One chunk is pinned at a time (`Column::require`), the next is prefetched, the
  token is checked every 256 columns and at every chunk transition.
- **Gaps**: `Empty | HasInvalid` breaks a polyline, a bare `Empty` (no samples) is joined across; regimes A/B
  break at the column edge an invalid sample touches (`BreakBefore`/`BreakAfter`); regime C never breaks on
  partial invalidity. In distance mode the samples of laps without a distance index are excluded and break the
  line like invalid samples, in regime A too (an excluded run before or after the column's one sample sets
  `BreakBefore`/`BreakAfter`, which `emit` honours for raw columns). Open (M2, with the first timestamped
  importer): a bare `Empty` column of a timestamped trace is also joined across a logging hole; breaking needs
  a gap threshold (D-016's `dataGapUs` or a multiple of the median period) that the design has not fixed yet.
  `decimateSummary` (the summary path of 04 §4.8) is tested (`m4_property_tests`) but not yet called by
  `TimeDistanceBuilder` and does not apply the distance-mode exclusions; both follow when it is wired for
  columns with n_c >= 2,048 (M2).
- **Distance**: `DistanceChain` keeps `R_i` relative to the origin piece, so inside the origin lap it equals the
  lap's index exactly and the next lap starts exactly at the origin lap's stretched length; an origin inside a
  lap (section or outing ranges) is supported. `LapPiece::lookup` (an `ILapDistance`) can stand in for
  `analysis::DistanceIndex`.
- **F9**: `convertZoom` implements `WorksheetViewModel::setDomain` (and the D-133 worksheet switch): through the
  Main chain when it covers the span, else the `zoomFrac` rule, then the outing clamp. `esses_qtmodel` should
  call it instead of re-deriving the rule. M1 integration fix: `outingExtentIn` uses a partial chain's metres
  only when the chain covers the Main range; otherwise (the Main lap's index still pending while another lap of
  the outing has one) the outing is in the fraction rule's normalised units like `mainExtentIn`, so the clamp no
  longer turns a fraction-rule span into the indexed lap's extent (`[-4868 m, 0]` on the ACC stand-in when F9
  came before the Main lap's index).
- **Parallel emit** (M1 integration, AC-M1-02): `TimeDistanceBuilder::buildStatic` emits each drawn trace into
  its own sub-list on the TaskPool (like `decimateMany`) and appends the sub-lists verbatim in the draw order, so
  the list equals an in-place emission (the `DrawList` JSON goldens are unchanged).
- **time_distance**: one `layoutOf` shared by the three layers (caption 18, key column via `KeyLayout`, axis
  strip 36, panels by `edges`, x axis 18; logical px × dpr, snapped to device pixels). Channel/Auto scales use
  the Main source's whole-column `SummaryPyramid` min/max (the M1 stand-in for `AutoscaleCache`) and fall back
  to the drawn columns while the summary is incomplete. The Static build fans its traces out with
  `decimateMany` on `ComponentBuildInputs::pool` (else `TaskPool::currentWorkerPool()`, else in order). Key
  values are one line per visible slot. The "no distance data" banner is Chrome (04 §5.2).

## Tests (`tests/`, Catch2, ctest `plotcore.*`)

- `m4_property_tests.cpp`: 04 §4.9 property tests: decimate vs a brute-force per-column reference, bit for bit
  (F32, F64, I32; uniform and timestamped; NaN sprinkles, invalid runs, an all-invalid chunk, a partial last
  chunk; 0.1 to 10^6 samples per pixel; time and distance mode with plateaus and a lap without an index); the
  summary path's bound with and without resident chunks; `decimateMany` on a `TaskPool` vs sequential;
  cancellation.
- `domain_mapper_tests.cpp`: `DistanceChain` continuity and round trips over three laps with different
  stretches, plateaus, partial chains; time-domain exactness; both F9 rules (AC-M1-09). A test against the real
  `analysis::DistanceIndex::fromSamples` runs once `esses_analysis` implements it (it warns while it is a stub).
- `layout_tests.cpp`: `ChangeMask`, slots and D-017/D-100 colours, `DrawList` JSON, vertex emission, `AxisLayout`,
  `KeyLayout`, `HitTester`.
- `time_distance_tests.cpp`: the body round trip, layout/draw order/colours/hit regions, Dynamic values and
  pending state, the banner, and the **`DrawList` JSON goldens** of the eight M1 worksheet cases at DPR 1 and 2
  (AC-M1-04): `tests/golden/drawlist/time_distance/<case>@<dpr>x.json`. The cases (`tests/td_golden_cases.hpp`,
  synthetic rational data, `FixedTextMetrics`, schemes built in code) are reusable by the pixel goldens.
  `ESSES_UPDATE_GOLDENS=1` rewrites the files (they are platform independent); a mismatch writes
  `<case>@<dpr>x.actual.json` next to the test binary.

## Deviations from the design documents

- `TraceSpec` holds `store::Column` handles by value where 04 §4.2 writes `const Column*` (a copied spec cannot
  dangle on a worker, 01 §5.3 rule 9) and gains `dash`; `M4Flag` scopes the flag constants and gains
  `BreakBefore`/`BreakAfter`; `M4Column` gains `xFrac` (the regime-A sample's exact sub-pixel x). `Clipped` is
  never set (emit works on const columns; the clamp is applied).
- Style `Points` draws regime-B columns as vertical lo-hi segments like regime C (the M4 column keeps no raw
  samples); isolated samples between gaps are drawn as dots.
- `TdGroup::scaleMin/scaleMax` are kept in `scaleUnit` (the persisted `{v, unit}`; "" = base units) and
  converted at build time (`ComponentBuildInputs::units`, else by matching the axis display unit).
- Distance axis labels are in metres (the `Corr Dist` display override is not among the build inputs yet); the
  time axis uses `m:ss.fff`.
- Added `component_build.hpp` and `time_distance_builder.hpp`: 04 §5.5 requires every builder to be a pure
  function of (spec, view, selection, data snapshot, layout, metrics) and 04 §7.2 item 9 / AC-M1-05 require the
  widget and `esses-render` to run the same builders; neither `esses_widgets` nor `esses_qtrender` can host them
  for the other, so they live here. `esses_qtmodel`'s `RenderService::prepareInputs` fills
  `ComponentBuildInputs` (channel resolution through maths stays out of plotcore, D-048).
- `DrawList` gained builder helpers (`clear`, `scissor`, `filledRect`, `polyline`, `lines`, `points`,
  `rectOutline`, `textRun`), `textOf` and `toJson`; `LayoutContext`, `HitResult` and
  `KeyRow`/`KeyGeometry`/`AxisSpec`/`AxisTick` are defined here (04 names them without declaring them).
