// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackSectionsRule: "circuit.track-sections" (02 section 4.5, kind "track-sections", phase 2;
// ARCHITECTURE_OUTLINE section 4 esses_analysis). For every section set S of the venue's TrackMap ("Default"
// always) it emits the group Outings:Laps:Track Sections:S with the sections of S under every lap of the
// parent group, and one child group per category present in S (corner -> ...:S:Corners, straight ->
// ...:S:Straights, any user category c -> ...:S:c). A section's [start, end) comes from the lap's
// DistanceIndex (D-016, D-140); a rebuild bumps only the versions of the Track Sections group paths, never
// that of Outings:Laps.
// Origin rule (02 section 4.5, 04 section 13 item 5, section 14 item 5): a lap that starts at a BCN measures
// forward from its start; any other lap (an out lap SOL->BCN, a partial lap after RESET) is positioned
// backwards from its end beacon: track distance s lies at lap distance lapLength - (trackLength - s), and a
// section that would start before the lap start is omitted.
// Ranges are RangeKind::Section with ids model::rangeId(groupPath, lap start marker, section name) (D-066).
// Phase 1 (run) produces nothing: the sections need the lap indexes, so RuleRunner's phase 2 calls
// sections() on a worker (RuleRunner::runPhase2 with a track map).
// As built: the set groups follow "always" then the map's sectionSets(); a set's category children follow the
// categories in track order; ranges carry the lap's class, number, trust, enabled flag and anchor markers
// (startMarker = the lap's, D-066), name = the section name and abbrev = its id; a section that reaches a lap
// bound starts or ends exactly there. A forward lap shorter than the track clamps its last section to the lap
// end and omits sections that start after it. Owner: src/track/ (the track work).
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/analysis/range_rule.hpp>
#include <esses/analysis/track_map.hpp>
#include <esses/base/cancel.hpp>
#include <esses/base/result.hpp>
#include <esses/model/range.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

struct TrackSectionsSettings { // the rule-1 "track-sections" members
    std::string produces = "Outings:Laps:Track Sections:{set}";
    std::string categoryChild = "{set}:{categoryGroup}";
    std::string parent = "Outings:Laps";
    std::vector<std::string> alwaysSets{"Default"};
    std::map<std::string, std::string> categoryGroups{{"corner", "Corners"}, {"straight", "Straights"}};
    std::string otherCategoryGroup = "{category}"; // the "*" entry
    std::string requiredChannel = "Corr Dist";
    bool supportsTrackGeneration = false; // groupFlags of the produced groups
    bool supportsReporting = true;
};

// The lap's DistanceIndex (null: the lap has none, so it gets no sections) and whether it starts at a BCN.
struct LapIndex {
    std::shared_ptr<const DistanceIndex> index;
    bool startsAtBeacon = true;
};
using LapIndexLookup = std::function<LapIndex(const model::Range& lap)>;

class TrackSectionsRule final : public IRangeRule {
public:
    explicit TrackSectionsRule(TrackSectionsSettings settings = {}, std::string id = "circuit.track-sections",
                               std::vector<std::string> layers = {"*"});
    // A "track-sections" rule-1 document: "produces" ends with "{set}" under a group of the laps named by
    // "parent", "categoryChild" starts with "{set}" and names "{categoryGroup}". Invalid with context.detail
    // otherwise.
    static Result<std::unique_ptr<TrackSectionsRule>> fromDocument(const RuleDocument& document);

    std::string_view id() const override { return id_; }
    std::string_view kind() const override { return "track-sections"; }
    int phase() const override { return 2; }
    bool appliesTo(std::string_view layerName) const override;
    // Phase 1: an empty tree (the rule reads pinned columns, 02 section 4.4 rule 13).
    Result<model::RangeGroupTree> run(const model::DataLayer& layer,
                                      const RuleContext& context) const override;
    // Phase 2, on a worker: the Track Sections groups for every lap of settings().parent in `tree` (a tree
    // holding the parent group and nothing of this rule's), as roots-relative groups ready for
    // RangeGroupTree::setGroup in order: the sets' parent ("Outings:Laps:Track Sections"), then each set's
    // group followed by its category children; every version 0. Empty when `tree` has no parent group.
    // Cancelled.
    Result<std::vector<model::RangeGroup>> sections(const model::RangeGroupTree& tree, const TrackMap& map,
                                                    const LapIndexLookup& indexOf,
                                                    const CancelToken& cancel) const;

    const TrackSectionsSettings& settings() const noexcept { return settings_; }

private:
    TrackSectionsSettings settings_;
    std::string id_;
    std::vector<std::string> layers_;
};

// The origin rule: the lap distance of track distance `trackM` (nullopt when it falls before the lap start of
// a lap that is measured backwards), and its inverse (track_position, 04 section 14 item 5).
std::optional<double> lapDistanceOfTrackDistance(double trackM, double lapLengthM, double trackLengthM,
                                                 bool lapStartsAtBeacon) noexcept;
double trackDistanceOfLapDistance(double lapM, double lapLengthM, double trackLengthM,
                                  bool lapStartsAtBeacon) noexcept;

} // namespace esses::analysis
