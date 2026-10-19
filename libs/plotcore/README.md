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
| `selection_snapshot.hpp` | `SlotKind`, `Slot`, `SelectionSnapshot`, `traceColour`, `rangeKeyColour` (06 §5.3 item 3: the variance traces, offset sub-axes, range band, Track Position dots, scatter/histogram overlays and gauge reference values, D-202), `traceDash`, `traceWidthScale`, `adjustLightness` | 04 §2.3, D-017, D-100, D-202 |
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
- M3 (`m3_*_tests.cpp`, helpers in `m3_golden_support.hpp`): `m3_layout_tests.cpp` (point sets against a
  brute-force search with ties, tags, outside and NaN points; `BandLayout` stacking, clipping, labels and hits;
  the `KeyLayout` columns, the default columns equal to the M1 layout and DrawLists; `ScaleSpec`/`ColourBands`
  JSON and `colourBandOf`), `m3_body_tests.cpp` (the outing, track_position, scatter and histogram bodies:
  `parse(x).toJson() == x` for the schema examples, errors with pointers, channel order),
  `m3_outing_track_tests.cpp` (the outing's lap band, zoom rectangle, overlays and cancellation;
  `TrackGeometry::fit`, the origin rule of `trackDistanceAt`, sections, dots, zoom span, banners) and
  `m3_plots_tests.cpp` (scatter frame, point-set tags, trend and equation, cursor ring, tiles, colour bands and
  legend, density, interval average; histogram slots, sub-bars, outlined overlays, lines, text table, bin hits;
  the end-to-end builds through `ScatterBinner` and `HistogramBinner`, which warn and skip while those are
  stubs). **`DrawList` JSON goldens** at DPR 1 and 2 in
  `tests/golden/drawlist/{band_layout,key_layout,outing,track_position,scatter,histogram}/`: the band and key
  layouts from fixed rows, the outing from the synthetic sources (columns made resident first), the track from an
  octagonal circuit with constant-speed laps, scatter and histogram from scenes computed by brute force in the
  test (so the goldens pin the drawing independently of the binners) with bands given in the test.

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

## M3 (07 §6.1; `docs/milestones/M3.md` §2; D-193, D-196)

| Header | Contents | Design |
|---|---|---|
| `band_layout.hpp` | `BandCell`, `BandMarkerKind`, `BandMarker`, `BandLayoutInput`, `BandColours`, `BandGeometry`, `BandLayout` | 04 §8.1, §8.11 |
| `key_layout.hpp` | `KeyMeasurement`, `KeyColumns`, `KeyCell`, `KeyGeometry::cellRect`/`valueLines`, the column-aware `KeyLayout::layout`, `emitDatumValues`, `emitMeasurements` | 04 §8.4, §8.7 |
| `hit_tester.hpp` | the M3 `HitKind`/`HandleKind` values and `HitResult` members, `hitAll`, point sets (`addPoints`/`nearestPoint` with tags) | 04 §5.5, §10.1 item 7 |
| `component_bodies.hpp` | `ChannelUse`, `componentChannelUses`, `ScaleSpec`, `BandOpen`, `ColourBand` and their JSON helpers | D-193 (f), 04 §7.3 |
| `outing_geometry.hpp`, `track_geometry.hpp`, `scatter_geometry.hpp`, `histogram_geometry.hpp` | the bodies and the three builders of each type; `TrackFit`, `TrackGeometry::fit`/`trackDistanceAt`, `OutingGeometry::zoomRect` | 04 §9-11, §14 |
| `gauge_geometry.hpp` | `GaugeNumericBody`, `GaugeBarBody`, `GaugeGeometry` | 04 §16-17 |
| `draw_list.hpp` (M3), `scatter_geometry.hpp`, `histogram_geometry.hpp` | `TextRun::scale`; `ScatterExportSet`, `ScatterAxes`, `ScatterGeometry::exportSets`/`axes`; `HistogramExportColumn`, `HistogramExport`, `HistogramGeometry::exportTable` (D-198) | 04 §10.1 items 8 and 10, §11.1 item 10, §16 item 1 |

