// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackSectionsRule (02 section 4.5, kind "track-sections", phase 2). Owner: the track work (src/track/).

#include "../rule_json.hpp"

#include <esses/analysis/track_sections_rule.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/model/ids.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace esses::analysis {

namespace {

using jsonio::asBool;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;
using nlohmann::json;

constexpr std::string_view kSetToken = "{set}";

std::string replaceAll(std::string text, std::string_view token, std::string_view value) {
    for (std::size_t at = text.find(token); at != std::string::npos;
         at = text.find(token, at + value.size())) {
        text.replace(at, token.size(), value);
    }
    return text;
}

// The category group of a category: the settings' map, else the "*" template.
std::string categoryGroupOf(const TrackSectionsSettings& s, const std::string& category) {
    if (const auto it = s.categoryGroups.find(category); it != s.categoryGroups.end()) {
        return it->second;
    }
    return replaceAll(s.otherCategoryGroup, "{category}", category);
}

Failure cancelledFailure() {
    return Failure{Error{ErrorCode::Cancelled, "cancelled", {}}};
}

// "Outings:Laps:Track Sections": the parent of every set's group.
std::string sectionsGroupPath(const TrackSectionsSettings& s) {
    return std::string(model::parentGroupPath(s.produces));
}

std::string setGroupPath(const TrackSectionsSettings& s, std::string_view set) {
    return replaceAll(s.produces, kSetToken, set);
}

std::string categoryGroupPath(const TrackSectionsSettings& s, std::string_view set,
                              const std::string& category) {
    std::string child = replaceAll(s.categoryChild, kSetToken, set);
    child = replaceAll(std::move(child), "{categoryGroup}", categoryGroupOf(s, category));
    return sectionsGroupPath(s) + ":" + child;
}

} // namespace

TrackSectionsRule::TrackSectionsRule(TrackSectionsSettings settings, std::string id,
                                     std::vector<std::string> layers)
    : settings_(std::move(settings)), id_(std::move(id)), layers_(std::move(layers)) {}

Result<std::unique_ptr<TrackSectionsRule>> TrackSectionsRule::fromDocument(const RuleDocument& document) {
    if (document.kind != "track-sections") {
        Error e{ErrorCode::Invalid,
                "rule '" + document.id + "' is of kind '" + document.kind + "', not track-sections",
                {}};
        e.context.detail = "/kind";
        return Failure{std::move(e)};
    }
    auto parsed = jsonio::parseJson(document.json.compact());
    if (!parsed) {
        return Failure{parsed.error()};
    }
    return jsonio::guardJson([&]() -> Result<std::unique_ptr<TrackSectionsRule>> {
        const json& j = *parsed;
        if (!j.is_object()) {
            violate("", "a rule-1 document is a JSON object");
        }
        ObjectReader r(j, "");
        detail::skipEnvelope(r);
        r.skip("id");
        r.skip("kind");
        r.skip("layers");
        if (const json* p = r.optional("phase")) {
            if (!p->is_number_integer() || p->get<std::int64_t>() != 2) {
                violate(r.child("phase"), "a track-sections rule runs in phase 2");
            }
        }
        TrackSectionsSettings s;
        s.produces = asString(r.required("produces"), r.child("produces"));
        if (s.produces.find(kSetToken) == std::string::npos ||
            model::lastGroupSegment(s.produces) != kSetToken) {
            violate(r.child("produces"), "produces ends with the segment \"{set}\"");
        }
        const std::string prefix(model::parentGroupPath(s.produces));
        if (prefix.empty()) {
            violate(r.child("produces"), "the section sets are grouped under a parent group");
        }
        s.parent = std::string(model::parentGroupPath(prefix));
        if (const json* p = r.optional("parent")) {
            if (asString(*p, r.child("parent")) != s.parent) {
                violate(r.child("parent"),
                        "parent must be '" + s.parent + "', the laps the sets are built under");
            }
        }
        if (s.parent.empty()) {
            violate(r.child("produces"), "the sections are built under a parent group of laps");
        }
        if (const json* p = r.optional("categoryChild")) {
            s.categoryChild = asString(*p, r.child("categoryChild"));
            if (!s.categoryChild.starts_with(kSetToken) ||
                s.categoryChild.find("{categoryGroup}") == std::string::npos) {
                violate(r.child("categoryChild"), "categoryChild is \"{set}:...{categoryGroup}...\"");
            }
        }
        if (const json* p = r.optional("sets")) {
            ObjectReader sets(*p, r.child("sets"));
            if (const json* from = sets.optional("from")) {
                if (asString(*from, sets.child("from")) != "trackMap") {
                    violate(sets.child("from"), "sets come from the track map (\"trackMap\")");
                }
            }
            if (const json* always = sets.optional("always")) {
                s.alwaysSets = detail::stringArray(*always, sets.child("always"));
                for (std::size_t i = 0; i < s.alwaysSets.size(); ++i) {
                    if (s.alwaysSets[i].empty() || s.alwaysSets[i].find(':') != std::string::npos) {
                        violate(jsonio::pointerIndex(sets.child("always"), i),
                                "a set name is one group segment");
                    }
                }
            }
            sets.closed();
        }
        if (const json* p = r.optional("categoryGroups")) {
            const std::string cp = r.child("categoryGroups");
            if (!p->is_object()) {
                violate(cp, "categoryGroups maps categories to group names");
            }
            s.categoryGroups.clear();
            for (const auto& [key, value] : p->items()) {
                const std::string name = asString(value, jsonio::pointerChild(cp, key));
                if (name.empty() || name.find(':') != std::string::npos) {
                    violate(jsonio::pointerChild(cp, key), "a category group is one group segment");
                }
                if (key == "*") {
                    s.otherCategoryGroup = name;
                } else {
                    s.categoryGroups.emplace(key, name);
                }
            }
        }
        if (const json* p = r.optional("requires")) {
            ObjectReader q(*p, r.child("requires"));
            if (const json* tm = q.optional("trackMap")) {
                static_cast<void>(asBool(*tm, q.child("trackMap")));
            }
            if (const json* ch = q.optional("channel")) {
                s.requiredChannel = asString(*ch, q.child("channel"));
            }
            q.closed();
        }
        if (const json* p = r.optional("groupFlags")) {
            ObjectReader g(*p, r.child("groupFlags"));
            if (const json* q = g.optional("supportsTrackGeneration")) {
                s.supportsTrackGeneration = asBool(*q, g.child("supportsTrackGeneration"));
            }
            if (const json* q = g.optional("supportsReporting")) {
                s.supportsReporting = asBool(*q, g.child("supportsReporting"));
            }
            g.closed();
        }
        r.closed();
        std::vector<std::string> layers =
            document.layers.empty() ? std::vector<std::string>{"*"} : document.layers;
        return std::make_unique<TrackSectionsRule>(std::move(s), document.id, std::move(layers));
    });
}

