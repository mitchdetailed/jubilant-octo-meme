// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MarkersDocument: schema markers-1, the fixture expectation expected.markers.json (05 section 9.1). It is
// the merged marker list of every layer after the companion merge and before the sidecar (each layer's
// baseMarkers), in model order, written with the marker serializer the sidecar uses (02 section 5.3 rule 8):
// keys id, class, group, tUs, name, comment, flags (omitted when 0), prov. The document is closed.
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::model {

struct DataSource;

struct MarkersDocument {
    static constexpr std::string_view kSchemaId = "urn:esses:markers-1";
    static constexpr int kSchemaMajor = 1;
    static constexpr int kSchemaVersion = 1;

    int schemaVersion = kSchemaVersion;
    std::map<std::string, std::vector<Marker>, std::less<>> layers; // layer name -> markers in model order
    std::optional<JsonDoc> legacy;                                  // "x-esses-legacy"

    // Every layer's baseMarkers (a layer without markers gets an empty list).
    static MarkersDocument fromSource(const DataSource& src);
    // Errors: Format (syntax; context.offset), Unsupported (another major), Invalid (context.detail =
    // pointer).
    static Result<MarkersDocument> parse(std::string_view json);
    Result<std::string> toJson() const; // canonical text with trailing LF

    friend bool operator==(const MarkersDocument&, const MarkersDocument&) = default;
};

} // namespace esses::model
