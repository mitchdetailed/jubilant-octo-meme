# esses_analysis

Layer 2, Qt-free (01 §2.1). The analysis algorithms that sit between the data model and the maths engine: range
rules, the fastest-lap candidate rule, range interval tables, the per-lap distance index and the corrected
speed/distance algorithms. M1 scope: 07 §4.1 (`LapStretch` M2; `alternateList` as a fallback only and
`gCorrection` M3; track, variance, reports, binners later).

Namespace `esses::analysis`; public headers in `include/esses/analysis/` (umbrella `analysis.hpp`). Links
`esses_base`, `esses_units`, `esses_store`, `esses_model`; nlohmann-json is private. Never links `esses_maths`,
`esses_project` or `esses_formats` (forbidden edges in `cmake/esses_targets.cmake`). Sources and tests are
globbed: add `src/*.cpp` / `tests/*.cpp` files without editing `CMakeLists.txt`.

| Header | Contents | Design |
|---|---|---|
| `range_rule.hpp` | `RuleDocument` (rule-1 envelope), `RuleContext`, `IRangeRule` (with `appliesTo`), `layerListMatches`, `makeRangeRule` | 02 §4.5, D-014 |
| `rule_runner.hpp` | `RuleRunner` (phase 1 over a source, `rebuildFunction()` for `DataSource::withSidecar` and `formats::OpenContext::rangeRebuild`, phase 2 `runPhase2`, with the track-sections pass when given a track map, D-195) | 02 §4.4 rule 13, D-140 |
| `beacon_pair_rule.hpp` | `BeaconPairTransition`, `BeaconPairClass`, `BeaconPairAnnotations`, `BeaconPairSettings`, `BeaconPairRule` | 02 §4.4 rules 9-13, §4.5, D-075, D-132 |
| `whole_outing_rule.hpp` | `WholeOutingRule` (wraps `model::buildWholeOutingGroup`) | 02 §4.5 |
| `fastest_lap.hpp` | `fastestLapCandidates`, `fastestLap`, `fastLapsWithin` (the TT 2485 candidate rule) | 06 §5.2, D-135 |
| `range_tables.hpp` | `RangeInterval`, `RangeTables` (immutable, `shared_ptr<const>`) | 01 §5.3 rule 9, 03 §6.1 |
| `distance_index.hpp` | `DistanceIndexKey` (D-140), `DistanceIndex` (`distAt`, `timeAt` in seconds, `indexAt`, 0.25 m) | D-016, D-099, 04 §3.1 |
| `corrected_speed.hpp` | `SpeedMethod`, `DistanceSource`, `Integration`, `SpeedClamp`, `GCorrectionSettings`, `CorrectedSpeedSettings`, `UniformSignal`, `CorrectedSpeedResult`, `CorrectedSpeed`, `CorrectedDistance`, `toUniformSeries` | 03 §9.1, D-016, D-084 |

Private: `src/series_access.hpp` (visits a `store::Series` window through `Column::require`, one chunk-sized block
at a time), `src/rule_json.hpp` (typed JSON reading helpers, the fixed speed-unit table of rule documents).

## Shipped data

`data/rules/circuit.whole-outing.rule.json`, `circuit.beacon-pair.rule.json` (the M1 rules) and
`circuit.track-sections.rule.json` (loaded from M3, phase 2), `circuit.detail-groups.rule.json` (02 §4.5
verbatim; `fromRuleIds` skips its kind until later), schema `schemas/rule-1.schema.json` with examples. `libs/project/CMakeLists.txt` copies
them next to the executables. `RuleRunner::circuitDefault()` builds the same two M1 rules in code; a test keeps
the shipped beacon-pair document and the code default equal.

## Semantics fixed here (02 §4.5 leaves them open)

- Beacon-pair transitions run over consecutive enabled markers of `markerGroups` whose class some transition
  names and that `ignoreClasses` does not list; the first matching transition wins; a pair without one (EOL→SOL,
  RESET→EOL) produces no range; a pair of equal times produces no range and consumes no number.
