// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared fixtures of the report tests (report_*_tests.cpp): a source whose range tree is written by hand
// (outing, laps, track sections), the ReportSource of one of its ranges and a channel lookup over a table of
// (source, channel name) -> ReportChannelData.
#pragma once

#include "analysis_test_support.hpp"

#include <esses/analysis/report_engine.hpp>
#include <esses/base/hash.hpp>
#include <esses/model/detail.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace esses::analysis::test {

inline constexpr std::string_view kOutings = "Outings";
inline constexpr std::string_view kLaps = "Outings:Laps";
inline constexpr std::string_view kSectionsParent = "Outings:Laps:Track Sections";
inline constexpr std::string_view kSections = "Outings:Laps:Track Sections:Default";

struct RangeSpec {
    std::string name;
    double startS = 0.0;
    double endS = 0.0;
    bool trusted = true;
};

inline model::Range makeRange(std::string_view group, const RangeSpec& r, model::RangeKind kind) {
    model::Range out;
    out.kind = kind;
    out.name = r.name;
    out.start = sec(r.startS);
    out.end = sec(r.endS);
    out.trusted = r.trusted;
    out.id = model::rangeId(group, model::MarkerId{}, r.name + "@" + std::to_string(out.start));
    return out;
}

inline void addGroup(model::DataLayer& layer, std::string_view path, const std::vector<RangeSpec>& ranges,
                     model::RangeKind kind) {
    model::RangeGroup g = model::RangeGroup::make(std::string(path));
    for (const RangeSpec& r : ranges) {
        g.ranges.push_back(makeRange(path, r, kind));
    }
    REQUIRE(layer.ranges.setGroup(std::move(g)));
}

// A source with one "Normal" layer of `durationS` seconds, the outing [0, durationS) named "test.ld", the
// given laps and track sections, and the details (Log Date / Log Time, Driver, ...).
inline std::shared_ptr<const model::DataSource> reportSource(std::string_view tag, double durationS,
                                                             const std::vector<RangeSpec>& laps,
                                                             const std::vector<RangeSpec>& sections,
                                                             std::vector<model::Detail> details = {}) {
    model::DataLayer layer;
    layer.duration = sec(durationS);
    addGroup(layer, kOutings, {RangeSpec{"test.ld", 0.0, durationS, true}}, model::RangeKind::Outing);
    addGroup(layer, kLaps, laps, model::RangeKind::Lap);
    addGroup(layer, kSectionsParent, {}, model::RangeKind::Section);
    addGroup(layer, kSections, sections, model::RangeKind::Section);
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(tag);
    src->layers.push_back(std::move(layer));
    src->details = std::move(details);
    src->baseDetails = src->details;
    return src;
}

// The ReportSource of the range called `name` in group `group`.
inline ReportSource sourceRange(const std::shared_ptr<const model::DataSource>& src, std::string_view group,
                                std::string_view name, ReportSlotRole role = ReportSlotRole::Main) {
    ReportSource s;
    s.role = role;
    s.source = src;
    s.range.src = src->id;
    s.range.layer = "Normal";
    s.range.groupPath = std::string(group);
    const model::RangeGroup* g = src->layers.front().ranges.find(group);
    REQUIRE(g != nullptr);
    for (const model::Range& r : g->ranges) {
        if (r.name == name) {
            s.range.range = r.id;
            s.range.name = r.name;
            return s;
        }
    }
    FAIL("no range " << name << " in " << group);
    return s;
}

// (source, channel name) -> what the lookup returns; anything else is missing.
class ChannelTable {
public:
    void add(const std::shared_ptr<const model::DataSource>& src, std::string name, store::Series series,
             units::Converter toDisplay = {}, std::string unit = {}, int dps = 2,
             store::Interp interp = store::Interp::Linear) {
        ReportChannelData d;
        d.series = std::move(series);
        model::ChannelDescriptor desc;
        desc.id = model::channelIdLogged(src->id, "Normal", name, "test");
        desc.name = name;
        desc.loggedName = name;
        desc.interp = interp;
        d.descriptor = desc;
        d.toDisplay = toDisplay;
        d.displayUnit = std::move(unit);
        d.dps = dps;
        table_[{src.get(), std::move(name)}] = std::move(d);
    }
    // As add, with the descriptor's base unit set (the column-unit conversion of D-197 (g)).
    void addWithUnit(const std::shared_ptr<const model::DataSource>& src, std::string name,
                     store::Series series, units::UnitId base, units::Converter toDisplay, std::string unit,
                     int dps = 2) {
        add(src, name, std::move(series), toDisplay, std::move(unit), dps);
        ReportChannelData& d = table_[{src.get(), std::move(name)}];
        if (d.descriptor) {
            d.descriptor->unit = base;
        }
    }
    void addStatus(const std::shared_ptr<const model::DataSource>& src, std::string name,
                   model::ChannelStatus status) {
        ReportChannelData d;
        d.status = status;
        table_[{src.get(), std::move(name)}] = std::move(d);
    }
    ReportChannelLookup lookup() const {
        return [this](const ReportSource& s, std::string_view name) {
            ++calls_;
            const auto it = table_.find({s.source.get(), std::string(name)});
            return it != table_.end() ? it->second : ReportChannelData{};
        };
    }
    int calls() const noexcept { return calls_; }

private:
    std::map<std::pair<const model::DataSource*, std::string>, ReportChannelData> table_;
    mutable int calls_ = 0;
};

inline model::Detail detailText(std::string id, std::string value) {
    return model::Detail{std::move(id), std::move(value), model::Provenance::Companion, std::nullopt};
}

} // namespace esses::analysis::test
