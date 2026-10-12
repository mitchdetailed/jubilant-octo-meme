// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BeaconPairRule: the circuit lap rule "circuit.beacon-pair" (02 section 4.5, 02 section 4.4 rules 9-13,
// D-014; acceptance AC-M1-07: `esses-cli laps --json` equals expected.laps.json for every fixture with
// beacons).
//
// Phase 1 evaluates the transition table over the time-sorted, enabled markers of `markerGroups`, skipping
// `ignoreClasses` (IGRDBCN, SPLTBCN): SOL->BCN out, BCN->BCN flying, BCN->EOL in, SOL->EOL out, *->RESET
// partial (the next range starts at the RESET), RESET->BCN out. Laps are numbered per outing from the numeric
// "Start Lap" detail (default 1): flying laps s..s+n-1, Out 0, In n+1, Partial takes the number the flying
// lap would have had without consuming it. Default trust comes from the class table only (out, in, partial
// untrusted). Ids: rangeId("Outings:Laps", start marker).
//
// Semantics fixed by this implementation (02 section 4.5 leaves them open):
//   * Consecutive pairs of the filtered marker sequence are matched against `transitions` in table order
//     (first match wins); a pair without a matching transition produces no range (EOL->SOL between outings,
//     RESET->EOL). Markers whose class no transition names (stage markers, device events in a listed group)
//     are skipped like `ignoreClasses`, so they never break a lap.
//   * Numbering restarts at every SOL (per outing). "{n}" in a template is the lap's number; "number": "n+1"
//     and a partial lap both take the next flying number (the last flying number + 1, or the start number).
//   * A pair whose markers share a time produces no range and consumes no number.
//   * A layer without any BCN (after the filters) produces no group at all (D-104: a log without beacons
//     shows its whole outing only; the UI adds the Messages hint). A layer without SOL starts at a virtual
//     SOL at t = 0 carrying the id an importer-synthesised SOL would have (as WholeOutingRule); a sequence
//     that does not end in an EOL closes at a virtual EOL at max(duration, last marker time), nil end marker.
//
// Phase 2 (annotate) sets `stint` (counter reset, RESET marker, or a logged-data hole > dataGapUs; never the
// beacon interval) and `pitStop` (>= stationarySec below stationaryBelow); neither changes class, number or
// trust (rule 12). Details of this implementation:
//   * pitStop: the longest run of consecutive valid speed samples below `stationaryBelowMps` inside the lap
//     [start, end) lasts >= stationarySec; a run lasts from its first sample to the next sample at or above
//     the threshold (or the lap end). Invalid speed samples end a run.
//   * Stint boundaries at time b start a new stint at the first lap whose start is >= b: a RESET marker (b =
//     its time); a decrease of the counter channel between valid samples t[i-1] and t[i] (b = t[i-1] + 1 us);
//     two consecutive timestamps of a timestamped channel more than dataGapUs apart, or an EOL followed by
//     a SOL more than dataGapUs later (b = the later time); the end of a pitStop lap, but only when the car
//     moved (a valid speed at or above the threshold) earlier in the same stint, so a log that starts in the
//     pit box does not split its first stint. The first lap is stint 1.
#pragma once

#include <esses/analysis/range_rule.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/range.hpp>
#include <esses/store/series.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

struct BeaconPairTransition {
    std::string from; // marker class or "*"
    std::string to;
    std::string lapClass; // "out" | "flying" | "in" | "partial"
};

struct BeaconPairClass {
    std::string name;   // "Lap {n}"
    std::string abbrev; // "L{n}"
    bool trusted = true;
    bool numbered = false;            // consumes the running flying-lap number
    std::optional<int> fixedNumber;   // "number": 0
    bool numberIsLastPlusOne = false; // "number": "n+1"
};

struct BeaconPairAnnotations {
    bool enabled = true;
    std::string counterChannel = "Lap Number";
    Micros dataGapUs = 10'000'000;
    double stationarySec = 15.0;
    std::string speedChannel = "Corr Speed";
    double stationaryBelowMps = 1.0 / 3.6; // {"v": 1.0, "unit": "km/h"} in base units
};

struct BeaconPairSettings {
    std::string produces = "Outings:Laps";
    std::string parent = "Outings";
    std::vector<std::string> layers{"Normal", "Fastest"};
    std::vector<std::string> markerGroups{"", "Beacons"};
    std::vector<std::string> ignoreClasses{"IGRDBCN", "SPLTBCN"};
    std::vector<BeaconPairTransition> transitions;
    std::map<std::string, BeaconPairClass, std::less<>> classes; // keyed by lapClass name
    std::string startDetail = "Start Lap";
    int defaultStart = 1;
    BeaconPairAnnotations annotations;
    bool supportsTrackGeneration = true;
    bool supportsReporting = true;

    // The kind-specific part of a "beacon-pair" rule-1 document (unit values converted to SI).
    static Result<BeaconPairSettings> fromDocument(const RuleDocument& document);
    // The shipped circuit.beacon-pair document of 02 section 4.5, built in code.
    static BeaconPairSettings circuitDefault();
};

class BeaconPairRule final : public IRangeRule {
public:
    explicit BeaconPairRule(BeaconPairSettings settings, std::string id = "circuit.beacon-pair");
    static Result<std::unique_ptr<BeaconPairRule>> fromDocument(const RuleDocument& document);

    std::string_view id() const override { return id_; }
    std::string_view kind() const override { return "beacon-pair"; }
    int phase() const override { return 1; }
    bool appliesTo(std::string_view layerName) const override;
    Result<model::RangeGroupTree> run(const model::DataLayer& layer,
                                      const RuleContext& context) const override;

    // Phase 2 on a worker: stint and pitStop annotations of the laps group (pins `speed` and the counter
    // channel through Column::require at Interactive). Without `speed` the layer's channel named
    // annotations.speedChannel is used when it exists; with neither, pitStop stays false. With
    // annotations.enabled false the annotations are cleared (stint 0, pitStop false). Cancelled when `cancel`
    // fires; never changes ids, classes, numbers, names or trust.
    Result<void> annotate(const model::DataLayer& layer, const std::optional<store::Series>& speed,
                          model::RangeGroup& laps, const CancelToken& cancel) const;

    const BeaconPairSettings& settings() const noexcept { return settings_; }

private:
    BeaconPairSettings settings_;
    std::string id_;
};

} // namespace esses::analysis
