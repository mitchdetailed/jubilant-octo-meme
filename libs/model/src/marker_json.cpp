// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "marker_json.hpp"

#include "json_read.hpp"

#include <string>

namespace esses::model::detail {

using nlohmann::json;

json markerToJson(const Marker& m) {
    json j = json::object();
    j["id"] = m.id.str();
    j["class"] = std::string(markerClassName(m.cls));
    j["group"] = m.group;
    j["tUs"] = m.t;
    j["name"] = m.name;
    j["comment"] = m.comment;
    if (m.flags != 0) {
        j["flags"] = m.flags;
    }
    j["prov"] = std::string(provenanceName(m.prov));
    return j;
}

Marker markerFromJson(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    Marker m;
    m.id = MarkerId{asUuid(r.required("id"), r.child("id"))};
    const std::string& cls = asString(r.required("class"), r.child("class"));
    const auto parsedClass = parseMarkerClass(cls);
    if (!parsedClass) {
        violate(r.child("class"), "unknown marker class '" + cls + "'");
    }
    m.cls = *parsedClass;
    m.group = asString(r.required("group"), r.child("group"));
    if (m.cls == MarkerClass::DeviceEvent && m.group != kGroupDeviceEvents) {
        violate(r.child("group"), "a DeviceEvent marker belongs to group 'DeviceEvents'");
    }
    m.t = asInt(r.required("tUs"), r.child("tUs"));
    m.name = asString(r.required("name"), r.child("name"));
    m.comment = asString(r.required("comment"), r.child("comment"));
    if (const json* flags = r.optional("flags")) {
        m.flags = asU32(*flags, r.child("flags"));
    }
    const std::string& prov = asString(r.required("prov"), r.child("prov"));
    const auto parsedProv = parseProvenance(prov);
    if (!parsedProv) {
        violate(r.child("prov"), "unknown provenance '" + prov + "'");
    }
    m.prov = *parsedProv;
    r.closed();
    return m;
}

} // namespace esses::model::detail