`ComponentBuildInputs` gains `trackMap` (the map of the Main source's venue, filled by `esses_qtmodel`). Every
geometry builder has `TimeDistanceBuilder`'s shape (`buildStatic` on a worker, `buildDynamic` and `buildChrome` on
the GUI thread), so `OffscreenRenderer` and the widgets use them alike. Internal helpers live in `src/geometry/`:
`geometry_common` (scheme roles, Quads, channel lookup, SummaryPyramid scales, unit conversion of `{v, unit}`
limits, a slot's visible span cut per lap with its trust flag, caption / pending / focus / banner Chrome),
`body_json.hpp` (the shared schema definitions and the "typed members written over `rest`" rule), and
`plot_scenes.hpp` with `plot_tiles.cpp` (the scatter/histogram frame, scene and emit split and the shared tiles).

As built (D-196 records the departures):

- **BandLayout**: bands of round(14 x dpr) px stacked from the top, omitted when hidden; each band is filled, its
  cells and markers clipped to it, and closed by a frame line. Cells draw their edges as ticks (a shared edge
  once; the outing's lap boundaries); range labels are elided to their cell, section labels drawn only when they
  fit; section fill: the cell colour, else `sectionCorner`/`sectionStraight` by category. Overlapping range cells
  share the one row, later cells on top. Markers: beacons as ticks, annotations as rectangles, device events as
  triangles (a Quads command). Hits: `Band` (`band` = cell index, `slot`), `Section` (`section`),
  `Marker`/`Annotation` (`markerId`); later cells and markers win.
- **KeyLayout**: with a datum a value line holds value@main, value@datum and delta right-aligned in that order;
  measurements add one line per measurement under the row's value lines with min, max and avg (`Measurement`
  style; colour 0 = the text colour). Widths add the shown cells after a pad each; when the 40 % clamp leaves no
  room for the label the cells shrink proportionally. The default `KeyColumns` give the M1 layout exactly
  (tested), so the value cell, `emitValues` and the M1 goldens do not move. `KeyGeometry::valueLines` records each
  row's value lines.
- **HitTester point sets**: finite points bucketed over the set's bounds on a 64 x 64 grid (outside points in
  the edge buckets); a query visits the buckets of its square; ties: the lower index within a set, the later set
  across sets; optional per-point tags.
- **Bodies**: strict about types and enums (Invalid with a JSON pointer), tolerant about absence; `toJson` writes
  typed members over `rest` (a member the source had is replaced, a new one only when it differs from its
  default), and a BandAxis that reads back unchanged keeps the source's form, so `parse(x).toJson() == x`.
  Limits are `{v, unit}`.
- **Outing**: whole-outing x (time since the outing start, or metres through the Main chain re-origined there);
  its own layout (caption, the lap band over the trace area, key column, 36 px axis strip, panels by `edges`, x
  axis), independent of `TimeDistanceBuilder::layoutOf`; traces from `decimateSummary` only; laps from the
  source's `Outings:Laps` group, else the chain pieces, else the Main range (the Main lap tinted);
  `enableOverlays` (time mode) draws the other visible slots aligned at lap start beneath Main. Dynamic: the zoom
  rectangle (`cursorZoom` at alpha 0x40 with a 0xC0 outline), the cursor line, key values per drawn slot. The
  widget derives the zoom handles from `zoomRect()`.
- **Track**: `fit` centres the bounding box of the rotated lines (map + body rotation) at the largest scale;
  manual multiplies it by `zoom` and shifts by `(panX, panY)` metres; sizes: `cursor` = dot diameter, `detail` =
  centreline width, `material` = extra zoom-span width per side; only the first section set (Default) is drawn
  (colours beneath the centreline, marks across it, labels beside the mid-point). Positions interpolate the
  centreline (a closed map wraps through its first point); `trackDistanceAt` applies the origin rule (a lap starts
  at a beacon when its layer has an enabled BCN at its start, or no BCN at all).
