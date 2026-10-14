// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeReportEngine (04 section 13, AC-M3-03). Owner: the reports work (src/reports/).

#include "../series_access.hpp"
#include "report_common.hpp"

#include <esses/analysis/time_report_engine.hpp>
#include <esses/base/json_nlohmann.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace esses::analysis {

namespace {

using jsonio::asBool;
using jsonio::asNumber;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

// One lap row before the summaries: the source range and its sections by name.
struct LapRow {
    std::size_t source = 0;
    std::string name;
    bool trusted = true;
    const model::DataSource* src = nullptr;
    std::string layer;
    Micros start = 0;
    Micros end = 0;
    std::vector<std::pair<std::string, Micros>> sections; // in time order, first of each name
};

// Column titles in track order: the section sequences of every lap merged, a name not seen yet going right
// after the previous name of its own lap (the front when it is the lap's first).
std::vector<std::string> mergeSectionOrder(const std::vector<LapRow>& laps) {
    std::vector<std::string> order;
    for (const LapRow& lap : laps) {
        std::size_t insertAt = 0;
        for (const auto& section : lap.sections) {
            const std::string& name = section.first;
            const auto it = std::find(order.begin(), order.end(), name);
            if (it != order.end()) {
                insertAt = static_cast<std::size_t>(it - order.begin()) + 1;
                continue;
            }
            order.insert(order.begin() + static_cast<std::ptrdiff_t>(insertAt), name);
            ++insertAt;
        }
    }
    return order;
}

bool contiguous(const LapRow& a, const LapRow& b) noexcept {
    return a.src == b.src && model::canonicalLayerName(a.layer) == model::canonicalLayerName(b.layer) &&
           a.end == b.start;
}

TimeReportRow summaryRow(std::string name, const std::vector<std::optional<Micros>>& values) {
    TimeReportRow row;
    row.name = std::move(name);
    for (const std::optional<Micros>& v : values) {
        TimeReportCell cell;
        cell.time = v;
        row.cells.push_back(cell);
    }
    return row;
}

} // namespace

Result<TimeReportSpec> TimeReportSpec::fromJson(const JsonDoc& body) {
    if (body.isNull()) {
        return TimeReportSpec{};
    }
    const nlohmann::json j = jsonio::fromDoc(body);
    return jsonio::guardJson([&]() -> Result<TimeReportSpec> {
        TimeReportSpec spec;
        ObjectReader r(j, "");
        if (const nlohmann::json* p = r.optional("scope")) {
            const std::string& text = asString(*p, r.child("scope"));
            const std::optional<ReportScope> parsed = parseReportScope(text);
            if (!parsed || *parsed == ReportScope::AllOutings) {
                violate(r.child("scope"), "scope is selectedRanges or selectedOutings");
            }
            spec.scope = parsed.value_or(ReportScope::SelectedOutings);
        }
        if (const nlohmann::json* p = r.optional("rowGroup")) {
            spec.rowGroup = asString(*p, r.child("rowGroup"));
            if (spec.rowGroup.empty()) {
                violate(r.child("rowGroup"), "rowGroup is a range-group path");
            }
        }
        if (const nlohmann::json* p = r.optional("options")) {
            ObjectReader o(*p, r.child("options"));
            if (const nlohmann::json* q = o.optional("includeUntrusted")) {
                spec.includeUntrusted = asBool(*q, o.child("includeUntrusted"));
            }
        }
        if (const nlohmann::json* p = r.optional("summaries")) {
            ObjectReader s(*p, r.child("summaries"));
            if (const nlohmann::json* q = s.optional("eclectic")) {
                spec.summaries.eclectic = asBool(*q, s.child("eclectic"));
            }
            if (const nlohmann::json* q = s.optional("rollingMin")) {
                spec.summaries.rollingMin = asBool(*q, s.child("rollingMin"));
            }
            if (const nlohmann::json* q = s.optional("total")) {
                spec.summaries.total = asBool(*q, s.child("total"));
            }
        }
        if (const nlohmann::json* p = r.optional("bands")) {
            const std::string pointer = r.child("bands");
            if (!p->is_array()) {
                violate(pointer, "bands is an array");
            }
            for (std::size_t i = 0; i < p->size(); ++i) {
                ObjectReader b((*p)[i], jsonio::pointerIndex(pointer, i));
                TimeReportBand band;
                if (const nlohmann::json* q = b.optional("pct")) {
                    band.pct = asNumber(*q, b.child("pct"));
                    if (band.pct < 0.0) {
                        violate(b.child("pct"), "pct is not negative");
                    }
                }
                if (const nlohmann::json* q = b.optional("colour")) {
                    const std::optional<std::uint32_t> colour =
                        reports_detail::parseColour(asString(*q, b.child("colour")));
                    if (!colour) {
                        violate(b.child("colour"), "colour is #rrggbb or #rrggbbaa");
                    }
                    band.colour = colour.value_or(band.colour);
                }
                if (const nlohmann::json* q = b.optional("visible")) {
                    band.visible = asBool(*q, b.child("visible"));
                }
                spec.bands.push_back(band);
            }
        }
        return spec; // timeFormat is the view's
    });
}

