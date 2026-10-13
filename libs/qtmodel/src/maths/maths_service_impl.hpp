// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsService::Impl (private to esses_qtmodel): the per-(source, layer) MathGraphs, the registered built-in
// modules, the project's maths documents (global modules, compat switches, mappings and aliases), each
// source's local and transient modules, the selection the SelectionDependent nodes key on, and the in-flight
// bookkeeping of the posted completions. Shared by src/maths_service.cpp (M1: registration, require,
// completions) and src/maths/maths_service_m2.cpp (M2: modules, invalidation, status rows, live checks).
#pragma once

#include <esses/analysis/distance_index.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/gps_corrections.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/maths/math_selection.hpp>
#include <esses/maths/maths_compat.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/opaque_json.hpp>
#include <esses/model/resolver.hpp>
#include <esses/model/source.hpp>
#include <esses/qtmodel/maths_service.hpp>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::qt {

namespace mathsdetail {

using GraphKey = std::pair<model::SourceId, std::string>; // (source, canonical layer)

struct InflightKey {
    maths::ChannelKey key;
    maths::Version version;
    friend bool operator<(const InflightKey& a, const InflightKey& b) {
        if (a.key.src != b.key.src) {
            return a.key.src < b.key.src;
        }
        if (a.key.layer != b.key.layer) {
            return a.key.layer < b.key.layer;
        }
        if (a.key.nameCf != b.key.nameCf) {
            return a.key.nameCf < b.key.nameCf;
        }
        return a.version < b.version;
    }
};

} // namespace mathsdetail

struct MathsService::Impl {
    maths::CorrectedSpeedDistanceModule module;
    maths::GpsCorrectionsModule gps;
    model::OpaqueJson settings; // null: the module defaults
    bool enabled = true;
    std::map<model::SourceId, std::shared_ptr<const model::DataSource>> sources;
    std::vector<model::SourceId> order; // registration order (Channel Status rows)
    std::map<mathsdetail::GraphKey, std::unique_ptr<maths::MathGraph>> graphs;
    std::set<mathsdetail::InflightKey> inflight;
    std::set<mathsdetail::InflightKey> reported;  // errors already sent to Messages (once per version)
    std::set<mathsdetail::InflightKey> announced; // channelReady already emitted (once per version)

    // ---- M2: the project's maths documents and each source's local and transient modules
    std::vector<maths::MathModule> globals; // project order, origin Global
    maths::MathsCompat compat;
    model::MappingTable mappings;
    model::AliasTable aliases;
    std::map<model::SourceId, std::optional<maths::MathModule>> localOverride; // setLocalModule
    std::map<model::SourceId, maths::MathModule> transient;
    std::map<mathsdetail::GraphKey, std::optional<maths::MathModule>> local; // in effect per graph
    // The selection SelectionDependent nodes key on (03 sections 7.2, 7.5).
    maths::MathSelection selection;
    std::shared_ptr<const analysis::DistanceIndex> mainDist;
    std::shared_ptr<const analysis::DistanceIndex> refDist;

    maths::MathGraph* graph(const model::SourceId& src, std::string_view layer) {
        const auto it =
            graphs.find(mathsdetail::GraphKey{src, std::string(model::canonicalLayerName(layer))});
        return it == graphs.end() ? nullptr : it->second.get();
    }
    const maths::MathGraph* graph(const model::SourceId& src, std::string_view layer) const {
        const auto it =
            graphs.find(mathsdetail::GraphKey{src, std::string(model::canonicalLayerName(layer))});
        return it == graphs.end() ? nullptr : it->second.get();
    }
};

} // namespace esses::qt