- **Scatter / histogram**: `scatterFrame`/`histogramFrame` (layout, scales, bands) read no samples; the scene
  comes from `ScatterBinner`/`TrendFit`/`HistogramBinner` on the worker; a binner failing with Cancelled or
  Unsupported fails the Static build, other failures drop that slot. Scatter: channel and auto scales both use
  the Main source's whole-column SummaryPyramid; point colours by colour band (the BandAxis rule applied
  locally) and trend curves by Horner from the coefficients, so the drawing does not depend on the binner;
  density above the threshold as 16 Quads levels of the set's colour; cursor styles ring, cross (crosshair) and
  none, the datum as a hollow square. Histogram: equal-width bin slots labelled at their boundaries every
  `binLabelStep`; y auto = 0 to the largest value; `stackLines` lifts trace n by n x 12 px; `condensedAxes`
  labels the bottom row of tiles; text style = a table of bins x sets. Point sets: the Main sets only, tagged
  with each point's Main-lap time.

- **Gauges** (the plots work, D-198): `GaugeNumericBody`/`GaugeBarBody` read and write like the other bodies
  (`parse(x).toJson() == x` for the schema examples; the `font` object is checked: a family and a point size of
  6..400). Both read their one channel at the cursor for Main and the reference overlay (the second visible
  slot) by the Values window's rule; "(missing)", "…" (pending) and "-" (invalid). Numeric: the value in the top 65 %
  of the content (all of it without a reference), the unit after it (`GaugeUnits`), the reference beneath at no
  more than half the main scale in its key colour; `dynamicFont` sizes the value with `TextRun::scale` for the
  widest value the channel can show (its SummaryPyramid extremes, digits as '8'), never below 6 pt of the
  scheme's `gauge` font; without it the scale is the body font's point size over the scheme's. The first visible
  colour band holding the value colours the text or fills the content. Bar: the scale strip (left of a vertical
  track, under a horizontal one; `scaleDps` labels), band lines across the track or a 4 px band strip
  (`bandLines`), the track, the bar from the clamped origin to the clamped value (`bandMode` bar recolours it),
  the reference as a 2 px marker line, the value text at the bar end. Scales `channel`/`auto` from the Main
  channel's summary, `manual` converted.
- **`TextRun::scale`** (`draw_list.hpp`, D-198 (b)): a per-run factor on the style's point size, in the DrawList
  JSON only when it is not 1 (no M1 or geometry golden changed); builders measure a scaled run as the style's
  metrics times the scale.
- **Exports and axes** (D-198 (d), (e)): `ScatterGeometry::exportSets` and `HistogramGeometry::exportTable` gather
  as `buildStatic` does (the scatter's `gatherSlot`, the histogram's scene, now shared by both paths), Main first;
  `ScatterGeometry::axes` returns the frame's tiles and scales (no samples) for the widget's own axis zoom and
  point picking. The scatter and histogram builders pass each channel's descriptor `Interp` to the binners (x, y
  and the colour channel; gating stays `Previous`; M3 open issue 7).
- **Tests**: `m3_gauge_tests.cpp` (the bodies, values against the synthetic series formatted with
  `std::to_chars`, colour bands and their unit conversion, the dynamic and fixed font scales, every origin style
  in both orientations, the reference marker, band lines and fill, `scaleDps`, and the gauge DrawList goldens of
  `tests/golden/drawlist/gauge_numeric`, `gauge_bar`); `plots_golden_cases.hpp` holds the fifteen cases (three
  each of scatter, histogram, outing, gauge_numeric, gauge_bar) shared with the pixel goldens of
  `tests/golden/images/tst_golden_plots.cpp`.

