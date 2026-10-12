// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "doc_io.hpp"

#include <esses/project/keymap.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <utility>

namespace esses::project {

namespace {

using detail::json;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

// <area>.<verb_noun>: snake_case segments separated by dots, at least two segments.
bool isCommandId(std::string_view id) noexcept {
    return detail::isLowerId(id, '_', 2);
}

template <std::size_t N> bool contains(const std::array<std::string_view, N>& set, std::string_view v) {
    return std::find(set.begin(), set.end(), v) != set.end();
}

std::string readContext(const json& j, const std::string& pointer) {
    const std::string& c = asString(j, pointer);
    if (!contains(kKeymapContexts, c)) {
        violate(pointer, "'" + c +
                             "' is not a command context (global, worksheet, component, graph, summary, "
                             "track, data_window, channels_window, values_window)");
    }
    return c;
}

std::string readCommand(const json& j, const std::string& pointer) {
    const std::string& c = asString(j, pointer);
    if (!isCommandId(c)) {
        violate(pointer, "'" + c + "' is not a command id (<area>.<verb_noun>, snake_case)");
    }
    return c;
}

KeyBinding readBinding(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    KeyBinding b;
    b.command = readCommand(r.required("command"), r.child("command"));
    b.keys = detail::stringArray(r.required("keys"), r.child("keys"));
    for (std::size_t i = 0; i < b.keys.size(); ++i) {
        if (b.keys[i].empty()) {
            violate(jsonio::pointerIndex(r.child("keys"), i), "a key sequence is not empty");
        }
    }
    b.context = readContext(r.required("context"), r.child("context"));
    if (const json* a = r.optional("args")) {
        if (!a->is_array()) {
            violate(r.child("args"), "args is an array");
        }
        b.args = detail::docOrViolate(*a, r.child("args"));
    }
    bool hasStatus = false;
    if (const json* s = r.optional("status")) {
        const std::string st =
            detail::enumString(*s, r.child("status"), {"implemented", "planned", "reserved"});
        b.status = st == "planned" ? KeymapStatus::Planned
                                   : (st == "reserved" ? KeymapStatus::Reserved : KeymapStatus::Implemented);
        hasStatus = true;
    }
    if (const json* m = r.optional("milestone")) {
        b.milestone = asString(*m, r.child("milestone"));
        if (!contains(kKeymapMilestones, b.milestone)) {
            violate(r.child("milestone"),
                    "'" + b.milestone + "' is not a milestone (M1, M2, M3, M4, v1.0, later)");
        }
    }
    if (hasStatus && b.status != KeymapStatus::Reserved && b.milestone.empty()) {
        violate(pointer, "a planned or implemented row names its milestone");
    }
    b.doc = detail::optString(r, "doc");
    r.closed();
    return b;
}

json bindingToJson(const KeyBinding& b) {
    json j = {{"command", b.command}, {"keys", b.keys}, {"context", b.context}};
    if (!b.args.compact().empty()) {
        j["args"] = jsonio::fromDoc(b.args);
    }
    if (b.status != KeymapStatus::Implemented || !b.milestone.empty()) {
        j["status"] = std::string(keymapStatusName(b.status));
    }
    if (!b.milestone.empty()) {
        j["milestone"] = b.milestone;
    }
    if (!b.doc.empty()) {
        j["doc"] = b.doc;
    }
    return j;
}

json keymapToJson(const Keymap& k) {
    json root = detail::newDocument("keymap");
    root["name"] = k.name;
    root["inherits"] = k.inherits ? json(*k.inherits) : json(nullptr);
    json rows = json::array();
    for (const KeyBinding& b : k.bindings) {
        rows.push_back(bindingToJson(b));
    }
    root["bindings"] = std::move(rows);
    json unbind = json::array();
    for (const KeyUnbind& u : k.unbind) {
        unbind.push_back({{"command", u.command}, {"context", u.context}});
    }
    root["unbind"] = std::move(unbind);
    detail::putUnknown(root, k.unknown);
    return root;
}

std::string caseFoldAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

} // namespace

std::string_view keymapStatusName(KeymapStatus s) noexcept {
    switch (s) {
    case KeymapStatus::Implemented:
        return "implemented";
    case KeymapStatus::Planned:
        return "planned";
    case KeymapStatus::Reserved:
        return "reserved";
    }
    return "implemented";
}

std::string normaliseKeySequence(std::string_view keys) {
    // Split at '+' separators; a '+' that is the last character (or follows another separator) is the key.
    std::vector<std::string> parts;
    std::string current;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const char c = keys[i];
        if (c == '+' && !current.empty() && i + 1 < keys.size()) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;
    std::string key;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string folded = caseFoldAscii(parts[i]);
        const bool last = i + 1 == parts.size();
        if (!last && (folded == "ctrl" || folded == "control")) {
            ctrl = true;
        } else if (!last && folded == "alt") {
            alt = true;
        } else if (!last && folded == "shift") {
            shift = true;
        } else if (!last && (folded == "meta" || folded == "cmd" || folded == "win")) {
            meta = true;
        } else {
            key = parts[i];
        }
    }
    if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') {
        key[0] = static_cast<char>(key[0] - 'a' + 'A');
    }
    std::string out;
    out += ctrl ? "Ctrl+" : "";
    out += alt ? "Alt+" : "";
    out += shift ? "Shift+" : "";
    out += meta ? "Meta+" : "";
    out += key;
    return out;
}

