// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// RuleRunner: runs a profile's range rules over a source (ARCHITECTURE_OUTLINE section 2.1 step 6, 05 section
// 2 step 6, 02 section 4.4 rule 13, D-014, D-140).
//
// Phase 1 rebuilds every layer's tree from the phase-1 rules that apply to it (in order; each produced group
// is inserted with RangeGroupTree::setGroup, so "Outings:Laps" needs "Outings" first), re-applies the
// sidecar's range overrides (by id, then by name with a note per fallback hit) and carries range-group
// versions over from the previous tree, so only changed group paths bump (D-140). Versions compare ranges
// without the phase-2 annotations (an annotation never bumps a version), and a range whose id, start and end
// are unchanged keeps its previous stint and pitStop until phase 2 lands again. It replaces esses_model's
// rebuildWholeOutingRanges as the DataSource::RangeRebuild of every snapshot from M1 on (esses-cli laps,
// SourceLoader, sidecar edits). Phase 2 (beacon-pair stint and pitStop annotations; with a track map, the
// Track Sections groups) runs on a worker under the task key ranges/<sourceId>/<layer>/<markerSetHash> and
// returns the annotated tree for the GUI to publish; it keeps every other group version. Phase 1 keeps the
// Track Sections groups of the previous tree while the laps group they were built under is unchanged
// (ignoring annotations): an edit that leaves the laps as they were does not drop them.
#pragma once

#include <esses/analysis/range_rule.hpp>
#include <esses/analysis/track_map.hpp>
#include <esses/analysis/track_sections_rule.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/model/range.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/model/source.hpp>
#include <esses/store/series.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

class RuleRunner {
public:
    RuleRunner() = default; // no rules: run() leaves every layer's tree empty
    explicit RuleRunner(std::vector<std::shared_ptr<const IRangeRule>> rules);

    // The rules a profile names (profile-1 "rules"), loaded from <rulesDir>/<id>.rule.json in that order.
    // Kinds without an implementation yet are skipped and listed in `skipped`.
    static Result<RuleRunner> fromRuleIds(const std::filesystem::path& rulesDir,
                                          std::span<const std::string> ruleIds,
                                          std::vector<std::string>* skipped = nullptr);
    // The shipped circuit phase-1 rules (circuit.whole-outing, then circuit.beacon-pair) built in code, for
    // callers without a data directory (tests, esses-cli when data/rules is absent).
    static RuleRunner circuitDefault();

    // Phase 1 over every layer of `src`; returns the sidecar notes (orphaned overrides, name fallbacks). A
    // rule that fails produces nothing (rules do not fail on valid markers); a group whose parent no earlier
    // rule produced is dropped.
    model::SidecarApplyReport run(model::DataSource& src) const;
    // run() as a DataSource::RangeRebuild for DataSource::withSidecar. The function shares this runner's
    // rules (shared_ptr), so it may outlive the RuleRunner.
    model::DataSource::RangeRebuild rebuildFunction() const;

    // Phase 2 for one layer, on a worker: the layer's tree with the beacon-pair annotations applied (stint,
    // pitStop; 02 section 4.5). `speed` is the stationary-speed series (Corr Speed, a maths output) when it
    // is available (analysis::toUniformSeries wraps a CorrectedSpeed result); without it the layer's channel
    // named by the rule's speedChannel is used, and with neither pitStop stays false. Never changes ids,
    // classes, numbers, trust or group versions. NotFound for an unknown layer; Cancelled.
    Result<model::RangeGroupTree> runPhase2(const model::DataSource& src, std::string_view layer,
                                            const std::optional<store::Series>& speed,
                                            const CancelToken& cancel) const;
    // Phase 2 with the track-sections pass (02 section 4.5): the annotations as above, then for every
    // TrackSectionsRule that applies to the layer the groups under its sets' parent ("Outings:Laps:Track
    // Sections") are replaced by TrackSectionsRule::sections over `trackMap` with the lap indexes of
    // `indexOf` (a null map removes them). A Track Sections group keeps the version of the same path in the
    // layer's tree when its ranges are unchanged and bumps it otherwise (1 when new, D-140); no other group
    // version changes. NotFound for an unknown layer; Cancelled.
    Result<model::RangeGroupTree> runPhase2(const model::DataSource& src, std::string_view layer,
                                            const std::optional<store::Series>& speed,
                                            const std::shared_ptr<const TrackMap>& trackMap,
                                            const LapIndexLookup& indexOf, const CancelToken& cancel) const;

    std::span<const std::shared_ptr<const IRangeRule>> rules() const noexcept { return rules_; }

private:
    std::vector<std::shared_ptr<const IRangeRule>> rules_;
};

} // namespace esses::analysis
