// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportSpec (04 section 12 items 2 and 8). Owner: the reports work (src/reports/).

#include <esses/analysis/report_spec.hpp>
#include <esses/base/json_nlohmann.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <utility>

namespace esses::analysis {

namespace {

using jsonio::asBool;
using jsonio::asNumber;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

constexpr std::array<std::pair<ReportStat, std::string_view>, 9> kStats{{
    {ReportStat::Avg, "avg"},
    {ReportStat::Min, "min"},
    {ReportStat::Max, "max"},
    {ReportStat::AbsMax, "absMax"},
    {ReportStat::Start, "start"},
    {ReportStat::End, "end"},
    {ReportStat::Change, "change"},
    {ReportStat::StdDev, "stdDev"},
    {ReportStat::Delta, "delta"},
}};

constexpr std::array<std::pair<ReportScope, std::string_view>, 3> kScopes{{
    {ReportScope::SelectedRanges, "selectedRanges"},
    {ReportScope::SelectedOutings, "selectedOutings"},
    {ReportScope::AllOutings, "allOutings"},
}};

} // namespace

std::string_view reportStatName(ReportStat stat) noexcept {
    for (const auto& [s, name] : kStats) {
        if (s == stat) {
            return name;
        }
    }
    return {};
}

std::optional<ReportStat> parseReportStat(std::string_view name) noexcept {
    for (const auto& [s, n] : kStats) {
        if (n == name) {
            return s;
        }
    }
    return std::nullopt;
}

std::string_view reportScopeName(ReportScope scope) noexcept {
    for (const auto& [s, name] : kScopes) {
        if (s == scope) {
            return name;
        }
    }
    return {};
}

std::optional<ReportScope> parseReportScope(std::string_view name) noexcept {
    for (const auto& [s, n] : kScopes) {
        if (n == name) {
            return s;
        }
    }
    return std::nullopt;
}

// The body's "report" object (04 section 12 item 8). Unknown members are ignored (the body schema is
// closed; componentChannelUses reads the channels of any body it can).
Result<ReportSpec> ReportSpec::fromJson(const JsonDoc& report) {
    if (report.isNull()) {
        return ReportSpec{};
    }
    const nlohmann::json j = jsonio::fromDoc(report);
    return jsonio::guardJson([&]() -> Result<ReportSpec> {
        ReportSpec spec;
        ObjectReader r(j, "");
        if (const nlohmann::json* p = r.optional("scope")) {
            const std::string& text = asString(*p, r.child("scope"));
            const std::optional<ReportScope> parsed = parseReportScope(text);
            if (!parsed) {
                violate(r.child("scope"), "scope is selectedRanges, selectedOutings or allOutings");
            }
            spec.scope = parsed.value_or(ReportScope::SelectedRanges);
        }
        if (const nlohmann::json* p = r.optional("headers")) {
            const std::string pointer = r.child("headers");
            if (!p->is_array()) {
                violate(pointer, "headers is an array of range-group paths");
            }
            spec.headers.clear();
            for (std::size_t i = 0; i < p->size(); ++i) {
                spec.headers.push_back(asString((*p)[i], jsonio::pointerIndex(pointer, i)));
            }
        }
        if (const nlohmann::json* p = r.optional("channels")) {
            const std::string pointer = r.child("channels");
            if (!p->is_array()) {
                violate(pointer, "channels is an array");
            }
            for (std::size_t i = 0; i < p->size(); ++i) {
                ObjectReader c((*p)[i], jsonio::pointerIndex(pointer, i));
                ReportChannel channel;
                if (const nlohmann::json* name = c.optional("name")) {
                    channel.name = asString(*name, c.child("name"));
                }
                if (const nlohmann::json* unit = c.optional("displayUnit")) {
                    channel.displayUnit = asString(*unit, c.child("displayUnit"));
                }
                if (const nlohmann::json* stats = c.optional("stats")) {
                    const std::string statsPointer = c.child("stats");
                    if (!stats->is_array()) {
                        violate(statsPointer, "stats is an array of statistic names");
                    }
                    for (std::size_t k = 0; k < stats->size(); ++k) {
                        const std::string at = jsonio::pointerIndex(statsPointer, k);
                        const std::optional<ReportStat> stat = parseReportStat(asString((*stats)[k], at));
                        if (!stat) {
                            violate(at,
                                    "unknown statistic (avg, min, max, absMax, start, end, change, stdDev, "
                                    "delta)");
                        }
                        channel.stats.push_back(stat.value_or(ReportStat::Avg));
                    }
                }
                spec.channels.push_back(std::move(channel));
            }
        }
        if (const nlohmann::json* p = r.optional("options")) {
            ObjectReader o(*p, r.child("options"));
            if (const nlohmann::json* q = o.optional("excludeOverlays")) {
                spec.options.excludeOverlays = asBool(*q, o.child("excludeOverlays"));
            }
            if (const nlohmann::json* q = o.optional("includeUntrusted")) {
                spec.options.includeUntrusted = asBool(*q, o.child("includeUntrusted"));
            }
            if (const nlohmann::json* q = o.optional("interpolateBoundaries")) {
                spec.options.interpolateBoundaries = asBool(*q, o.child("interpolateBoundaries"));
            }
            if (const nlohmann::json* q = o.optional("probeSeconds")) {
                spec.options.probeSeconds = asNumber(*q, o.child("probeSeconds"));
                if (spec.options.probeSeconds < 0.0) {
                    violate(o.child("probeSeconds"), "probeSeconds is not negative");
                }
            }
            if (const nlohmann::json* q = o.optional("zoomLinked")) {
                spec.options.zoomLinked = asBool(*q, o.child("zoomLinked"));
            }
            if (const nlohmann::json* q = o.optional("ignoreInterpolated")) {
                spec.options.ignoreInterpolated = asBool(*q, o.child("ignoreInterpolated")); // D-210 (c)
            }
        }
        return spec;
    });
}

// Every member, so a round trip through fromJson is exact; canonical member order (JsonDoc).
JsonDoc ReportSpec::toJson() const {
    nlohmann::json channelsJson = nlohmann::json::array();
    for (const ReportChannel& c : channels) {
        nlohmann::json stats = nlohmann::json::array();
        for (const ReportStat s : c.stats) {
            stats.push_back(std::string(reportStatName(s)));
        }
        nlohmann::json channel{{"name", c.name}, {"stats", std::move(stats)}};
        if (!c.displayUnit.empty()) {
            channel["displayUnit"] = c.displayUnit; // absent = the channel's default display unit (D-211)
        }
        channelsJson.push_back(std::move(channel));
    }
    nlohmann::json j{{"scope", std::string(reportScopeName(scope))},
                     {"headers", headers},
                     {"channels", std::move(channelsJson)},
                     {"options",
                      {{"excludeOverlays", options.excludeOverlays},
                       {"includeUntrusted", options.includeUntrusted},
                       {"interpolateBoundaries", options.interpolateBoundaries},
                       {"probeSeconds", options.probeSeconds},
                       {"zoomLinked", options.zoomLinked}}}};
    if (options.ignoreInterpolated) {
        j["options"]["ignoreInterpolated"] = true; // absent = false: M3 bodies read and write back unchanged
    }
    Result<JsonDoc> doc = jsonio::toDoc(j);
    return doc ? std::move(doc).value() : JsonDoc{}; // fails only on a non-finite probeSeconds
}

} // namespace esses::analysis