bool TrackSectionsRule::appliesTo(std::string_view layerName) const {
    return layerListMatches(layers_, layerName);
}

Result<model::RangeGroupTree> TrackSectionsRule::run(const model::DataLayer&, const RuleContext&) const {
    return model::RangeGroupTree{}; // phase 2 only
}

Result<std::vector<model::RangeGroup>> TrackSectionsRule::sections(const model::RangeGroupTree& tree,
                                                                   const TrackMap& map,
                                                                   const LapIndexLookup& indexOf,
                                                                   const CancelToken& cancel) const {
    std::vector<model::RangeGroup> out;
    const model::RangeGroup* laps = tree.find(settings_.parent);
    if (laps == nullptr) {
        return out; // no laps (D-104): no sections
    }
    // The sets: the always-present ones, then the map's in its order.
    std::vector<std::string> sets = settings_.alwaysSets;
    for (const std::string& s : map.sectionSets()) {
        if (std::find(sets.begin(), sets.end(), s) == sets.end()) {
            sets.push_back(s);
        }
    }
    const auto makeGroup = [&](std::string path) {
        model::RangeGroup g = model::RangeGroup::make(std::move(path));
        g.supportsTrackGeneration = settings_.supportsTrackGeneration;
        g.supportsReporting = settings_.supportsReporting;
        return g;
    };
    out.push_back(makeGroup(sectionsGroupPath(settings_)));

    // Each lap's index, read once.
    struct LapInfo {
        const model::Range* lap = nullptr;
        LapIndex index;
    };
    std::vector<LapInfo> lapInfo;
    lapInfo.reserve(laps->ranges.size());
    for (const model::Range& lap : laps->ranges) {
        if (cancel.cancelled()) {
            return cancelledFailure();
        }
        lapInfo.push_back(LapInfo{&lap, indexOf ? indexOf(lap) : LapIndex{}});
    }

    const double trackLength = map.lengthM;
    for (const std::string& set : sets) {
        // The sections of the set in track order.
        std::vector<const TrackSection*> members;
        for (const TrackSection& s : map.sections) {
            if (s.set == set) {
                members.push_back(&s);
            }
        }
        std::stable_sort(members.begin(), members.end(), [](const TrackSection* a, const TrackSection* b) {
            return a->startM != b->startM ? a->startM < b->startM : a->endM < b->endM;
        });
        // The categories present, in order of first appearance along the track.
        std::vector<std::string> categories;
        for (const TrackSection* s : members) {
            if (std::find(categories.begin(), categories.end(), s->category) == categories.end()) {
                categories.push_back(s->category);
            }
        }
        model::RangeGroup setGroup = makeGroup(setGroupPath(settings_, set));
        std::vector<model::RangeGroup> categoryGroups;
        categoryGroups.reserve(categories.size());
        for (const std::string& c : categories) {
            categoryGroups.push_back(makeGroup(categoryGroupPath(settings_, set, c)));
        }
        for (const LapInfo& info : lapInfo) {
            if (cancel.cancelled()) {
                return cancelledFailure();
            }
            const DistanceIndex* index = info.index.index.get();
            if (index == nullptr || index->lengthM() <= 0.0 || trackLength <= 0.0) {
                continue;
            }
            const model::Range& lap = *info.lap;
            const double stretch = index->stretch() > 0.0 ? index->stretch() : 1.0;
            const double lapLength = index->lengthM() * stretch; // stretched lap distance
            const auto timeOf = [&](double lapM) {
                return lap.start + static_cast<Micros>(std::llround(index->timeAt(lapM / stretch) * 1e6));
            };
            const bool startsAtBeacon = info.index.startsAtBeacon;
            const bool endsAtBeacon = info.index.endsAtBeacon;
            std::set<std::string, std::less<>> qualifiers;
            for (const TrackSection* s : members) {
                const std::optional<double> a = lapDistanceOfTrackDistance(s->startM, lapLength, trackLength,
                                                                           startsAtBeacon, endsAtBeacon);
                if (!a || *a >= lapLength) {
                    continue; // before the start of a lap measured backwards, or after the lap end
                }
                const double bRaw =
                    lapDistanceOfTrackDistance(s->endM, lapLength, trackLength, startsAtBeacon, endsAtBeacon)
                        .value_or(lapLength);
                // A lap that does not end at a BCN (an in lap, a lap cut by a reset) keeps no section that
                // would reach past its end: a truncated section is a bogus short time (04 section 13 item 5,
                // the mirror of the out lap's rule; D-195 (d)).
                if (!endsAtBeacon && bRaw > lapLength + 1e-6) {
                    continue;
                }
                const double b = std::min(lapLength, bRaw);
                // A section reaching a lap bound ends (or starts) exactly there, so the sections of a lap add
                // up to its lap time even where the distance stalls before the beacon.
                const Micros t0 = *a <= 1e-6 ? lap.start : std::clamp(timeOf(*a), lap.start, lap.end);
                const Micros t1 = b >= lapLength - 1e-6 ? lap.end : std::clamp(timeOf(b), lap.start, lap.end);
                if (t1 <= t0) {
                    continue;
                }
                std::string qualifier = s->name;
                if (!qualifiers.insert(qualifier).second) {
                    qualifier += "\x1f" + s->id; // two sections of one name in a set
                    qualifiers.insert(qualifier);
                }
                model::Range range;
                range.kind = model::RangeKind::Section;
                range.lapClass = lap.lapClass;
                range.startMarker = lap.startMarker;
                range.endMarker = lap.endMarker;
                range.start = t0;
                range.end = t1;
                range.name = s->name;
                range.abbrev = s->id;
                range.number = lap.number;
                range.trusted = lap.trusted;
                range.enabled = lap.enabled;
                range.id = model::rangeId(setGroup.path, lap.startMarker, qualifier);
                setGroup.ranges.push_back(range);
                const auto c = static_cast<std::size_t>(
                    std::find(categories.begin(), categories.end(), s->category) - categories.begin());
                model::Range child = range;
                child.id = model::rangeId(categoryGroups[c].path, lap.startMarker, qualifier);
                categoryGroups[c].ranges.push_back(std::move(child));
            }
        }
        const auto byTime = [](const model::Range& a, const model::Range& b) {
            return a.start != b.start ? a.start < b.start : a.end < b.end;
        };
        std::stable_sort(setGroup.ranges.begin(), setGroup.ranges.end(), byTime);
        out.push_back(std::move(setGroup));
        for (model::RangeGroup& g : categoryGroups) {
            std::stable_sort(g.ranges.begin(), g.ranges.end(), byTime);
            out.push_back(std::move(g));
        }
    }
    return out;
}

