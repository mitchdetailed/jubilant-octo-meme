// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private helpers of esses_qtmodel: finding ranges, outings and laps in a published DataSource (02 section
// 4.4) and turning them into the plotcore geometry (04 section 3.1). Pure functions over immutable
// snapshots; no Qt.
#pragma once

#include <esses/base/time.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/domain_mapper.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace esses::qt::detail {

inline constexpr std::string_view kOutingsGroup = "Outings";
inline constexpr std::string_view kLapsGroup = "Outings:Laps";
inline constexpr std::string_view kCorrSpeed = "Corr Speed";
inline constexpr std::string_view kCorrDist = "Corr Dist";

const model::DataLayer* layerOf(const model::DataSource& src, std::string_view layer);
const model::RangeGroup* groupOf(const model::DataSource& src, std::string_view layer, std::string_view path);
// The range `r` names in `src` (by layer, group path and RangeId), or nullptr.
const model::Range* findRange(const model::DataSource& src, const model::RangeRef& r);
// The RangeRef of `range` in group `groupPath` of `layer` (canonical layer name, the range's name).
model::RangeRef makeRef(const model::DataSource& src, std::string_view layer, std::string_view groupPath,
                        const model::Range& range);

// The "Outings" range of `layer` containing t (start <= t < end; t at the very end belongs to the last
// outing), else the nearest one; nullptr when the layer has no outing.
const model::Range* outingRangeAt(const model::DataLayer& layer, Micros t);
// The outing geometry around t: outingRangeAt, or [0, max(duration, t + 1)) when the layer has none.
plot::OutingGeom outingAt(const model::DataLayer& layer, Micros t);
// Ranges of `groupPath` overlapping [start, end), in time order.
std::vector<const model::Range*> rangesWithin(const model::DataLayer& layer, std::string_view groupPath,
                                              Micros start, Micros end);
plot::RangeGeom geomOf(const model::Range& r);

// 32 lower-case hex digits of a SourceId (task keys: chunk/<hex>, maths/<hex>/, distidx/<hex>/...).
std::string sourceHex(const model::SourceId& id);

} // namespace esses::qt::detail
