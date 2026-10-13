// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ComponentSpec: the persisted component record of workbook-1 (04 section 7.3 is the canonical header, 02
// section 6.4 the envelope; D-019, D-027). Channel references inside `body` are names. `body` is
// type-specific (04 sections 8.14-17) and validated by schemas/components/<type>-<version>.schema.json; a
// component of an unknown type is preserved byte-for-byte (`known == false`, body never re-serialised from a
// typed struct) and rendered as a placeholder.
//
// JSON (a workbook-1 "components" item, schemas/workbook-1.schema.json): every header member is written (full
// form: id, type, version, rect [l, t, r, b], z, caption {show, auto, text}, links, legend, activeChannel);
// "body" and "x-i2" are written when not null. `body` and `xI2` are JsonDoc (model::OpaqueJson is the same
// type since M1). The known types are the ten MVP component types of 04 sections 8-17 at version 1; any other
// (type, version) is preserved with known == false.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc

#include <string>
#include <string_view>

namespace esses::project {

struct Rect { // fractions 0..1 of the worksheet; overlap allowed
    double l = 0.0;
    double t = 0.0;
    double r = 1.0;
    double b = 1.0;
    friend bool operator==(const Rect&, const Rect&) = default;
};

struct Links {
    bool zoom = true;
    bool cursor = true;
    bool datum = true;
    friend bool operator==(const Links&, const Links&) = default;
};

struct Caption { // JSON {show, auto, text}
    bool show = true;
    bool autoText = true;
    std::string text;
    friend bool operator==(const Caption&, const Caption&) = default;
};

struct Legend {
    bool channels = true;
    bool colour = false;
    friend bool operator==(const Legend&, const Legend&) = default;
};

struct ComponentSpec {
    Uuid id;          // component UUID: undo merge, properties identity, build key
    std::string type; // JSON type string: "time_distance", "values_list", ...
    int version = 1;
    Rect rect;
    int z = 0; // ascending paint order (04 section 7.3)
    Links links;
    Caption caption;
    Legend legend;
    std::string activeChannel;
    JsonDoc body;      // type-specific object; null when the component has none
    JsonDoc xI2;       // JSON key "x-i2": imported attributes not implemented (D-019 round-trip bag)
    bool known = true; // false: preserved verbatim, placeholder rendering

    // One component object of a workbook-1 document. Invalid with context.detail (a JSON pointer) on a
    // malformed header (a component without "id" is Invalid here; WorkbookFile derives missing ids), Format
    // on a JSON syntax error; an unknown `type` is not an error (known = false).
    static Result<ComponentSpec> parse(std::string_view json);
    // The canonical JSON object (02 section 6.5 rule 1), compact; "" when a member holds a non-finite number.
    std::string toJson() const;

    friend bool operator==(const ComponentSpec&, const ComponentSpec&) = default;
};

// The ten MVP component types (04 sections 8-17): time_distance, outing, scatter, histogram, channel_report,
// time_report, track_position, values_list, gauge_numeric, gauge_bar.
bool isKnownComponentType(std::string_view type) noexcept;
// The newest version of a known type this build reads (1 for every known type); 0 for an unknown type.
int supportedComponentVersion(std::string_view type) noexcept;

} // namespace esses::project
