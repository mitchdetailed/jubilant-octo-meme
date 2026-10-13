// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "json_read.hpp"
#include "marker_json.hpp"

#include <esses/model/markers_document.hpp>
#include <esses/model/source.hpp>

#include <string>
#include <utility>

namespace esses::model {

using nlohmann::json;

MarkersDocument MarkersDocument::fromSource(const DataSource& src) {
    MarkersDocument doc;
    for (const DataLayer& l : src.layers) {
        std::vector<Marker> markers = l.baseMarkers.empty() ? l.markers : l.baseMarkers;
        // markers-1 holds the merged set before the sidecar: no user markers, no edits.
        std::erase_if(markers, [](const Marker& m) { return m.prov == Provenance::User; });
        for (Marker& m : markers) {
            m.t = m.loggedT();
            m.originalT.reset();
            m.enabled = true;
        }
        sortMarkers(markers);
        doc.layers[std::string(canonicalLayerName(l.name))] = std::move(markers);
    }
    return doc;
}

Result<MarkersDocument> MarkersDocument::parse(std::string_view text) {
    auto parsed = detail::parseJson(text);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    const json& root = *parsed;
    return detail::guardJson([&root]() -> Result<MarkersDocument> {
        detail::ObjectReader r(root, "");
        detail::checkSchemaId(root, "markers", kSchemaMajor);
        r.optional("$schema");
        MarkersDocument doc;
        const std::int64_t version = detail::asInt(r.required("schemaVersion"), r.child("schemaVersion"));
        if (version < 1 || version > 1'000'000) {
            detail::violate(r.child("schemaVersion"), "schemaVersion must be a positive minor version");
        }
        doc.schemaVersion = static_cast<int>(version);
        const std::string layersPtr = r.child("layers");
        const json& layersJson = r.required("layers");
        if (!layersJson.is_object()) {
            detail::violate(layersPtr, "expected an object keyed by layer name");
        }
        for (auto it = layersJson.begin(); it != layersJson.end(); ++it) {
            if (it.key().empty()) {
                detail::violate(layersPtr, "layer names are non-empty (\"Normal\", not the empty alias)");
            }
            const std::string p = detail::pointerChild(layersPtr, it.key());
            if (!it.value().is_array()) {
                detail::violate(p, "expected an array of markers");
            }
            std::vector<Marker> markers;
            markers.reserve(it.value().size());
            for (std::size_t i = 0; i < it.value().size(); ++i) {
                markers.push_back(detail::markerFromJson(it.value()[i], detail::pointerIndex(p, i)));
            }
            doc.layers.emplace(it.key(), std::move(markers));
        }
        if (const json* legacyJson = r.optional("x-esses-legacy")) {
            auto o = jsonio::toDoc(*legacyJson);
            if (!o) {
                detail::violate(r.child("x-esses-legacy"), o.error().message);
            }
            doc.legacy = std::move(o).value();
        }
        r.closed();
        return doc;
    });
}

Result<std::string> MarkersDocument::toJson() const {
    json root = json::object();
    root["$schema"] = std::string(kSchemaId);
    root["schemaVersion"] = schemaVersion < 1 ? 1 : schemaVersion;
    json layersJson = json::object();
    for (const auto& [name, markers] : layers) {
        json a = json::array();
        for (const Marker& m : markers) {
            a.push_back(detail::markerToJson(m));
        }
        layersJson[name] = std::move(a);
    }
    root["layers"] = std::move(layersJson);
    if (legacy) {
        root["x-esses-legacy"] = jsonio::fromDoc(*legacy);
    }
    return jsonio::canonicalJson(root);
}

} // namespace esses::model
