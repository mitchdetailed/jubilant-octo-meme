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
| `rule_runner.hpp` | `RuleRunner` (phase 1 over a source, `rebuildFunction()` for `DataSource::withSidecar` and `formats::OpenContext::rangeRebuild`, phase 2 `runPhase2`) | 02 §4.4 rule 13, D-140 |
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
`circuit.track-sections.rule.json`, `circuit.detail-groups.rule.json` (02 §4.5 verbatim; `fromRuleIds` skips their
kinds until M3/later), schema `schemas/rule-1.schema.json` with examples. `libs/project/CMakeLists.txt` copies
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
  takes its first usable channel; none: NotFound `E-CSD-NO-SPEED`; `gCorrection`: Unsupported (M3). Invalid
  samples are NaN with `valid[i] == 0`. Corr Dist trapezoid: a segment touching an invalid sample adds 0.
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
