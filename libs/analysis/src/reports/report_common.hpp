// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private helpers shared by ReportEngine and TimeReportEngine (04 sections 12-13): the range a ReportSource
// names, the chronological order of sources by their Log Date / Log Time details, and colour text.
#pragma once

#include <esses/analysis/report_engine.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace esses::analysis::reports_detail {

// The layer and range a ReportSource names: the layer of its RangeRef (ReportSource::layer when the ref
// has none), the group of the ref's path, the range by id, else by the ref's fallback name. nullptr when
// any part is missing.
struct ResolvedRange {
    const model::DataLayer* layer = nullptr;
    const model::Range* range = nullptr;
};
ResolvedRange resolveRange(const ReportSource& source);

// Seconds since 1970-01-01 of the source's local Log Date and Log Time (a date-time detail, or text
// "dd/mm/yyyy" / "yyyy-mm-dd" and "hh:mm[:ss]"); nullopt without a usable Log Date. A Log Date without a Log
// Time counts as its midnight unless the date is a date-time, whose own time of day is used.
std::optional<std::int64_t> chronologicalKey(const model::DataSource& source);

// The rank of every source of `sources` in chronological order (04 section 12 item 6): sources with a key
// first, by key, then the others; ties keep the order of first appearance. Ranges of one DataSource share
// its rank. ranks[i] belongs to sources[i].
std::vector<std::size_t> chronologicalRanks(const std::vector<ReportSource>& sources);

// "#rrggbb" (alpha ff) or "#rrggbbaa" as 0xRRGGBBAA; nullopt for anything else.
std::optional<std::uint32_t> parseColour(std::string_view text) noexcept;

} // namespace esses::analysis::reports_detail
