// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// componentChannelUses: the one dispatch by component type (M3 contract, final). Each type's body parser
// supplies its channel list, so the owners of the bodies never edit this file.

#include <esses/analysis/report_spec.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/plotcore/component_bodies.hpp>
#include <esses/plotcore/gauge_geometry.hpp>
#include <esses/plotcore/histogram_geometry.hpp>
#include <esses/plotcore/outing_geometry.hpp>
#include <esses/plotcore/scatter_geometry.hpp>
#include <esses/plotcore/time_distance_builder.hpp>

#include <nlohmann/json.hpp>

#include <string_view>
#include <utility>

namespace esses::plot {

namespace {

void appendTraces(std::vector<ChannelUse>& out, const TimeDistanceBody& body) {
    for (const TdGroup& g : body.groups) {
        for (const TdTrace& t : g.traces) {
            out.push_back(ChannelUse{t.channel, t.displayUnit, t.dps});
        }
    }
}

std::vector<ChannelUse> valuesListChannels(const JsonDoc& body) {
    std::vector<ChannelUse> out;
    const nlohmann::json j = jsonio::fromDoc(body);
    const auto rows = j.find("channels");
    if (rows == j.end() || !rows->is_array()) {
        return out;
    }
    for (const nlohmann::json& row : *rows) {
        if (!row.is_object()) {
            continue;
        }
        ChannelUse use;
        if (const auto c = row.find("channel"); c != row.end() && c->is_string()) {
            use.name = c->get<std::string>();
        }
        if (const auto u = row.find("displayUnit"); u != row.end() && u->is_string()) {
            use.displayUnit = u->get<std::string>();
        }
        if (const auto d = row.find("dps"); d != row.end() && d->is_number_integer()) {
            use.dps = d->get<int>();
        }
        out.push_back(std::move(use));
    }
    return out;
}

std::vector<ChannelUse> channelReportChannels(const JsonDoc& body) {
    std::vector<ChannelUse> out;
    const nlohmann::json j = jsonio::fromDoc(body);
    const auto report = j.find("report");
    if (report == j.end() || !report->is_object()) {
        return out;
    }
    Result<JsonDoc> doc = jsonio::toDoc(*report);
    if (!doc) {
        return out;
    }
    Result<analysis::ReportSpec> spec = analysis::ReportSpec::fromJson(*doc);
    if (!spec) {
        return out;
    }
    for (const analysis::ReportChannel& c : spec->channels) {
        out.push_back(ChannelUse{c.name, {}, std::nullopt});
    }
    return out;
}

template <class Body> std::vector<ChannelUse> listOf(const JsonDoc& json) {
    Result<Body> body = Body::parse(json);
    return body ? body->channels() : std::vector<ChannelUse>{};
}

} // namespace

std::vector<ChannelUse> componentChannelUses(const project::ComponentSpec& spec) {
    std::vector<ChannelUse> out;
    if (!spec.known || !spec.body.isObject()) {
        return out;
    }
    const std::string_view type = spec.type;
    if (type == "time_distance") {
        if (Result<TimeDistanceBody> body = TimeDistanceBody::parse(spec.body)) {
            appendTraces(out, *body);
        }
    } else if (type == "outing") {
        if (Result<OutingBody> body = OutingBody::parse(spec.body)) {
            appendTraces(out, body->graph);
        }
    } else if (type == "scatter") {
        out = listOf<ScatterBody>(spec.body);
    } else if (type == "histogram") {
        out = listOf<HistogramBody>(spec.body);
    } else if (type == "gauge_numeric") {
        if (Result<GaugeNumericBody> body = GaugeNumericBody::parse(spec.body)) {
            out.push_back(ChannelUse{body->channel, body->displayUnit, body->displayDps});
        }
    } else if (type == "gauge_bar") {
        if (Result<GaugeBarBody> body = GaugeBarBody::parse(spec.body)) {
            out.push_back(ChannelUse{body->channel, body->displayUnit, std::nullopt});
        }
    } else if (type == "values_list") {
        out = valuesListChannels(spec.body);
    } else if (type == "channel_report") {
        out = channelReportChannels(spec.body);
    }
    // time_report and track_position show no channels.
    std::erase_if(out, [](const ChannelUse& u) { return u.name.empty(); });
    return out;
}

} // namespace esses::plot