Result<TimeReportTable> TimeReportEngine::build(const TimeReportSpec& spec, const ReportInputs& inputs,
                                                const CancelToken& cancel) {
    if (cancel.checkpoint()) {
        return detail::cancelledFailure();
    }
    // The laps in scope, chronological (source order by Log Date / Log Time, then time).
    const std::vector<std::size_t> ranks = reports_detail::chronologicalRanks(inputs.sources);
    struct Candidate {
        std::size_t index = 0;
        std::size_t rank = 0;
        reports_detail::ResolvedRange resolved;
    };
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < inputs.sources.size(); ++i) {
        const reports_detail::ResolvedRange resolved = reports_detail::resolveRange(inputs.sources[i]);
        if (resolved.range == nullptr || (!resolved.range->trusted && !spec.includeUntrusted)) {
            continue;
        }
        candidates.push_back(Candidate{i, ranks[i], resolved});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.resolved.range->start < b.resolved.range->start;
    });
    std::vector<LapRow> laps;
    for (const Candidate& c : candidates) {
        const ReportSource& s = inputs.sources[c.index];
        const model::Range& lap = *c.resolved.range;
        LapRow row;
        row.source = c.index;
        row.name = lap.name;
        row.trusted = lap.trusted;
        row.src = s.source.get();
        row.layer = c.resolved.layer->name;
        row.start = lap.start;
        row.end = lap.end;
        // The sections of rowGroup under the lap: those that start inside it.
        if (const model::RangeGroup* group = c.resolved.layer->ranges.find(spec.rowGroup)) {
            for (const model::Range& section : group->ranges) {
                if (section.start >= lap.end) {
                    break;
                }
                if (section.start < lap.start || !section.enabled) {
                    continue;
                }
                const bool seen = std::any_of(row.sections.begin(), row.sections.end(),
                                              [&section](const auto& p) { return p.first == section.name; });
                if (!seen) {
                    row.sections.emplace_back(section.name, section.duration());
                }
            }
        }
        laps.push_back(std::move(row));
    }
    if (cancel.checkpoint()) {
        return detail::cancelledFailure();
    }

    TimeReportTable table;
    table.sections = mergeSectionOrder(laps);
    const std::size_t ns = table.sections.size();
    // Section times per lap at the column positions, then the lap time.
    std::vector<std::vector<std::optional<Micros>>> times(laps.size(),
                                                          std::vector<std::optional<Micros>>(ns + 1));
    for (std::size_t r = 0; r < laps.size(); ++r) {
        for (const auto& section : laps[r].sections) {
            const auto it = std::find(table.sections.begin(), table.sections.end(), section.first);
            times[r][static_cast<std::size_t>(it - table.sections.begin())] = section.second;
        }
        times[r][ns] = laps[r].end - laps[r].start;
    }
    // The fastest of every column (sections and the lap time).
    std::vector<std::optional<Micros>> fastest(ns + 1);
    for (const auto& lapTimes : times) {
        for (std::size_t c = 0; c <= ns; ++c) {
            if (lapTimes[c] && (!fastest[c] || *lapTimes[c] < *fastest[c])) {
                fastest[c] = lapTimes[c];
            }
        }
    }
    for (std::size_t r = 0; r < laps.size(); ++r) {
        TimeReportRow row;
        row.name = laps[r].name;
        row.trusted = laps[r].trusted;
        row.source = laps[r].source;
        for (std::size_t c = 0; c <= ns; ++c) {
            TimeReportCell cell;
            cell.time = times[r][c];
            const std::optional<Micros>& best = fastest[c];
            if (cell.time && best) {
                cell.fastest = *cell.time == *best;
                cell.band = bandOf(*cell.time, *best, spec.bands);
            }
            row.cells.push_back(cell);
        }
        table.rows.push_back(std::move(row));
    }

    std::vector<std::vector<std::optional<Micros>>> sectionTimes;
    sectionTimes.reserve(times.size());
    for (const auto& lapTimes : times) {
        sectionTimes.emplace_back(lapTimes.begin(), lapTimes.begin() + static_cast<std::ptrdiff_t>(ns));
    }
    if (spec.summaries.eclectic) {
        table.eclectic = summaryRow("Eclectic", eclectic(sectionTimes));
    }
    if (spec.summaries.rollingMin) {
        // Over each run of contiguous laps (a lap ending where the next starts), so a window never joins
        // laps that were not driven one after the other; the earliest window wins a tie.
        RollingMinimum best;
        best.bySection.assign(ns, std::nullopt);
        std::size_t runStart = 0;
        for (std::size_t r = 0; r <= laps.size(); ++r) {
            if (r < laps.size() && (r == runStart || contiguous(laps[r - 1], laps[r]))) {
                continue;
            }
            std::vector<std::optional<Micros>> chronological;
            for (std::size_t k = runStart; k < r; ++k) {
                chronological.insert(chronological.end(), sectionTimes[k].begin(), sectionTimes[k].end());
            }
            const RollingMinimum run = rollingMinimum(chronological, ns);
            if (run.sum && (!best.sum || *run.sum < *best.sum)) {
                best = run;
                best.firstIndex += runStart * ns;
            }
            runStart = r;
        }
        std::vector<std::optional<Micros>> cells = best.bySection;
        cells.push_back(best.sum);
        table.rollingMin = summaryRow("Rolling minimum", cells);
    }
    if (spec.summaries.total) {
        std::vector<std::optional<Micros>> sums(ns + 1);
        for (const auto& lapTimes : times) {
            for (std::size_t c = 0; c <= ns; ++c) {
                if (lapTimes[c]) {
                    sums[c] = sums[c].value_or(0) + *lapTimes[c];
                }
            }
        }
        table.total = summaryRow("Total", sums);
    }
    return table;
}

