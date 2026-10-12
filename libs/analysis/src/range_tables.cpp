// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/analysis/range_tables.hpp>

#include <algorithm>

namespace esses::analysis {

std::shared_ptr<const RangeTables> RangeTables::build(const model::DataLayer& layer) {
    auto tables = std::make_shared<RangeTables>();
    layer.ranges.forEach([&](const model::RangeGroup& g) {
        std::vector<RangeInterval>& out = tables->groups_[g.path];
        out.reserve(g.ranges.size());
        for (const model::Range& r : g.ranges) {
            if (!r.enabled) {
                continue;
            }
            RangeInterval iv;
            iv.start = r.start;
            iv.end = r.end;
            iv.id = r.id;
            iv.lapClass = r.lapClass;
            iv.number = r.number;
            iv.trusted = r.trusted;
            out.push_back(iv);
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const RangeInterval& a, const RangeInterval& b) { return a.start < b.start; });
        tables->versions_[g.path] = g.version;
    });
    return tables;
}

std::span<const RangeInterval> RangeTables::group(std::string_view path) const noexcept {
    const auto it = groups_.find(path);
    if (it == groups_.end()) {
        return {};
    }
    return it->second;
}

const RangeInterval* RangeTables::find(std::string_view path, Micros t) const noexcept {
    const std::span<const RangeInterval> g = group(path);
    // The last interval starting at or before t; ranges of one group do not overlap.
    auto it = std::upper_bound(g.begin(), g.end(), t,
                               [](Micros v, const RangeInterval& iv) { return v < iv.start; });
    if (it == g.begin()) {
        return nullptr;
    }
    --it;
    return t < it->end ? &*it : nullptr;
}

std::uint64_t RangeTables::rangeVersion(std::string_view path) const noexcept {
    const auto it = versions_.find(path);
    return it == versions_.end() ? 0 : it->second;
}

} // namespace esses::analysis
