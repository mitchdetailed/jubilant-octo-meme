// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Dictionary: dictionary-1 reading, writing and matching (05 section 8.3, 02 section 4.6 rule 15,
// schemas/dictionary-1.schema.json).
#include "text/text_scan.hpp"

#include <esses/base/json_nlohmann.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/dictionary.hpp>

#include <charconv>
#include <cstdio>
#include <memory>
#include <regex>
#include <string>
#include <utility>

namespace esses::formats {
namespace {

using jsonio::asInt32;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;
using json = nlohmann::json;

bool oneOf(std::string_view v, std::initializer_list<std::string_view> values) {
    for (const std::string_view x : values) {
        if (x == v) {
            return true;
        }
    }
    return false;
}

Dictionary::Match parseMatch(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    Dictionary::Match m;
    if (const json* v = r.optional("vendorId")) {
        m.vendorId = asString(*v, r.child("vendorId"));
    }
    if (const json* v = r.optional("name")) {
        m.name = asString(*v, r.child("name"));
    }
    if (const json* v = r.optional("nameRegex")) {
        m.nameRegex = asString(*v, r.child("nameRegex"));
        if (!text::compileRegex(*m.nameRegex)) {
            violate(r.child("nameRegex"),
                    "'" + *m.nameRegex + "' is not a valid ECMAScript regular expression");
        }
        if (text::hasNestedQuantifier(*m.nameRegex)) {
            violate(r.child("nameRegex"),
                    "'" + *m.nameRegex + "' repeats a group that itself repeats (catastrophic backtracking)");
        }
    }
    r.closed();
    if (!m.vendorId && !m.name && !m.nameRegex) {
        violate(pointer, "a match needs vendorId, name or nameRegex");
    }
    return m;
}

json matchJson(const Dictionary::Match& m) {
    json j = json::object();
    if (m.vendorId) {
        j["vendorId"] = *m.vendorId;
    }
    if (m.name) {
        j["name"] = *m.name;
    }
    if (m.nameRegex) {
        j["nameRegex"] = *m.nameRegex;
    }
    return j;
}

std::uint64_t parseMask(const std::string& s, const std::string& pointer) {
    std::string_view t(s);
    int base = 10;
    if (t.starts_with("0x") || t.starts_with("0X")) {
        t.remove_prefix(2);
        base = 16;
    }
    std::uint64_t v = 0;
    const auto r = std::from_chars(t.data(), t.data() + t.size(), v, base);
    if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size() || v == 0) {
        violate(pointer, "mask '" + s + "' is not a non-zero integer (\"0x0001\")");
    }
    return v;
}

std::string maskText(std::uint64_t mask) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%04llX", static_cast<unsigned long long>(mask));
    return buf;
}

Dictionary parseDictionary(const json& j, std::vector<std::string>& log) {
    if (!j.is_object()) {
        violate("", "a dictionary is a JSON object");
    }
    jsonio::checkSchemaId(j, "dictionary", 1);
    ObjectReader r(j, "");
    r.skip("$schema");
    (void)jsonio::asInt(r.required("schemaVersion"), r.child("schemaVersion"));
    Dictionary d;
    d.importer = asString(r.required("importer"), r.child("importer"));
    d.version = asString(r.required("version"), r.child("version"));
    if (const json* v = r.optional("entries")) {
        if (!v->is_array()) {
            violate(r.child("entries"), "expected an array");
        }
        for (std::size_t i = 0; i < v->size(); ++i) {
            const std::string p = jsonio::pointerIndex(r.child("entries"), i);
            ObjectReader e((*v)[i], p);
            Dictionary::Entry entry;
            entry.match = parseMatch(e.required("match"), e.child("match"));
            const auto str = [&](std::string_view key, std::optional<std::string>& out) {
                if (const json* x = e.optional(key)) {
                    out = asString(*x, e.child(key));
                }
            };
            str("name", entry.name);
            str("shortName", entry.shortName);
            str("unit", entry.unit);
            str("quantity", entry.quantity);
            str("kind", entry.kind);
            str("interp", entry.interp);
            str("dtype", entry.dtype);
            if (entry.kind && !oneOf(*entry.kind, {"Real", "Integer", "Enum", "Bool", "Bitfield", "Text"})) {
                violate(e.child("kind"), "unknown kind '" + *entry.kind + "'");
            }
            if (entry.interp && !oneOf(*entry.interp, {"Linear", "Closest", "Previous", "Next"})) {
                violate(e.child("interp"), "unknown interp '" + *entry.interp + "'");
            }
            if (entry.dtype && !oneOf(*entry.dtype, {"F32", "F64", "I32"})) {
                violate(e.child("dtype"), "dtype must be F32, F64 or I32");
            }
            if (const json* x = e.optional("lapSource")) {
                auto doc = jsonio::toDoc(*x);
                if (!doc) {
                    violate(e.child("lapSource"), doc.error().message);
                }
                entry.lapSource = std::move(doc).value();
            }
            e.closed();
            d.entries.push_back(std::move(entry));
        }
    }
    if (const json* v = r.optional("bitfields")) {
        if (!v->is_array()) {
            violate(r.child("bitfields"), "expected an array");
        }
        for (std::size_t i = 0; i < v->size(); ++i) {
            const std::string p = jsonio::pointerIndex(r.child("bitfields"), i);
            ObjectReader b((*v)[i], p);
            Dictionary::Bitfield bf;
            bf.match = parseMatch(b.required("match"), b.child("match"));
            if (const json* x = b.optional("device")) {
                ObjectReader dv(*x, b.child("device"));
                Dictionary::Device dev;
                dev.type = asString(dv.required("type"), dv.child("type"));
                if (const json* y = dv.optional("minVersion")) {
                    dev.minVersion = asInt32(*y, dv.child("minVersion"));
                }
                if (const json* y = dv.optional("maxVersion")) {
                    dev.maxVersion = asInt32(*y, dv.child("maxVersion"));
                }
                dv.closed();
                bf.device = std::move(dev);
            }
            const json& bits = b.required("bits");
            if (!bits.is_array() || bits.empty()) {
                violate(b.child("bits"), "expected a non-empty array of {mask, name}");
            }
            for (std::size_t k = 0; k < bits.size(); ++k) {
                const std::string pk = jsonio::pointerIndex(b.child("bits"), k);
                ObjectReader br(bits[k], pk);
                Dictionary::Bit bit;
                bit.mask = parseMask(asString(br.required("mask"), br.child("mask")), br.child("mask"));
                bit.name = asString(br.required("name"), br.child("name"));
                br.closed();
                bf.bits.push_back(std::move(bit));
            }
            b.closed();
            d.bitfields.push_back(std::move(bf));
        }
    }
    json unknown = r.unread();
    if (!unknown.empty()) {
        for (const auto& [key, value] : unknown.items()) {
            log.push_back("dictionary '" + d.importer + "': unknown member '" + key + "' kept");
        }
        if (auto doc = jsonio::toDoc(unknown)) {
            d.unknown = std::move(doc).value();
        }
    }
    return d;
}