namespace {

// The proportional scale of a complete lap within the tolerance, else nullopt.
std::optional<double> proportionalScale(double lapLengthM, double trackLengthM, bool startsAtBeacon,
                                        bool endsAtBeacon) noexcept {
    if (!startsAtBeacon || !endsAtBeacon || !(lapLengthM > 0.0) || !(trackLengthM > 0.0)) {
        return std::nullopt;
    }
    const double ratio = lapLengthM / trackLengthM;
    if (std::abs(ratio - 1.0) > kLapLengthTolerance) {
        return std::nullopt;
    }
    return ratio;
}

} // namespace

std::optional<double> lapDistanceOfTrackDistance(double trackM, double lapLengthM, double trackLengthM,
                                                 bool lapStartsAtBeacon, bool lapEndsAtBeacon) noexcept {
    if (const std::optional<double> scale =
            proportionalScale(lapLengthM, trackLengthM, lapStartsAtBeacon, lapEndsAtBeacon)) {
        return trackM * *scale;
    }
    if (lapStartsAtBeacon) {
        return trackM;
    }
    const double d = lapLengthM - (trackLengthM - trackM);
    if (d < 0.0) {
        return std::nullopt;
    }
    return d;
}

double trackDistanceOfLapDistance(double lapM, double lapLengthM, double trackLengthM, bool lapStartsAtBeacon,
                                  bool lapEndsAtBeacon) noexcept {
    if (const std::optional<double> scale =
            proportionalScale(lapLengthM, trackLengthM, lapStartsAtBeacon, lapEndsAtBeacon)) {
        return lapM / *scale;
    }
    if (lapStartsAtBeacon) {
        return lapM;
    }
    return trackLengthM - (lapLengthM - lapM);
}

} // namespace esses::analysis