Result<Keymap> Keymap::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::parseDocument<Keymap>(json, [&](const detail::json& root) -> Result<Keymap> {
        ObjectReader r(root, "");
        detail::readSchemaHeader(r, "keymap", log);
        Keymap k;
        k.name = asString(r.required("name"), r.child("name"));
        if (const detail::json* inh = r.optional("inherits")) {
            k.inherits = asString(*inh, r.child("inherits"));
        }
        const detail::json& rows = r.required("bindings");
        const std::string rp = r.child("bindings");
        if (!rows.is_array()) {
            violate(rp, "bindings is an array");
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            k.bindings.push_back(readBinding(rows[i], jsonio::pointerIndex(rp, i)));
        }
        if (const detail::json* un = r.optional("unbind")) {
            const std::string up = r.child("unbind");
            if (!un->is_array()) {
                violate(up, "unbind is an array");
            }
            for (std::size_t i = 0; i < un->size(); ++i) {
                ObjectReader ur((*un)[i], jsonio::pointerIndex(up, i));
                KeyUnbind u;
                u.command = readCommand(ur.required("command"), ur.child("command"));
                u.context = readContext(ur.required("context"), ur.child("context"));
                ur.closed();
                k.unbind.push_back(std::move(u));
            }
        }
        k.unknown = detail::unknownMembers(r);
        for (const std::string& c : k.conflicts()) {
            log.push_back(c);
        }
        return k;
    });
}

Result<Keymap> Keymap::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    return detail::loadDocument<Keymap>(path,
                                        [&](std::string_view text) { return Keymap::parse(text, log); });
}

std::string Keymap::toJson() const {
    return detail::canonicalOrEmpty(keymapToJson(*this));
}

Result<void> Keymap::save(const std::filesystem::path& path) const {
    return detail::saveCanonical(keymapToJson(*this), path);
}

Keymap Keymap::merged(const Keymap& base, const Keymap& user) {
    Keymap out;
    out.name = user.name.empty() ? base.name : user.name;
    out.unknown = base.unknown;
    const auto unbound = [&user](const KeyBinding& b) {
        return std::any_of(user.unbind.begin(), user.unbind.end(), [&b](const KeyUnbind& u) {
            return u.command == b.command && u.context == b.context;
        });
    };
    for (const KeyBinding& b : base.bindings) {
        if (!unbound(b)) {
            out.bindings.push_back(b);
        }
    }
    for (const KeyBinding& u : user.bindings) {
        const auto same = std::find_if(out.bindings.begin(), out.bindings.end(), [&u](const KeyBinding& b) {
            return b.command == u.command && b.context == u.context && b.args == u.args;
        });
        if (same != out.bindings.end()) {
            same->keys = u.keys;
        } else {
            out.bindings.push_back(u);
        }
    }
    return out;
}

std::vector<std::string> Keymap::conflicts() const {
    std::vector<std::string> out;
    // (context, case-folded normal form) -> first row; key names compare case-insensitively ("f9" is F9).
    std::map<std::pair<std::string, std::string>, const KeyBinding*> seen;
    for (const KeyBinding& b : bindings) {
        for (const std::string& key : b.keys) {
            const std::string normal = normaliseKeySequence(key);
            const auto [it, inserted] = seen.emplace(std::make_pair(b.context, caseFoldAscii(normal)), &b);
            if (!inserted && it->second != &b) {
                out.push_back("keymap '" + name + "': " + normal + " is bound twice in context " + b.context +
                              " (" + it->second->command + " and " + b.command + ")");
            }
        }
    }
    return out;
}

std::vector<const KeyBinding*> Keymap::rowsFor(std::string_view command) const {
    std::vector<const KeyBinding*> out;
    for (const KeyBinding& b : bindings) {
        if (b.command == command) {
            out.push_back(&b);
        }
    }
    return out;
}

} // namespace esses::project