Every `M3-STUB` of `esses_plotcore` is replaced. Final: `HitTester::hitAll`, `clear`/`empty` over the point sets,
the M1 `HitKind` values (the new ones appended after `Handle`), `componentChannelUses`, `OutingBody::channelNames`
(`tests/m3_contract_tests.cpp`). The M1 builder sets `HitResult`s through a helper because the struct grew
(GCC/Clang `-Wmissing-field-initializers`).

Consumers: `esses_qtrender` (`OffscreenRenderer::builtinBuilders` calls every geometry builder), `esses_qtmodel`
(`componentChannelUses` in `componentChannels`), `esses_widgets` (the components and `HitResult`; the scatter
widget reads a clicked point's Main-lap time from `nearestPoint`'s tag and its axes from `ScatterGeometry::axes`,
the outing widget derives its zoom handles from `zoomRect()`, the plots components export through `exportSets`,
`exportTable` and their samples; the time_distance completion uses `BandLayout` and the `KeyLayout` columns).

### M3: the time_distance completion (as built, D-201)

`TimeDistanceBuilder` covers the whole of 04 §8 (`src/time_distance_builder.cpp`; `time_distance_builder.hpp`):

- **Body**: every member of `schemas/components/time_distance-1` is typed (`pointSize`, `showMarkers`, `showBands`,
  `bandMode`, `bandGroup`, `overlapMode`, `cursorStyle`, `cursorSnap`, `key.stats`, `showStatusStrip`,
  `statusChannels`, `showDeviceEvents`, `variance {show, heightFrac, autoScale, min, max}`, `axis.showXScrollbar`/
  `showYScrollbar`, the groups' `lo`/`hi` and `lines.{zero,min,max}.{enabled,colour}`); the new enums are checked
  (Invalid with the member's pointer); `{v, unit}` is always written with its unit (schema `unitValue`).
  `parse(x).toJson() == x` holds for `schemas/examples/components/time_distance-1/valid/full.json`.
- **Layout** (`layoutOf`): the range / section / marker bands (`BandLayout`, 14 px each) over the trace area under
  the caption; below the panels the status strip (10 px per status channel), the variance panel (`heightFrac` of
  the trace area, only with a visible overlay), the x axis, the offset axis (two 12 px sub-axes while
  `ViewState::offsetAxis`) and the horizontal scrollbar; overlapped mode lays each group over its `[lo, hi]` of
  the whole area with Lo/Hi handles at the right edge and the key listing the groups in turn; the active group's
  vertical scrollbar in its axis strip; the key's datum and measurement columns (`KeyColumns`). With the M3
  elements off, the layout and every list equal the M1 builder's (the M1 cases switch the default-on bands and
  scrollbars off in their bodies, `td_golden_cases.hpp` `m1Display()`).
- **Static** additions: bands (the visible slots' ranges, overlays first so Main is on top; the other ranges of
  the Main range's group, so zooming out shows the neighbouring laps; a slot cell mostly under Main's lends Main
  its name: one label "Main | overlay"), sections of `bandGroup` coloured by their child group (Corners /
  Straights / a user category), beacons (BCN, SOL, EOL), device events (`showDeviceEvents`) and the Main source's
  sidecar annotations with their dotted lines; scale lines min / max at `ResolvedChannel::limitMin`/`limitMax`;
  the status strip (status channels decimated with the traces, a filled span wherever a column is non-zero; names
  left of the strip); the variance panel (one `analysis::VarianceTable` per visible overlay against Main, offsets
  of both slots, `Δt [s]` in distance and `Δd [m]` in time, NaN columns blank, autoscale over the tables and 0
  with 10 % headroom, else the manual `{v, unit}` limits); the offset sub-axes (ticks in each slot's own
  coordinate, the offset written at the right end by `offsetText`); the horizontal scrollbar (the zoom over the
  Main outing extent); measurements (min / max / avg of the valid samples over the visible span per trace and
  slot, read chunk by chunk with `Column::require` on the worker; `KeyLayout::emitMeasurements`). Hit regions:
  `Band`, `Section`, `Marker`, `Annotation`, `StatusRow`, `Variance`, `OffsetAxis` (`OffsetMain` /
  `OffsetReference`), `ScrollBar` (`ScrollThumb` or the track; `group` for the vertical one), `Handle` (`GroupLo`,
  `GroupHi`; `PanelSplitter` left of the trace area, so a click in a panel always places the cursor),
  `KeyMeasurement`; overlapped mode adds the whole area as a Panel of the active group.
- **Dynamic** additions: the cursor style (`none` hides the main cursor line; `crosshair` adds a dotted horizontal
  line at the active channel's Main value), cursor lines through the status strip and the variance panel, and
  value@datum and Δ (main - datum, signed) per slot line while a datum is shown.
- **Chrome** addition: "variance needs distance data" in the variance panel when a lap of Main or of an overlay
  has no `DistanceIndex` (a chain piece without `idx`).
- **Widget-local state** (`TdLocalState`, the overloads of `buildStatic`/`buildDynamic`): the vertical zoom per
  group (`TdGroupZoom`, the shown fraction of the group's scale) and the scales the last Static build drew (so the
  crosshair lines up); `TdStaticInfo` reports what a Static build placed (band entries, markers, measurement
  positions, scales, the scrollbar extent, `varianceTables` for AC-M3-05). `ResolvedChannel` gains `limitMin`/
  `limitMax` (the override limits in base units; Channel scaling and the min/max lines use them).
- **KeyLayout**: the column-aware layout keeps up to 30 % of the clamped column for the labels and shrinks the
  cells below that (`kMinLabelFraction`), so a datum on a narrow graph no longer elides the channel names away.
- **Tests**: `td_m3_tests.cpp` (body members and errors, the layout stack, variance tables per overlay and none
  without one against `VarianceTable` and its autoscale, measurements against a sample-by-sample reference, the
  datum key, offset texts and sub-axis hits, bands with categories, markers, annotations and SHIFT+E, the status
  strip's spans, overlapped regions and handles, splitters, the scrollbar, scale lines and Channel scaling with
  limits, the crosshair, cursor style none, the vertical zoom) and the DrawList JSON goldens of the thirteen M3
  cases of `td_m3_cases.hpp` at DPR 1 and 2 in `tests/golden/drawlist/time_distance/` (the M1 `dark_scheme`
  golden changed with the datum columns). `td_m3_cases.hpp` decorates the synthetic sources with a Normal layer
  (laps, track sections with categories, beacons, device events, Bitfield/Bool status channels), sidecar
  annotations and an `analysis::DistanceIndex` per lap.

## M4 followups (D-210)

- M3 item 29: `tests/variance_mapper_tests.cpp` checks `analysis::VarianceTable` against `DomainMapper::xOf` (the
  mapping a slot's trace is drawn with, offsets and stretch applied) in both domains, blank columns included.

## M4 followups of the widgets (D-211)

- `display_units.hpp` (`src/display_units.cpp`): `defaultDisplayUnit` (the stored unit; `%` for a fraction stored
  in the Ratio base unit `ratio`), `displayUnitFor` (a named unit of the quantity, else the default) and
  `corrDistDisplayUnit` (the Corr Dist override when a length, else km for km/h and mi for mph, else m). The logged
  unit as the default is v1.0's (the descriptor keeps no file unit). `ResolvedChannel::displayUnitId` is the unit
  `toDisplay` converts to.
- `ScatterBody::xDisplayUnit`, `ScatterTrace::displayUnit`, `ScatterBody::colourDisplayUnit` (written only when
  set) reach `componentChannelUses`, as does the channel report channels' `displayUnit`.
- The time_distance Static build registers each group's key column as its `HitKind::GroupHeading`, beneath the
  group's labels, values and measurement cells (no new drawing; no golden changes).
- Tests: `tests/m4_widgets_followups_tests.cpp` (`plotcore.followups D-211: ...`, 6 cases).
