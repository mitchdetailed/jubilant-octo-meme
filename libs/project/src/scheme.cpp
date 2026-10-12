// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "doc_io.hpp"

#include <esses/project/scheme.hpp>

#include <string>
#include <utility>

namespace esses::project {

namespace {

using detail::json;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

Rgba readColour(const json& j, const std::string& pointer) {
    const std::string& text = asString(j, pointer);
    const auto c = parseRgba(text);
    if (!c) {
        violate(pointer, R"(expected a colour "#rrggbb" or "#rrggbbaa", found ')" + text + "'");
    }
    return *c;
}

std::vector<Rgba> readPalette(const json& j, const std::string& pointer) {
    if (!j.is_array()) {
        violate(pointer, "a palette is an array of colours");
    }
    std::vector<Rgba> out;
    out.reserve(j.size());
    for (std::size_t i = 0; i < j.size(); ++i) {
        out.push_back(readColour(j[i], jsonio::pointerIndex(pointer, i)));
    }
    return out;
}

bool isRoleName(std::string_view name) noexcept {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

FontSpec readFont(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    FontSpec f;
    f.family = asString(r.required("family"), r.child("family"));
    if (f.family.empty()) {
        violate(r.child("family"), "a font family is not empty");
    }
    f.pointSize = jsonio::asNumber(r.required("pointSize"), r.child("pointSize"));
    if (f.pointSize < 1.0 || f.pointSize > 200.0) {
        violate(r.child("pointSize"), "pointSize lies in 1..200");
    }
    f.weight = jsonio::asInt32(r.required("weight"), r.child("weight"));
    if (f.weight < 1 || f.weight > 1000) {
        violate(r.child("weight"), "weight lies in 1..1000 (CSS/OpenType weight)");
    }
    f.italic = detail::optBool(r, "italic", false);
    f.strikeOut = detail::optBool(r, "strikeOut", false);
    r.closed();
    return f;
}

json fontToJson(const FontSpec& f) {
    json j = {{"family", f.family}, {"pointSize", f.pointSize}, {"weight", f.weight}, {"italic", f.italic}};
    if (f.strikeOut) {
        j["strikeOut"] = true;
    }
    return j;
}

json paletteToJson(const std::vector<Rgba>& palette) {
    json a = json::array();
    for (const Rgba c : palette) {
        a.push_back(formatRgba(c));
    }
    return a;
}

json schemeToJson(const Scheme& s) {
    json root = detail::newDocument("scheme");
    root["name"] = s.name;
    root["background"] = s.background;
    if (!s.roles.empty()) {
        json roles = json::object();
        for (const auto& [roleName, colour] : s.roles) {
            roles[roleName] = formatRgba(colour);
        }
        root["roles"] = std::move(roles);
    }
    if (!s.channelPalette.empty()) {
        root["channelPalette"] = paletteToJson(s.channelPalette);
    }
    if (!s.overlayPalette.empty()) {
        root["overlayPalette"] = paletteToJson(s.overlayPalette);
    }
    root["overlayStyle"] = {{"colourSource", s.overlayStyle.colourSource},
                            {"dash", s.overlayStyle.dash},
                            {"lightness", s.overlayStyle.lightness},
                            {"widthScale", s.overlayStyle.widthScale}};
    if (!s.fonts.empty()) {
        json fonts = json::object();
        for (const auto& [fontRole, spec] : s.fonts) {
            fonts[fontRole] = fontToJson(spec);
        }
        root["fonts"] = std::move(fonts);
    }
    if (!s.print.compact().empty()) {
        root["print"] = jsonio::fromDoc(s.print);
    }
    detail::putUnknown(root, s.unknown);
    return root;
}

} // namespace

Rgba Scheme::role(std::string_view roleName, Rgba fallback) const {
    const auto it = roles.find(roleName);
    return it == roles.end() ? fallback : it->second;
}

FontSpec Scheme::font(std::string_view roleName) const {
    if (const auto it = fonts.find(roleName); it != fonts.end()) {
        return it->second;
    }
    if (const auto it = fonts.find(std::string_view("caption")); it != fonts.end()) {
        return it->second;
    }
    return FontSpec{};
}

Rgba Scheme::channelColour(int index, Rgba fallback) const {
    if (channelPalette.empty() || index < 0) {
        return fallback;
    }
    return channelPalette[static_cast<std::size_t>(index) % channelPalette.size()];
}

Rgba Scheme::overlayColour(int overlayNumber, Rgba fallback) const {
    if (overlayPalette.empty() || overlayNumber < 1) {
        return fallback;
    }
    return overlayPalette[static_cast<std::size_t>(overlayNumber - 1) % overlayPalette.size()];
}

std::vector<std::string> Scheme::fillMissingFrom(const Scheme& shipped) {
    std::vector<std::string> filled;
    const std::string prefix = "scheme '" + name + "': ";
    const std::string from = " missing, taken from '" + shipped.name + "'";
    const auto line = [&](std::string_view what, std::string_view item) {
        std::string text = prefix;
        text += what;
        text += " '";
        text += item;
        text += '\'';
        text += from;
        return text;
    };
    for (const auto& [roleName, colour] : shipped.roles) {
        if (roles.emplace(roleName, colour).second) {
            filled.push_back(line("role", roleName));
        }
    }
    for (const auto& [fontRole, spec] : shipped.fonts) {
        if (fonts.emplace(fontRole, spec).second) {
            filled.push_back(line("font", fontRole));
        }
    }
    if (channelPalette.empty() && !shipped.channelPalette.empty()) {
        channelPalette = shipped.channelPalette;
        filled.push_back(prefix + "channelPalette" + from);
    }
    if (overlayPalette.empty() && !shipped.overlayPalette.empty()) {
        overlayPalette = shipped.overlayPalette;
        filled.push_back(prefix + "overlayPalette" + from);
    }
    return filled;
}

Result<Scheme> Scheme::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::parseDocument<Scheme>(json, [&](const detail::json& root) -> Result<Scheme> {
        ObjectReader r(root, "");
        detail::readSchemaHeader(r, "scheme", log);
        Scheme s;
        s.name = asString(r.required("name"), r.child("name"));
        if (s.name.empty()) {
            violate(r.child("name"), "a scheme name is not empty");
        }
        s.background = detail::enumString(r.required("background"), r.child("background"), {"light", "dark"});
        if (const detail::json* rolesJson = r.optional("roles")) {
            ObjectReader rr(*rolesJson, r.child("roles"));
            for (auto it = rolesJson->begin(); it != rolesJson->end(); ++it) {
                if (!isRoleName(it.key())) {
                    violate(rr.child(it.key()), "role names are camelCase identifiers");
                }
                s.roles.emplace(it.key(), readColour(it.value(), rr.child(it.key())));
            }
        }
        if (const detail::json* p = r.optional("channelPalette")) {
            s.channelPalette = readPalette(*p, r.child("channelPalette"));
            if (s.channelPalette.size() < 10) {
                violate(r.child("channelPalette"),
                        "channelPalette has at least 10 colours (06 section 16 rule 2)");
            }
        }
        if (const detail::json* p = r.optional("overlayPalette")) {
            s.overlayPalette = readPalette(*p, r.child("overlayPalette"));
            if (s.overlayPalette.size() != 9) {
                violate(r.child("overlayPalette"),
                        "overlayPalette has exactly 9 colours (06 section 16 rule 3)");
            }
        }
        if (const detail::json* o = r.optional("overlayStyle")) {
            ObjectReader orr(*o, r.child("overlayStyle"));
            if (const detail::json* v = orr.optional("colourSource")) {
                s.overlayStyle.colourSource =
                    detail::enumString(*v, orr.child("colourSource"), {"channel", "overlayPalette"});
            }
            if (const detail::json* v = orr.optional("dash")) {
                s.overlayStyle.dash =
                    detail::enumString(*v, orr.child("dash"), {"solid", "dash", "dot", "dashDot"});
            }
            if (const detail::json* v = orr.optional("lightness")) {
                s.overlayStyle.lightness = jsonio::asNumber(*v, orr.child("lightness"));
                if (s.overlayStyle.lightness < -1.0 || s.overlayStyle.lightness > 1.0) {
                    violate(orr.child("lightness"), "lightness lies in -1..1");
                }
            }
            if (const detail::json* v = orr.optional("widthScale")) {
                s.overlayStyle.widthScale = jsonio::asNumber(*v, orr.child("widthScale"));
                if (s.overlayStyle.widthScale <= 0.0 || s.overlayStyle.widthScale > 10.0) {
                    violate(orr.child("widthScale"), "widthScale lies in (0, 10]");
                }
            }
            orr.closed();
        }
        if (const detail::json* fontsJson = r.optional("fonts")) {
            ObjectReader fr(*fontsJson, r.child("fonts"));
            for (auto it = fontsJson->begin(); it != fontsJson->end(); ++it) {
                if (!isRoleName(it.key())) {
                    violate(fr.child(it.key()), "font role names are camelCase identifiers");
                }
                s.fonts.emplace(it.key(), readFont(it.value(), fr.child(it.key())));
            }
        }
        if (const detail::json* p = r.optional("print")) {
            s.print = detail::objectDoc(*p, r.child("print"));
        }
        s.unknown = detail::unknownMembers(r);
        return s;
    });
}

Result<Scheme> Scheme::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    return detail::loadDocument<Scheme>(path,
                                        [&](std::string_view text) { return Scheme::parse(text, log); });
}

std::string Scheme::toJson() const {
    return detail::canonicalOrEmpty(schemeToJson(*this));
}

Result<void> Scheme::save(const std::filesystem::path& path) const {
    return detail::saveCanonical(schemeToJson(*this), path);
}

} // namespace esses::project
