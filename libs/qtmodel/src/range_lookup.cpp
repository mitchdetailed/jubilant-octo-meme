// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "range_lookup.hpp"

#include <algorithm>
#include <limits>

namespace esses::qt::detail {

const model::DataLayer* layerOf(const model::DataSource& src, std::string_view layer) {
    return src.layer(model::canonicalLayerName(layer));
}

const model::RangeGroup* groupOf(const model::DataSource& src, std::string_view layer,
                                 std::string_view path) {
    const model::DataLayer* l = layerOf(src, layer);
    return l == nullptr ? nullptr : l->ranges.find(path);
}

const model::Range* findRange(const model::DataSource& src, const model::RangeRef& r) {
    if (r.src != src.id) {
        return nullptr;
    }
    const model::RangeGroup* g = groupOf(src, r.layer, r.groupPath);
    if (g == nullptr) {
        return nullptr;
    }
    for (const model::Range& range : g->ranges) {
        if (range.id == r.range) {
            return &range;
        }
    }
    return nullptr;
}

model::RangeRef makeRef(const model::DataSource& src, std::string_view layer, std::string_view groupPath,
                        const model::Range& range) {
    model::RangeRef r;
    r.src = src.id;
    r.layer = std::string(model::canonicalLayerName(layer));
    r.groupPath = std::string(groupPath);
    r.range = range.id;
    r.name = range.name;
    return r;
}

const model::Range* outingRangeAt(const model::DataLayer& layer, Micros t) {
    const model::RangeGroup* g = layer.ranges.find(kOutingsGroup);
    if (g == nullptr || g->ranges.empty()) {
        return nullptr;
    }
    const model::Range* best = nullptr;
    Micros bestGap = std::numeric_limits<Micros>::max();
    for (const model::Range& o : g->ranges) {
        if (t >= o.start && t < o.end) {
            return &o;
        }
        const Micros gap = t < o.start ? o.start - t : t - o.end;
        if (gap < bestGap || (gap == bestGap && t >= o.end)) { // t at an end belongs to that outing
            best = &o;
            bestGap = gap;
        }
    }
    return best;
}

plot::OutingGeom outingAt(const model::DataLayer& layer, Micros t) {
    if (const model::Range* o = outingRangeAt(layer, t)) {
        return plot::OutingGeom{o->start, o->end};
    }
    return plot::OutingGeom{0, std::max<Micros>(layer.duration, t + 1)};
}

std::vector<const model::Range*> rangesWithin(const model::DataLayer& layer, std::string_view groupPath,
                                              Micros start, Micros end) {
    std::vector<const model::Range*> out;
    const model::RangeGroup* g = layer.ranges.find(groupPath);
    if (g == nullptr) {
        return out;
    }
    for (const model::Range& r : g->ranges) {
        if (r.end > start && r.start < end) {
            out.push_back(&r);
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const model::Range* a, const model::Range* b) { return a->start < b->start; });
    return out;
}

plot::RangeGeom geomOf(const model::Range& r) {
    plot::RangeGeom g;
    g.start = r.start;
    g.end = r.end;
    g.offsetUs = r.offsetUs;
    g.offsetM = r.offsetM;
    g.stretch = r.stretch.value_or(1.0);
    return g;
}

std::string sourceHex(const model::SourceId& id) {
    return id.hex();
}

} // namespace esses::qt::detail