std::vector<std::optional<Micros>>
TimeReportEngine::eclectic(const std::vector<std::vector<std::optional<Micros>>>& rows) {
    std::size_t columns = 0;
    for (const auto& row : rows) {
        columns = std::max(columns, row.size());
    }
    std::vector<std::optional<Micros>> out(columns + 1);
    for (const auto& row : rows) {
        for (std::size_t c = 0; c < row.size(); ++c) {
            if (row[c] && (!out[c] || *row[c] < *out[c])) {
                out[c] = row[c];
            }
        }
    }
    if (columns > 0) {
        Micros total = 0;
        bool complete = true;
        for (std::size_t c = 0; c < columns; ++c) {
            if (const std::optional<Micros>& m = out[c]) {
                total += *m;
            } else {
                complete = false;
            }
        }
        if (complete) {
            out[columns] = total;
        }
    }
    return out;
}

RollingMinimum TimeReportEngine::rollingMinimum(std::span<const std::optional<Micros>> chronological,
                                                std::size_t sectionsPerLap) {
    RollingMinimum out;
    out.bySection.assign(sectionsPerLap, std::nullopt);
    if (sectionsPerLap == 0 || chronological.size() < sectionsPerLap) {
        return out;
    }
    // A sliding window: the sum of its present values and how many are missing.
    Micros sum = 0;
    std::size_t gaps = 0;
    for (std::size_t i = 0; i < sectionsPerLap; ++i) {
        if (const std::optional<Micros>& v = chronological[i]) {
            sum += *v;
        } else {
            ++gaps;
        }
    }
    for (std::size_t first = 0;; ++first) {
        if (gaps == 0 && (!out.sum || sum < *out.sum)) {
            out.sum = sum;
            out.firstIndex = first;
        }
        const std::size_t next = first + sectionsPerLap;
        if (next >= chronological.size()) {
            break;
        }
        if (const std::optional<Micros>& leaving = chronological[first]) {
            sum -= *leaving;
        } else {
            --gaps;
        }
        if (const std::optional<Micros>& entering = chronological[next]) {
            sum += *entering;
        } else {
            ++gaps;
        }
    }
    if (out.sum) {
        for (std::size_t j = out.firstIndex; j < out.firstIndex + sectionsPerLap; ++j) {
            out.bySection[j % sectionsPerLap] = chronological[j];
        }
    }
    return out;
}

int TimeReportEngine::bandOf(Micros cell, Micros fastest, std::span<const TimeReportBand> bands) noexcept {
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const TimeReportBand& b = bands[i];
        if (!b.visible) {
            continue;
        }
        const double limit = static_cast<double>(fastest) * (1.0 + b.pct / 100.0);
        if (static_cast<double>(cell) <= limit) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace esses::analysis