- Numbering restarts at every SOL. `"number": "n+1"` (In) and a partial lap take the next flying number (last
  flying number + 1, or the start number when none). `Start Lap` is read case-insensitively from the details
  as an integral number or integral text; anything else falls back to `numbering.default`.
- A layer without any BCN produces no `Outings:Laps` group (D-104: whole outing only; the Messages hint is
  the UI's). No SOL: a virtual SOL at 0 with the id an importer-synthesised SOL would have; no final EOL: a
  virtual EOL at the layer duration (the last relevant marker when the duration is 0), nil end marker; a SOL
  while an outing is still open ends that outing with an implied EOL at its time (nil end marker), so its in
  lap is kept (D-158). Markers at equal times sort EOL before SOL (model order, D-158).
- `pitStop`: the longest run of consecutive valid Corr Speed samples below `stationaryBelow` inside the lap
  lasts ≥ `stationarySec` (a run ends at the next sample at or above the threshold, an invalid sample or the
  lap end). Stint boundaries: RESET markers, a lap-counter decrease (between valid samples), a timestamped
  channel hole or an EOL→SOL pause > `dataGapUs`, and the end of a `pitStop` lap — the last only when the car
  moved earlier in the same stint, so a log that starts in the pit box keeps one first stint. A boundary at
  time b starts the new stint at the first lap starting at or after b.
- `RuleRunner::run` compares ranges without annotations when it carries versions over, and keeps the
  annotations of a range whose id, start and end are unchanged until phase 2 lands again.
- `DistanceIndex`: the distance at the lap bounds is interpolated between samples (`distAt(lapStart) == 0`);
  `tAtD` uses the running maximum (first reach) and interpolates inside the crossing segment; `timeAt` finds
  the sample segment that first reaches `d` through the step table (a binary search over that step's
  segments) and interpolates on it, so a stop inside a step does not move `d` later than its first reach
  (D-157); it ends at the time `lengthM()` (the maximum reached) is first reached; evenly spaced samples give
  an O(1) `distAt`, otherwise a binary search.
- `CorrectedSpeed::compute`: channels resolve by canonical then logged name (no aliases: `ChannelResolver`
  is the caller's); a channel with status Error, a timestamped channel or (with a registry) a non-speed unit
  is unusable. `fastestWheel` with an unusable channel falls back to `alternateList` (`W-CSD-FALLBACK`), which
  takes its first usable channel; none: NotFound `E-CSD-NO-SPEED`; `gCorrection` (M3) has no fallback (see
  "M3: binning, variance and corrected speed" below). Invalid samples are NaN with `valid[i] == 0`. Corr Dist trapezoid: a segment touching an invalid sample adds 0.
  `fastestWheel`'s grid has the highest input rate and starts at the grid point at or before the earliest first
  sample of any wheel (a lower-rate wheel that starts earlier is not cut off); an input on that grid is
  gathered, any other is interpolated.

## Deviations from the design text

- `CorrectedSpeedSettings::alternateListChannels` defaults to 03 §9.1's list plus `"Speed"` (the sim-exporter
  `SPEED`; see `libs/maths/README.md`; D-154, proposed, with 03 §9.1 updated).
- `IRangeRule::run(const DataLayer&, const RuleContext&)` where 01 §10 and D-014 write `(const DataLayer&, const
  Profile&)`: `Profile` is an `esses_project` document and `esses_project` links `esses_analysis`, so the
  profile reaches the rules as the ordered rule documents it names (`RuleRunner::fromRuleIds`) plus the details.
- `CorrectedSpeed::compute` returns whole-extent `UniformSignal` buffers (base units) rather than a
  `store::Series`: the 0.1 maths path evaluates whole extents (03 §6.0) and `esses_maths` seals the buffers into
  `ChunkCache` chunks through `PlanProvider`. An overload takes `const UnitRegistry*` (null: no conversion) for
  tools that open logs without units; `fastestWheel` also takes any number of wheels (the settings schema
  allows two).
- `DistanceIndex::timeAt` returns seconds since the lap start (D-099); `distAt` takes absolute layer time.
- The fastest-lap rule reads AC-M1-07's "trusted flying laps if any" (see `fastest_lap.hpp`) where D-135
  writes "if any trusted lap exists".
- Rule documents convert `stationaryBelow` with a fixed table (m/s, km/h, kph, mph, kn, ft/s): the rule
  parser has no unit registry.

## Tests

`esses_analysis_tests` (Catch2, `analysis.<test case>`): `range_rule_tests` (shipped documents, envelope and
kind errors, `fromRuleIds`), `beacon_pair_rule_tests` (AC-M1-07 phase 1 and 2), `rule_runner_tests` (overrides,
versions, `withSidecar`), `fastest_lap_tests` (TT 2485), `range_tables_tests`, `distance_index_tests`
(AC-M1-08 property test and plateau), `corrected_speed_tests` (AC-M1-11 hand reference, trapezoid 1e-9,
settings against the schema examples). The fixture goldens (`fixtures.verify_public`, `fixtures.laps_golden`)
cover AC-M1-07 through `esses-cli`.

## M2: lap stretching (07 §5.1, D-016, D-140)

`lap_stretch.hpp`: `LapStretchSettings` (`options.lapStretch`: the toolbar toggle and the tolerance as a fraction),
`LapStretchRow` (lap, measured length, track length, the stretch in effect, `applied`, `withinTolerance`,
`candidate`, `percent()`) and `LapStretch` (`kDefaultTolerance`, `factor`, `apply`, `row`, `report`). `apply` sets
the lap index's stretch through `DistanceIndex::setStretch`: the stretch is a scalar of the index key (D-140) that
scales nothing inside the index (`distAt`/`timeAt` stay unstretched; `DistanceChain` and `DomainMapper` multiply,
04 §3.1), so it is set before the index is published and a changed setting builds a new key rather than a new
table. With the toggle off the stretch is 1 and the row still reports `withinTolerance` and `candidate`, so the
Lap Editor can show what stretching would do; a lap without an index or without a track length gets no stretch.
`report` gives the rows of the Lap Editor's Stretch column and the Corrected Speed and Distance dialog's table in
the order given. Consumers: `esses_widgets` (`LapEditor`), `esses_qtmodel` (`DistanceService`, the toolbar
toggle).

AC-M2-05's core part is `tests/marker_edit_rebuild_tests.cpp` (`analysis.marker_edit: ...`): on a 60-lap outing
with 200 channels, a beacon inserted before lap 30 and an offset edit rebuild the laps through
`DataSource::withSidecar` and `RuleRunner::rebuildFunction()`; lap 30's trust flag and alignment offsets follow
its start marker (renumbered 31); restore deletes the edit (D-065) and "restore logged laps" removes every edit
and user marker. The best of 15 rebuilds is asserted under 5 ms in optimised builds and reported in debug ones
(about 3 ms in an MSVC debug build). `tests/lap_stretch_tests.cpp` covers `apply`, the toggle and the report.

## M3 contract (07 §6.1; `docs/milestones/M3.md` §2; D-193)

New headers (all in the umbrella `analysis.hpp`), each in the source root of its owner; every root is under the
recursive `src/` glob, so no work edits `CMakeLists.txt`:

| Root | Headers | Contents | Design |
|---|---|---|---|
| `src/speed/` (properties/shell work) | `corrected_speed.hpp` (additions) | `AlternateListChoice`, `CorrectedSpeed::alternateList`, `CorrectedSpeed::gCorrection` | 03 §9.1 items 2-3 |
| `src/variance/` (time_distance work) | `variance_table.hpp` | `VarianceMode`, `VarianceRequest`, `VarianceTable`, `varianceTimeAt`, `varianceDistanceAt` | D-023, 04 §8.8, AC-M3-05 |
| `src/track/` (track work) | `track_map.hpp`, `track_generator.hpp`, `section_detector.hpp`, `track_sections_rule.hpp` | `TrackMap` (`track-1`) and its value types; `TrackGenerator` (`fromSpeedLatG`, `fromGps`, `toEnu`); `SectionDetector::defaults`; `TrackSectionsRule` (phase 2) with `lapDistanceOfTrackDistance`/`trackDistanceOfLapDistance` | 02 §4.5, §6.9; 06 §13.5; AC-M3-04 |
| `src/reports/` (reports work) | `report_spec.hpp`, `stat_cache.hpp`, `report_engine.hpp`, `time_report_engine.hpp` | `ReportSpec` and its enums and names; `StatCache` (LRU, 64 MB); `ReportEngine` (`cells`, `statistic`) with `ReportInputs`/`ReportTable`; `TimeReportSpec`, `TimeReportEngine` (`build`, `eclectic`, `rollingMinimum`, `bandOf`) | 04 §12-13; AC-M3-03 |
| `src/binning/` (scatter/histogram work) | `band_axis.hpp`, `histogram_binner.hpp`, `scatter_binner.hpp`, `trend_fit.hpp` | `UnitValue`, `BandAxis`, `Band`, `generateBands`, `SampleSpan`; `HistogramBinner`; `ScatterBinner` (`gather`, `intervalAverage`, `density`); `TrendFit` | 04 §7.3, §10-11; AC-M3-06, -07 |

### M3-STUB items the implementing agents must replace

- Done (2026-10-14): `src/speed/corrected_speed_methods.cpp` (`alternateList`, `gCorrection`; `compute()` calls
  both) and `src/variance/variance_table.cpp` (`VarianceTable::build`, `valueRange`, `varianceTimeAt`,
  `varianceDistanceAt`); see the M3 section below.
- Done (2026-10-14): `src/track/` (`TrackMap::fromJson`, `toJson`, `validate`, `pointAt`;
  `TrackGenerator::fromSpeedLatG`, `fromGps`, `toEnu`; `SectionDetector::defaults`;
  `TrackSectionsRule::fromDocument`, `sections` and the two origin helpers; the `RuleRunner::runPhase2`
  track-sections pass); see "M3: track maps and sections" below.
- Done (2026-10-14): `src/reports/` (`ReportSpec::fromJson`/`toJson`; every `StatCache` member; `ReportEngine::cells`,
  `statistic`; `TimeReportSpec::fromJson`, `TimeReportEngine::build`, `eclectic`, `rollingMinimum`, `bandOf`); see
  "M3: reports" below.
- Done (2026-10-14): `src/binning/` (`BandAxis::fromJson`/`toJson`, `generateBands`; `HistogramBinner::bin`,
  `binOf`; `ScatterBinner::gather`, `intervalAverage`, `density`; `TrendResult::evaluate`, `TrendFit::fit`,
  `equationText`).

Final: `reportStatName`/`parseReportStat`, `reportScopeName`/`parseReportScope`, `TrackMap::sectionSets`,
`TrackSectionsRule`'s constructor, `kind`, `phase`, `appliesTo` and `run` (phase 1 produces nothing),
`ReportTable::pending`/`at`, `StatCache::bytes`, the value types and their defaults (`tests/m3_contract_tests.cpp`).

Consumers: `esses_plotcore` (`BandAxis`, `UnitValue`, `TrendSpec`, `TrackMap`, `ReportSpec::fromJson` in
`componentChannelUses`, the geometry builders), `esses_project` (`TrackMap` in `TrackMapFile`), `esses_qtmodel`
(`ReportSpec`, `TimeReportSpec`, `StatCache` in `ReportModel`), `esses_widgets` (`CorrectedSpeedSettings`).

Deviations: `VarianceTable::build` takes a `VarianceRequest` beside the two indexes; `TrendFit` adds no Eigen
dependency (D-193 (h)); `UnitValue`, `Band`, `SampleSpan` and the report, track and binning value types are new
names (outline §4, "Names added by the M3 contracts").

## M3: binning, variance and corrected speed (07 §6.1; AC-M3-05, -06, -07; D-194)

The `src/binning/`, `src/variance/` and `src/speed/` stubs are implemented; the headers state the semantics,
D-194 records the readings the design leaves open. In short:

- **Series reads** (`src/binning/series_cursor.hpp`, private): the binners read through `Column::require` with
  at most two chunks per column pinned at a time; a secondary channel (gating, colour, every scatter channel)
  is looked up at non-decreasing times under its own `Interp` with `store::Resampler`'s rules (invalid outside
  the channel's first..last sample). The members that carry the policies are new and defaulted:
  `ScatterChannel::interp` (Linear), `ScatterInput::gatingInterp` (Previous, as the histogram's gating),
  `HistogramInput::colourInterp` (Linear).
- **`BandAxis`**: `fromJson`/`toJson` cover every member of the schema's `bandAxis` (closed below the top level,
  unknown top-level members in `rest`; colours written lower case, `#rrggbb` when opaque). `generateBands`: width
  mode gives full-width bands from `dataMin` (the last may end past `dataMax`), count mode equal bands, `centred`
  centres band k on `dataMin + k * w`; the open bands are `[-inf, lo)` and `[hi, +inf)`; values convert through
  the registry (a width by scale only), at most 10,000 bands; palettes `twoColour` and `rainbow` (the default for
  any other name).
- **`HistogramBinner`**: a sample counts in the band holding its value (`binOf` is a binary search); percent is of
  the samples counted in some bin; time credits one period per uniform sample and, for a timestamped channel,
  the time to the next sample (cut at the span end); gating Previous, dropped at <= 0 or invalid; untrusted
  spans skipped when asked; the colour split uses the bars' y mode.
- **`ScatterBinner`**: the union grid is the highest-rate channel's sample times (x must be uniform); a point
  needs a valid x and y and gating > 0 (an invalid z keeps the point with z = NaN); `intervalAverage` bins are
  `[k w, (k + 1) w)`; `density` cells are half open with the upper edges in the last column and row.
- **`TrendFit`**: QR by Givens rotations, row by row, on x scaled to about [-1, 1] (no Eigen, D-193 (h)); r² is
  `1 - SSres/SStot`; `equationText` writes `y = 0.01234x^2 - 1.234x + 5.678` with `std::to_chars` (locale
  independent). The reference is `tools/trend_ref.py`: exact rational normal equations over the exact binary
  inputs, written to `tests/data/trend_fit_cases.json`; the `lint`-labelled ctest entries
  `analysis.trend_reference_current` and `analysis.trend_reference_self_test` keep it current and checked.
- **`VarianceTable`**: per column one lookup in each lap's `DistanceIndex`, offsets applied and each lap's
  stretch scaling its distances as `DistanceChain` does (`timeAt(d / stretch)`, `distAt(t) * stretch`); blank
  outside the Main lap `[0, extent)` or the overlay's `[0, extent]`. A panel without an overlay calls nothing
  (the time_distance builder builds one table per visible overlay).
- **`CorrectedSpeed`**: `alternateList` takes the first engaged candidate and clamps it; `compute()` reads the
  list only up to its first usable channel. `gCorrection` follows 03 §9.1 item 3 with a linear blend of the
  difference back to the wheel over `recoverSeconds`; the first sample, a missing acceleration and the sample
  after an invalid wheel sample follow the wheel; an invalid wheel sample is invalid. In `compute()` the
  longitudinal-G channel is converted through the registry when it has a unit (an acceleration) and taken in G
  otherwise; either channel unusable is NotFound `E-CSD-NO-SPEED` naming it (no fallback: the maths module lists
  exactly these two inputs).

Tests: `band_axis_tests.cpp`, `histogram_binner_tests.cpp` (AC-M3-07 against a brute force over plain arrays),
`scatter_binner_tests.cpp` (gather against a brute force; the cursor against `Resampler::valueAt` for every
policy), `trend_fit_tests.cpp` (AC-M3-06, 14 reference cases), `variance_table_tests.cpp` (AC-M3-05: 3 overlay
pairs, 1,601 columns, offsets and stretch, against a linear search of the raw samples),
`corrected_speed_methods_tests.cpp` (hand-computed lock, spin and blend).

## M3: track maps and sections (07 §6.1; AC-M3-04; D-195)

`src/track/` holds the four classes and a private helper, `track_path.hpp` (arc length, resampling, the adaptive
smoother, chord headings and curvature, nearest segments, the length-unit table of track-1 documents).

- `TrackMap`: `fromJson` reads `{v, unit}` lengths in m, km, cm, mm, mi, ft, yd or in, checks `$schema` when present,
  takes the last centreline distance when `length` is absent, rejects unknown members inside the closed objects and
  keeps unknown top-level ones (`x-i2`, ...) in `rest`; `toJson` writes every known member, lengths in metres and
  `generatedFrom` only when set; `validate` (10 m minimum and bounds within 1e-6 m, unique non-empty ids, non-empty
  names, sets and categories, an ordered centreline, finite numbers, the origin's range) names the member's JSON
  pointer; `pointAt` wraps a closed map (with or without the closing row) and clamps an open one.
- `TrackGenerator` (D-195 (a), (b)): GPS fixes projected onto the WGS-84 local tangent plane (`toEnu` through
  ECEF at the origin's altitude), gaps bridged by a Hermite curve, the loop closed by joining end and start, an
  adaptive local-linear Gaussian smoother (sigma smoothingM / 2 in corners, up to 8x on straights, inward offset
  within 0.5 m), resampled every `stepM` with the closing row; `report.rmsM` is the RMS distance of the fixes from
  the line and `closureErrorM` the gap between the interpolated lap bounds. Speed+LatG dead reckoning (lateral
  acceleration positive to the left, `invertLatAccel` for the other convention) with a heading-rate bias and a
  position gap removed for closure; the raw gap is the reported closure error. Inner/outer lines carry the
  centreline distance of their nearest centreline point. Diagnostics: `W-TRACK-DROPOUT`, `W-TRACK-EDGE-SKIPPED`,
  `W-TRACK-NOT-A-LOOP`.
- `SectionDetector::defaults` (D-195 (c)): chord curvature over +-smoothingM / 2, corners above 1 / cornerRadiusM,
  short runs absorbed (S-bends split, same-way corners joined), corners turning less than `minTurnDeg` (15°)
  straightened, a closed map's run across the start line cut there; generated sections first in track order,
  then the kept ones (every user section and every non-Default set).
- `TrackSectionsRule` (D-195 (d)): `fromDocument` reads the rule-1 members (Invalid at a pointer otherwise);
  `sections` returns `Outings:Laps:Track Sections`, then per set its group and category children; ranges carry the
  lap's class, number, trust, enabled flag and start marker (ids `rangeId(group, lap start marker, name)`, D-066),
  times through the stretched `DistanceIndex`, a section reaching a lap bound ends there, laps not starting at a
  beacon measured backwards from their end. `RuleRunner::runPhase2(src, layer, speed, trackMap, indexOf, cancel)`
  replaces the groups (a null map removes them; versions per D-140) and phase 1 keeps them while the laps are
  unchanged. `fromRuleIds` loads the rule; phase 1 ignores it, so the M1/M2 trees are unchanged.

Tests: `track_map_tests.cpp`, `track_generator_tests.cpp` (AC-M3-04 on `fixtures/track/synthetic-circuit`, written
by `tools/gen_track_fixtures.py`; the measuring helpers are in `track_fixture.hpp`, independent of `src/track/`),
`section_detector_tests.cpp` (exact geometry built in the test), `track_sections_rule_tests.cpp` (hand-computed
section times of an out lap, flying laps, a stretched lap and a short in lap; the phase-2 pass and its versions);
`range_rule_tests.cpp` now expects the track-sections rule to load. `TrackMapFile` is tested in `esses_project`.
Results (2026-10-14, MSVC debug): (a) GPS 0.80 m RMS from the true centreline (inner 0.50 m, outer 0.52 m, length
-0.14 %, fit RMS 1.31 m, closure 3.4 m); (b) Speed+LatG 4.1 m RMS from the GPS map after the rigid alignment,
seven corners in both, worst apex 4.5 m, closure error before correction 45.5 m.

## M3: reports (07 §6.1; AC-M3-03; D-197)

`src/reports/` holds the four classes and a private helper, `report_common.hpp` (the range a `ReportSource` names,
the chronological order of sources by their `Log Date`/`Log Time` details, colour text).

- `ReportSpec::fromJson` reads the body's `report` object (Invalid at the member's JSON pointer for a wrong type, an
  unknown statistic or scope, a negative `probeSeconds`; unknown members ignored); `toJson` writes every member.
  `TimeReportSpec::fromJson` reads the whole `time_report` body (`allOutings` is Invalid, as in its schema;
  `timeFormat` is the view's).
- `StatCache`: an LRU of `budgetBytes / 64` cells behind one mutex; keys held as 128-bit digests of their fields
  in one flat slot vector (48 bytes a slot plus a hash-map entry; 64 bytes is the accounting unit of 01 §7.1).
- `ReportEngine::statistic` (D-197 (a)): over the valid samples of `[start, end)`: arithmetic mean, population
  standard deviation, min/max/absMax with the time of their first occurrence (absMax is the magnitude),
  delta = max - min, change = end - start. Start is the range's first sample when valid, else the first valid one
  at most `probeSeconds` after `start`; end mirrors it before `end`. `interpolateBoundaries` takes the value at
  exactly `start`/`end` by the channel's `Interp` (store::Resampler's policy, neighbours outside the range count)
  and falls back to the probe when that is invalid. One pass serves every statistic of a (row, channel): uniform
  series straight from the pinned chunks and their validity runs, timestamped ones through `forEachSample`.
- `ReportEngine::cells` (D-197 (b)-(e)): rows are the source ranges in scope (untrusted ones and their untrusted
  sub-ranges only with `includeUntrusted`; overlays not with `excludeOverlays`) divided by the headers in order:
  each header's range group splits a row into its intersections with the group's enabled ranges, so headers come
  in any order; a header that names no group of the layer labels the row with the source detail of that name
  ("Driver", "Venue"; empty without one); without headers a row is the source range under its own name. Rows are
  ordered by source (chronological), then time, then grouped so equal names at each level stay under their first
  occurrence. Columns are (channel, stat) in spec order, each followed by its Main - Overlay 0 column with
  `deltaVsOverlay0` (paired by heading path; `excludeOverlays` hides the Overlay 0 rows but keeps the pairing;
  numbered overlays never pair). Values convert with `toDisplay`, differences (change, delta, stdDev) with its
  scale only. A Pending, Error or unresolved channel gives Pending, Error or Missing cells; isMin/isMax mark a
  column's extremes when two Ok cells differ. The cache key adds the column's identity to its version and always
  carries the row's exact interval (a range keeps its id when a beacon offset moves it); the probe and boundary
  options enter only the start/end/change keys. Before scanning, the cells already cached are taken; the scans
  then run in order while the chunks of the next ones, up to 256 chunks ahead, are prefetched
  (`Column::prefetch`, a no-op without a pool).
- `TimeReportEngine` (D-197 (f)): rows are the lap ranges given (the caller expands outings), chronological;
  columns are the `rowGroup` sections starting inside a lap, merged into track order across laps, then the lap
  time; eclectic = per-column minima plus their total (none while a column has no time); the rolling minimum runs
  over each run of contiguous laps only (a lap ending where the next starts), earliest window on a tie; total sums
  the times present; `bandOf` takes the first visible band with `cell <= fastest * (1 + pct / 100)`.

Tests (`analysis.report_*`, AC-M3-03): `report_stats_tests.cpp` (every statistic against values computed by hand in
the file, the probe at its limits, interpolation by each `Interp`, timestamped series, chunk-crossing ranges with an
invalid run, F32), `report_cells_tests.cpp` (a hand-computed outing: untrusted exclusion, headers in both orders, a
detail heading, overlays in date order, delta versus Overlay 0, zoom clip, Pending/Missing/Error, an affine display
unit, the cache, chunks prefetched on a pool giving the same table), `report_time_tests.cpp` (a hand-computed section table: eclectic, rolling minimum with and without
untrusted laps and across a gap, total, bands), `report_spec_tests.cpp` (the schema examples, error pointers, the LRU
and concurrent use); shared fixtures in `report_test_support.hpp`. `bench/report_bench` measures the 07 §12 row
(`bench/workloads.md`).
