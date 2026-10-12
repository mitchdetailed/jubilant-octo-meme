// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "component_json.hpp"

#include <esses/project/component_spec.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace esses::project {

namespace {

constexpr std::array<std::string_view, 10> kKnownTypes{
    "time_distance", "outing",         "scatter",     "histogram",     "channel_report",
    "time_report",   "track_position", "values_list", "gauge_numeric", "gauge_bar"};

} // namespace

bool isKnownComponentType(std::string_view type) noexcept {
    return std::find(kKnownTypes.begin(), kKnownTypes.end(), type) != kKnownTypes.end();
}

int supportedComponentVersion(std::string_view type) noexcept {
    return isKnownComponentType(type) ? 1 : 0;
}

namespace detail {

using jsonio::asNumber;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

namespace {

Rect readRect(const json& j, const std::string& pointer) {
    if (!j.is_array() || j.size() != 4) {
        violate(pointer, "rect is [l, t, r, b]: four fractions of the worksheet");
    }
    std::array<double, 4> v{};
    for (std::size_t i = 0; i < 4; ++i) {
        v[i] = asNumber(j[i], jsonio::pointerIndex(pointer, i));
        if (v[i] < 0.0 || v[i] > 1.0) {
            violate(jsonio::pointerIndex(pointer, i), "rect fractions lie in 0..1");
        }
    }
    if (!(v[0] < v[2]) || !(v[1] < v[3])) {
        violate(pointer, "rect needs l < r and t < b");
    }
    return Rect{v[0], v[1], v[2], v[3]};
}

} // namespace

ComponentSpec componentFromJson(const json& j, const std::string& pointer,
                                const std::optional<Uuid>& fallbackId, bool* derivedId) {
    ObjectReader r(j, pointer);
    ComponentSpec c;
    if (const json* id = r.optional("id")) {
        c.id = jsonio::asUuid(*id, r.child("id"));
    } else if (fallbackId) {
        c.id = *fallbackId;
        if (derivedId != nullptr) {
            *derivedId = true;
        }
    } else {
        violate(pointer, "required member 'id' is missing");
    }
    c.type = asString(r.required("type"), r.child("type"));
    if (!isLowerId(c.type, '_', 1, 1)) {
        violate(r.child("type"),
                "a component type is snake_case (\"time_distance\"), found '" + c.type + "'");
    }
    if (const json* v = r.optional("version")) {
        c.version = jsonio::asInt32(*v, r.child("version"));
        if (c.version < 1) {
            violate(r.child("version"), "a component version is at least 1");
        }
    }
    if (const json* v = r.optional("rect")) {
        c.rect = readRect(*v, r.child("rect"));
    }
    if (const json* v = r.optional("z")) {
        c.z = jsonio::asInt32(*v, r.child("z"));
    }
    if (const json* v = r.optional("caption")) {
        ObjectReader cr(*v, r.child("caption"));
        c.caption.show = optBool(cr, "show", true);
        c.caption.autoText = optBool(cr, "auto", true);
        c.caption.text = optString(cr, "text");
        cr.closed();
    }
    if (const json* v = r.optional("links")) {
        ObjectReader lr(*v, r.child("links"));
        c.links.zoom = optBool(lr, "zoom", true);
        c.links.cursor = optBool(lr, "cursor", true);
        c.links.datum = optBool(lr, "datum", true);
        lr.closed();
    }
    if (const json* v = r.optional("legend")) {
        ObjectReader gr(*v, r.child("legend"));
        c.legend.channels = optBool(gr, "channels", true);
        c.legend.colour = optBool(gr, "colour", false);
        gr.closed();
    }
    c.activeChannel = optString(r, "activeChannel");
    if (const json* v = r.optional("body")) {
        c.body = objectDoc(*v, r.child("body"));
    }
    if (const json* v = r.optional("x-i2")) {
        c.xI2 = objectDoc(*v, r.child("x-i2"));
    }
    r.closed();
    const int supported = supportedComponentVersion(c.type);
    c.known = supported != 0 && c.version <= supported;
    return c;
}

json componentToJson(const ComponentSpec& c) {
    json j = json::object();
    j["id"] = c.id.str();
    j["type"] = c.type;
    j["version"] = c.version;
    j["rect"] = json::array({c.rect.l, c.rect.t, c.rect.r, c.rect.b});
    j["z"] = c.z;
    j["caption"] = {{"show", c.caption.show}, {"auto", c.caption.autoText}, {"text", c.caption.text}};
    j["links"] = {{"zoom", c.links.zoom}, {"cursor", c.links.cursor}, {"datum", c.links.datum}};
    j["legend"] = {{"channels", c.legend.channels}, {"colour", c.legend.colour}};
    j["activeChannel"] = c.activeChannel;
    if (!c.body.compact().empty()) {
        j["body"] = jsonio::fromDoc(c.body);
    }
    if (!c.xI2.compact().empty()) {
        j["x-i2"] = jsonio::fromDoc(c.xI2);
    }
    return j;
}

} // namespace detail

Result<ComponentSpec> ComponentSpec::parse(std::string_view json) {
    return detail::parseDocument<ComponentSpec>(json, [](const detail::json& root) -> Result<ComponentSpec> {
        return detail::componentFromJson(root, "", std::nullopt, nullptr);
    });
}

std::string ComponentSpec::toJson() const {
    auto text = jsonio::compactCanonicalJson(detail::componentToJson(*this));
    return text ? std::move(text).value() : std::string();
}

} // namespace esses::project