bool matches(const Dictionary::Match& m, std::string_view vendorId, std::string_view name) {
    if (m.vendorId && *m.vendorId != vendorId) {
        return false;
    }
    if (m.name && *m.name != name) {
        return false;
    }
    if (m.nameRegex) {
        // Compiled once per pattern, not once per channel and entry (D-190).
        const std::shared_ptr<const std::regex> re = text::cachedRegex(*m.nameRegex);
        if (re == nullptr || !text::regexSearch(*re, name)) {
            return false;
        }
    }
    return true;
}

} // namespace

Result<Dictionary> Dictionary::parse(std::string_view text, std::vector<std::string>& log) {
    auto j = jsonio::parseJson(text);
    if (!j) {
        return Failure{j.error()};
    }
    return jsonio::guardJson([&]() -> Result<Dictionary> { return parseDictionary(*j, log); });
}

Result<Dictionary> Dictionary::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    auto text = jsonio::readFile(path);
    if (!text) {
        return Failure{text.error()};
    }
    auto d = parse(*text, log);
    if (!d) {
        Error e = d.error();
        e.context.path = path;
        e.message = pathToUtf8(path) + ": " + e.message;
        return Failure{std::move(e)};
    }
    return d;
}

Result<std::string> Dictionary::toJson() const {
    json j = json::object();
    if (!unknown.isNull()) {
        j = jsonio::fromDoc(unknown);
        if (!j.is_object()) {
            j = json::object();
        }
    }
    j["$schema"] = std::string(kSchemaId);
    j["schemaVersion"] = 1;
    j["importer"] = importer;
    j["version"] = version;
    json es = json::array();
    for (const Entry& e : entries) {
        json x = json::object();
        x["match"] = matchJson(e.match);
        const auto put = [&](const char* key, const std::optional<std::string>& v) {
            if (v) {
                x[key] = *v;
            }
        };
        put("name", e.name);
        put("shortName", e.shortName);
        put("unit", e.unit);
        put("quantity", e.quantity);
        put("kind", e.kind);
        put("interp", e.interp);
        put("dtype", e.dtype);
        if (!e.lapSource.isNull()) {
            x["lapSource"] = jsonio::fromDoc(e.lapSource);
        }
        es.push_back(std::move(x));
    }
    j["entries"] = std::move(es);
    json bs = json::array();
    for (const Bitfield& b : bitfields) {
        json x = json::object();
        x["match"] = matchJson(b.match);
        if (b.device) {
            json dv = json::object();
            dv["type"] = b.device->type;
            if (b.device->minVersion) {
                dv["minVersion"] = *b.device->minVersion;
            }
            if (b.device->maxVersion) {
                dv["maxVersion"] = *b.device->maxVersion;
            }
            x["device"] = std::move(dv);
        }
        json bits = json::array();
        for (const Bit& bit : b.bits) {
            bits.push_back(json{{"mask", maskText(bit.mask)}, {"name", bit.name}});
        }
        x["bits"] = std::move(bits);
        bs.push_back(std::move(x));
    }
    j["bitfields"] = std::move(bs);
    return jsonio::canonicalJson(j);
}

const Dictionary::Entry* Dictionary::match(std::string_view vendorId, std::string_view name) const {
    // Pass 1: entries keyed by vendorId; pass 2: by exact name; pass 3: by nameRegex only. Every member an
    // entry's match states must hold.
    if (!vendorId.empty()) {
        for (const Entry& e : entries) {
            if (e.match.vendorId && matches(e.match, vendorId, name)) {
                return &e;
            }
        }
    }
    for (const Entry& e : entries) {
        if (!e.match.vendorId && e.match.name && matches(e.match, vendorId, name)) {
            return &e;
        }
    }
    for (const Entry& e : entries) {
        if (!e.match.vendorId && !e.match.name && e.match.nameRegex && matches(e.match, vendorId, name)) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace esses::formats
