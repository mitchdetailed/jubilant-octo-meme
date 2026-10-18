// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ScaleSpec and ColourBands JSON helpers (04 sections 7.3, 10.2, 11.2, 16, 17): the "scaleSpec" and
// "colourBands" definitions of schemas/components/*. Readers are strict about types (Invalid with a JSON
// pointer relative to the value) and tolerant about absence (a missing member keeps its default); writers
// emit every member except absent limits, so a body re-saved once is stable.

#include "body_json.hpp"

#include <esses/plotcore/component_bodies.hpp>

#include <cmath>
#include <utility>

namespace esses::plot {

namespace {

using nlohmann::json;

ScaleMode scaleModeOf(const std::string& text, const std::string& pointer) {
    if (text == "channel") {
        return ScaleMode::Channel;
    }
    if (text == "auto") {
        return ScaleMode::Auto;
    }
    if (text == "manual") {
        return ScaleMode::Manual;
    }
    jsonio::violate(pointer, "unknown scale mode \"" + text + "\"");
}

const char* scaleModeText(ScaleMode mode) noexcept {
    switch (mode) {
    case ScaleMode::Channel:
        return "channel";
    case ScaleMode::Auto:
        return "auto";
    case ScaleMode::Manual:
        return "manual";
    }
    return "channel";
}

BandOpen bandOpenOf(const std::string& text, const std::string& pointer) {
    if (text == "none") {
        return BandOpen::None;
    }
    if (text == "below") {
        return BandOpen::Below;
    }
    if (text == "above") {
        return BandOpen::Above;
    }
    jsonio::violate(pointer, "unknown band opening \"" + text + "\"");
}

const char* bandOpenText(BandOpen open) noexcept {
    switch (open) {
    case BandOpen::None:
        return "none";
    case BandOpen::Below:
        return "below";
    case BandOpen::Above:
        return "above";
    }
    return "none";
}

} // namespace

namespace detail {

analysis::UnitValue readUnitValue(const json& j, const std::string& pointer) {
    if (!j.is_object()) {
        jsonio::violate(pointer, "expected {v, unit}");
    }
    jsonio::ObjectReader r(j, pointer);
    analysis::UnitValue out;
    out.v = jsonio::asNumber(r.required("v"), r.child("v"));
    if (const json* unit = r.optional("unit")) {
        out.unit = jsonio::asString(*unit, r.child("unit"));
    }
    return out;
}

json unitValueJson(const analysis::UnitValue& value) {
    json o = json::object();
    o["v"] = value.v;
    o["unit"] = value.unit;
    return o;
}

ScaleSpec readScaleSpec(const json& j, const std::string& pointer) {
    if (!j.is_object()) {
        jsonio::violate(pointer, "expected a scale object");
    }
    jsonio::ObjectReader r(j, pointer);
    ScaleSpec out;
    if (const json* mode = r.optional("mode")) {
        out.mode = scaleModeOf(jsonio::asString(*mode, r.child("mode")), r.child("mode"));
    }
    if (const json* min = r.optional("min")) {
        out.min = readUnitValue(*min, r.child("min"));
    }
    if (const json* max = r.optional("max")) {
        out.max = readUnitValue(*max, r.child("max"));
    }
    return out;
}

json scaleSpecJson(const ScaleSpec& scale) {
    json o = json::object();
    o["mode"] = scaleModeText(scale.mode);
    if (scale.min) {
        o["min"] = unitValueJson(*scale.min);
    }
    if (scale.max) {
        o["max"] = unitValueJson(*scale.max);
    }
    return o;
}

std::vector<ColourBand> readColourBands(const json& j, const std::string& pointer) {
    if (!j.is_array()) {
        jsonio::violate(pointer, "expected an array of colour bands");
    }
    std::vector<ColourBand> out;
    for (std::size_t i = 0; i < j.size(); ++i) {
        const std::string ptr = jsonio::pointerIndex(pointer, i);
        const json& item = j[i];
        if (!item.is_object()) {
            jsonio::violate(ptr, "expected a colour band object");
        }
        jsonio::ObjectReader r(item, ptr);
        ColourBand band;
        if (const json* min = r.optional("min")) {
            band.min = readUnitValue(*min, r.child("min"));
        }
        if (const json* max = r.optional("max")) {
            band.max = readUnitValue(*max, r.child("max"));
        }
        if (const json* open = r.optional("open")) {
            band.open = bandOpenOf(jsonio::asString(*open, r.child("open")), r.child("open"));
        }
        if (const json* colour = r.optional("colour")) {
            band.colour = readColour(*colour, r.child("colour"));
        }
        if (const json* visible = r.optional("visible")) {
            band.visible = jsonio::asBool(*visible, r.child("visible"));
        }
        out.push_back(std::move(band));
    }
    return out;
}

json colourBandsJson(std::span<const ColourBand> bands) {
    json a = json::array();
    for (const ColourBand& b : bands) {
        json o = json::object();
        o["min"] = unitValueJson(b.min);
        o["max"] = unitValueJson(b.max);
        o["open"] = bandOpenText(b.open);
        o["colour"] = project::formatRgba(b.colour);
        o["visible"] = b.visible;
        a.push_back(std::move(o));
    }
    return a;
}

Rgba readColour(const json& j, const std::string& pointer) {
    const std::string& text = jsonio::asString(j, pointer);
    const std::optional<Rgba> colour = project::parseRgba(text);
    if (!colour) {
        jsonio::violate(pointer, R"(expected a colour "#rrggbb" or "#rrggbbaa")");
    }
    return *colour;
}

json bandAxisJson(const json* old, const analysis::BandAxis& axis) {
    if (old != nullptr && old->is_object()) {
        if (Result<JsonDoc> doc = jsonio::toDoc(*old)) {
            const Result<analysis::BandAxis> again = analysis::BandAxis::fromJson(*doc);
            if (again && *again == axis) {
                return *old;
            }
        }
    }
    return jsonio::fromDoc(axis.toJson());
}

json mergeOver(const JsonDoc& rest) {
    json j = rest.isObject() ? jsonio::fromDoc(rest) : json::object();
    return j.is_object() ? j : json::object();
}

JsonDoc docOf(const json& j, const JsonDoc& fallback) {
    Result<JsonDoc> doc = jsonio::toDoc(j);
    if (doc) {
        return std::move(*doc);
    }
    return fallback;
}

} // namespace detail

Result<ScaleSpec> parseScaleSpec(const JsonDoc& json) {
    return jsonio::guardJson(
        [&]() -> Result<ScaleSpec> { return detail::readScaleSpec(jsonio::fromDoc(json), std::string{}); });
}

JsonDoc scaleSpecToJson(const ScaleSpec& scale) {
    return detail::docOf(detail::scaleSpecJson(scale), JsonDoc{});
}

Result<std::vector<ColourBand>> parseColourBands(const JsonDoc& array) {
    return jsonio::guardJson([&]() -> Result<std::vector<ColourBand>> {
        if (array.isNull()) {
            return std::vector<ColourBand>{};
        }
        return detail::readColourBands(jsonio::fromDoc(array), std::string{});
    });
}

JsonDoc colourBandsToJson(std::span<const ColourBand> bands) {
    return detail::docOf(detail::colourBandsJson(bands), JsonDoc{});
}

std::optional<std::size_t> colourBandOf(std::span<const ColourBand> bands, double value) noexcept {
    if (std::isnan(value)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const ColourBand& b = bands[i];
        if (!b.visible) {
            continue;
        }
        bool inside = false;
        switch (b.open) {
        case BandOpen::None:
            inside = value >= b.min.v && value < b.max.v;
            break;
        case BandOpen::Below:
            inside = value < b.max.v;
            break;
        case BandOpen::Above:
            inside = value >= b.min.v;
            break;
        }
        if (inside) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace esses::plot
