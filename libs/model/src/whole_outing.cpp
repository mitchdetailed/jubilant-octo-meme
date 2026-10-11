// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/model/source.hpp>
#include <esses/model/whole_outing.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace esses::model {

namespace {

struct OpenOuting {
    MarkerId start;
    Micros t = 0;
};

} // namespace

RangeGroup buildWholeOutingGroup(const DataLayer& layer, const std::vector<Detail>& details,
                                 const WholeOutingOptions& options) {
    RangeGroup group = RangeGroup::make(options.groupPath);
    group.supportsTrackGeneration = options.supportsTrackGeneration;
    group.supportsReporting = options.supportsReporting;

    std::string name;
    if (const Detail* d = findDetail(details, options.nameDetail)) {
        name = detailValueText(d->value);
    }
    if (name.empty()) {
        name = options.fallbackName;
    }

    Micros lastT = 0;
    for (const Marker& m : layer.markers) {
        lastT = std::max(lastT, m.t);
    }
    const Micros layerEnd = std::max(layer.duration, lastT);

    const auto emit = [&](const OpenOuting& open, const MarkerId& endMarker, Micros end) {
        const int index = static_cast<int>(group.ranges.size()) + 1;
        Range r;
        r.id = rangeId(group.path, open.start);
        r.kind = RangeKind::Outing;
        r.lapClass = LapClass::None;
        r.startMarker = open.start;
        r.endMarker = endMarker;
        r.start = open.t;
        r.end = std::max(end, open.t);
        r.name = name;
        r.abbrev = std::to_string(index);
        r.number = index;
        r.trusted = options.trusted;
        group.ranges.push_back(std::move(r));
    };

    std::optional<OpenOuting> open;
    bool sawSol = false;
    const Marker* firstEol = nullptr;
    for (const Marker& m : layer.markers) {
        if (!m.enabled || !m.group.empty()) {
            continue;
        }
        if (m.cls == MarkerClass::EOL && firstEol == nullptr) {
            firstEol = &m;
        }
        if (m.cls == MarkerClass::SOL) {
            sawSol = true;
            if (open) {
                emit(*open, m.id, m.t);
            }
            open = OpenOuting{m.id, m.t};
        } else if (m.cls == MarkerClass::EOL && open) {
            emit(*open, m.id, m.t);
            open.reset();
        }
    }
    if (open) {
        emit(*open, MarkerId{}, layerEnd);
    }
    if (!sawSol) {
        // The id an importer-synthesised SOL at t = 0 would have, so the outing's RangeId does not change
        // when an importer starts synthesising it.
        const OpenOuting synthetic{markerIdLogged(MarkerClass::SOL, 0, kGroupIntrinsic, 0), 0};
        if (firstEol != nullptr) {
            emit(synthetic, firstEol->id, firstEol->t);
        } else {
            emit(synthetic, MarkerId{}, layerEnd);
        }
    }
    return group;
}

SidecarApplyReport rebuildWholeOutingRanges(DataSource& src, const WholeOutingOptions& options) {
    SidecarApplyReport report;
    for (DataLayer& layer : src.layers) {
        const RangeGroupTree previous = layer.ranges;
        layer.ranges.setGroup(buildWholeOutingGroup(layer, src.details, options));
        report.append(src.sidecar.applyRangeOverrides(layer));
        layer.ranges.inheritVersions(previous);
    }
    return report;
}

} // namespace esses::model
