// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TextProfile: importer-profile-1 reading and writing (05 section 7.2,
// schemas/importer-profile-1.schema.json). The reader is lenient where the schema is strict about shape only
// (unknown top-level members are kept in `unknown`), and strict about meaning: a regular expression that does
// not compile, an unknown enumeration value or an out-of-range number is Invalid with the JSON pointer in
// context.detail.
#include "text/text_scan.hpp"

#include <esses/base/json_nlohmann.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/text_profile.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace esses::formats {
namespace {

using jsonio::asBool;
using jsonio::asInt32;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;
using json = nlohmann::json;

constexpr std::string_view kSchemaName = "importer-profile";
void checkRegex(const std::string& pattern, const std::string& pointer) {
    if (!text::compileRegex(pattern)) {
        violate(pointer, "'" + pattern + "' is not a valid ECMAScript regular expression");
    }
    if (text::hasNestedQuantifier(pattern)) {
        violate(pointer, "'" + pattern + "' repeats a group that itself repeats (catastrophic backtracking)");
    }
}

std::vector<std::string> stringArray(const json& j, const std::string& pointer) {
    if (!j.is_array()) {
        violate(pointer, "expected an array of strings");
    }
    std::vector<std::string> out;
    out.reserve(j.size());
    for (std::size_t i = 0; i < j.size(); ++i) {
        out.push_back(asString(j[i], jsonio::pointerIndex(pointer, i)));
    }
    return out;
}

TextProfile::Encoding parseEncoding(const std::string& s, const std::string& pointer) {
    if (s == "auto") {
        return TextProfile::Encoding::Auto;
    }
    if (s == "utf-8") {
        return TextProfile::Encoding::Utf8;
    }
    if (s == "utf-16le") {
        return TextProfile::Encoding::Utf16Le;
    }
    if (s == "utf-16be") {
        return TextProfile::Encoding::Utf16Be;
    }
    if (s == "cp1252") {
        return TextProfile::Encoding::Cp1252;
    }
    violate(pointer, "unknown encoding '" + s + "' (auto, utf-8, utf-16le, utf-16be, cp1252)");
}

std::string_view encodingName(TextProfile::Encoding e) {
    switch (e) {
    case TextProfile::Encoding::Auto:
        return "auto";
    case TextProfile::Encoding::Utf8:
        return "utf-8";
    case TextProfile::Encoding::Utf16Le:
        return "utf-16le";
    case TextProfile::Encoding::Utf16Be:
        return "utf-16be";
    case TextProfile::Encoding::Cp1252:
        break;
    }
    return "cp1252";
}

bool oneOf(std::string_view v, std::initializer_list<std::string_view> values) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

TextProfile::LapSource parseLapSource(const json& j, const std::string& pointer, int depth) {
    if (depth > 4) {
        violate(pointer, "lapSource fallbacks nest deeper than 4 levels");
    }
    ObjectReader r(j, pointer);
    TextProfile::LapSource s;
    s.kind = asString(r.required("kind"), r.child("kind"));
    if (!oneOf(s.kind, {"none", "lap_counter_edge", "beacon_event", "lap_table"})) {
        violate(r.child("kind"), "unknown lapSource kind '" + s.kind + "'");
    }
    if (const json* v = r.optional("column")) {
        s.column = asString(*v, r.child("column"));
        if (s.column.starts_with("@auto:")) {
            checkRegex(s.column.substr(6), r.child("column"));
        }
    }
    if (const json* v = r.optional("section")) {
        s.section = asString(*v, r.child("section"));
    }
    if (const json* v = r.optional("detailsKey")) {
        s.detailsKey = asString(*v, r.child("detailsKey"));
    }
    if (const json* v = r.optional("match")) {
        ObjectReader m(*v, r.child("match"));
        if (!v->is_object()) {
            violate(r.child("match"), "expected an object");
        }
        for (const auto& [key, value] : v->items()) {
            const std::string p = m.child(key);
            s.match[key] = asString(value, p);
            checkRegex(s.match[key], p);
        }
    }
    if (const json* v = r.optional("optional")) {
        s.optional = asBool(*v, r.child("optional"));
    }
    if (const json* v = r.optional("fallback")) {
        s.fallback = std::make_shared<const TextProfile::LapSource>(
            parseLapSource(*v, r.child("fallback"), depth + 1));
    }
    r.closed();
    return s;
}

json lapSourceJson(const TextProfile::LapSource& s) {
    json j = json::object();
    j["kind"] = s.kind;
    if (!s.column.empty()) {
        j["column"] = s.column;
    }
    if (!s.section.empty()) {
        j["section"] = s.section;
    }
    if (!s.detailsKey.empty()) {
        j["detailsKey"] = s.detailsKey;
    }
    if (!s.match.empty()) {
        json m = json::object();
        for (const auto& [k, v] : s.match) {
            m[k] = v;
        }
        j["match"] = std::move(m);
    }
    if (s.optional) {
        j["optional"] = true;
    }
    if (s.fallback) {
        j["fallback"] = lapSourceJson(*s.fallback);
    }
    return j;
}

TextProfile::Column parseColumn(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    TextProfile::Column c;
    const auto str = [&](std::string_view key, std::string& out) {
        if (const json* v = r.optional(key)) {
            out = asString(*v, r.child(key));
        }
    };
    str("name", c.name);
    str("quantity", c.quantity);
    str("unit", c.unit);
    str("dtype", c.dtype);
    str("kind", c.kind);
    str("interp", c.interp);
    if (!c.dtype.empty() && !oneOf(c.dtype, {"F32", "F64", "I32"})) {
        violate(r.child("dtype"), "dtype must be F32, F64 or I32");
    }
    if (!c.kind.empty() && !oneOf(c.kind, {"Real", "Integer", "Enum", "Text"})) {
        violate(r.child("kind"), "kind must be Real, Integer, Enum or Text");
    }
    if (!c.interp.empty() && !oneOf(c.interp, {"Linear", "Closest", "Previous", "Next"})) {
        violate(r.child("interp"), "interp must be Linear, Closest, Previous or Next");
    }
    if (const json* v = r.optional("dps")) {
        const int dps = asInt32(*v, r.child("dps"));
        if (dps < 0 || dps > 15) {
            violate(r.child("dps"), "dps must be 0..15");
        }
        c.dps = dps;
    }
    if (const json* v = r.optional("sentinels")) {
        c.sentinels = stringArray(*v, r.child("sentinels"));
    }
    r.closed();
    return c;
}

json columnJson(const TextProfile::Column& c) {
    json j = json::object();
    const auto put = [&](const char* key, const std::string& v) {
        if (!v.empty()) {
            j[key] = v;
        }
    };
    put("name", c.name);
    put("quantity", c.quantity);
    put("unit", c.unit);
    put("dtype", c.dtype);
    put("kind", c.kind);
    put("interp", c.interp);
    if (c.dps) {
        j["dps"] = *c.dps;
    }
    if (!c.sentinels.empty()) {
        j["sentinels"] = c.sentinels;
    }
    return j;
}

TextProfile parseProfile(const json& j, std::vector<std::string>& log) {
    if (!j.is_object()) {
        violate("", "an importer profile is a JSON object");
    }
    jsonio::checkSchemaId(j, kSchemaName, 1);
    ObjectReader r(j, "");
    r.skip("$schema");
    TextProfile p;
    (void)jsonio::asInt(r.required("schemaVersion"), r.child("schemaVersion"));
    p.id = asString(r.required("id"), r.child("id"));
    if (p.id.empty() || p.id.find_first_of(":/\\") != std::string::npos) {
        violate(r.child("id"), "the profile id must be a non-empty file stem without ':', '/' or '\\'");
    }
    if (const json* v = r.optional("importerId")) {
        p.importerId = asString(*v, r.child("importerId"));
    }
    p.name = asString(r.required("name"), r.child("name"));
    p.version = asString(r.required("version"), r.child("version"));

    if (const json* v = r.optional("sniff")) {
        ObjectReader s(*v, r.child("sniff"));
        if (const json* x = s.optional("preambleRegex")) {
            p.sniff.preambleRegex = asString(*x, s.child("preambleRegex"));
            checkRegex(p.sniff.preambleRegex, s.child("preambleRegex"));
        }
        if (const json* x = s.optional("extensions")) {
            p.sniff.extensions = stringArray(*x, s.child("extensions"));
            for (std::string& e : p.sniff.extensions) {
                std::transform(e.begin(), e.end(), e.begin(), [](char c) {
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                });
            }
        }
        if (const json* x = s.optional("confidence")) {
            p.sniff.confidence = asInt32(*x, s.child("confidence"));
            if (p.sniff.confidence < 0 || p.sniff.confidence > 100) {
                violate(s.child("confidence"), "confidence must be 0..100");
            }
        }
        s.closed();
    }
    if (const json* v = r.optional("encoding")) {
        p.encoding = parseEncoding(asString(*v, r.child("encoding")), r.child("encoding"));
    }
    if (const json* v = r.optional("delimiter")) {
        p.delimiter = asString(*v, r.child("delimiter"));
        if (!oneOf(p.delimiter, {",", ";", "\t", "|", " ", "auto"})) {
            violate(r.child("delimiter"), "delimiter must be one of , ; tab | space auto");
        }
    }
    if (const json* v = r.optional("decimal")) {
        p.decimal = asString(*v, r.child("decimal"));
        if (!oneOf(p.decimal, {".", ",", "auto"})) {
            violate(r.child("decimal"), "decimal must be '.', ',' or auto");
        }
    }
    if (const json* v = r.optional("quote")) { // null: no quoting
        const std::string& q = asString(*v, r.child("quote"));
        if (q.size() != 1 || static_cast<unsigned char>(q[0]) >= 0x80) {
            violate(r.child("quote"), "quote must be one ASCII character or null");
        }
        p.quote = q[0];
    } else if (j.contains("quote")) {
        p.quote.reset();
    }
    if (const json* v = r.optional("trim")) {
        p.trim = asBool(*v, r.child("trim"));
    }
    if (const json* v = r.optional("trailingDelimiter")) {
        p.trailingDelimiter = asBool(*v, r.child("trailingDelimiter"));
    }
    if (const json* v = r.optional("layout")) {
        const std::string& l = asString(*v, r.child("layout"));
        if (l == "table") {
            p.layout = TextProfile::Layout::Table;
        } else if (l == "blocks") {
            p.layout = TextProfile::Layout::Blocks;
        } else {
            violate(r.child("layout"), "layout must be table or blocks");
        }
    }
    if (const json* v = r.optional("preamble")) {
        ObjectReader a(*v, r.child("preamble"));
        if (const json* x = a.optional("lines")) {
            p.preamble.lines = asInt32(*x, a.child("lines"));
            if (p.preamble.lines < 0) {
                violate(a.child("lines"), "lines must be >= 0");
            }
        }
        if (const json* x = a.optional("untilRegex")) {
            p.preamble.untilRegex = asString(*x, a.child("untilRegex"));
            if (p.preamble.untilRegex != "@auto") {
                checkRegex(p.preamble.untilRegex, a.child("untilRegex"));
            }
        }
        if (const json* x = a.optional("untilBlank")) {
            p.preamble.untilBlank = asBool(*x, a.child("untilBlank"));
        }
        if (const json* x = a.optional("keyValueColumns")) {
            if (!x->is_array()) {
                violate(a.child("keyValueColumns"),
                        "expected an array of [key, value, unit?] column triples");
            }
            for (std::size_t i = 0; i < x->size(); ++i) {
                const std::string pi = jsonio::pointerIndex(a.child("keyValueColumns"), i);
                const json& t = (*x)[i];
                if (!t.is_array() || t.size() < 2 || t.size() > 3) {
                    violate(pi, "expected [key, value] or [key, value, unit] column indexes");
                }
                std::array<int, 3> kv{-1, -1, -1};
                for (std::size_t k = 0; k < t.size(); ++k) {
                    kv[k] = asInt32(t[k], jsonio::pointerIndex(pi, k));
                    if (kv[k] < 0 || kv[k] > 1000) {
                        violate(jsonio::pointerIndex(pi, k), "column index must be 0..1000");
                    }
                }
                p.preamble.keyValueColumns.push_back(kv);
            }
        }
        if (const json* x = a.optional("capture")) {
            if (!x->is_object()) {
                violate(a.child("capture"), "expected an object");
            }
            ObjectReader c(*x, a.child("capture"));
            for (const auto& [key, value] : x->items()) {
                const std::string& target = asString(value, c.child(key));
                // With keyValueColumns the map is <file key> -> <detail id | @input>; otherwise it is
                // <detail id | @input> -> <regex with one group> (05 section 7.3).
                if (p.preamble.keyValueColumns.empty()) {
                    checkRegex(target, c.child(key));
                }
                p.preamble.capture.emplace_back(key, target);
            }
        }
        if (const json* x = a.optional("unmatchedTo")) {
            p.preamble.unmatchedTo = asString(*x, a.child("unmatchedTo"));
        }
        a.closed();
    }
    if (const json* v = r.optional("header")) {
        ObjectReader h(*v, r.child("header"));
        if (const json* x = h.optional("skipBlank")) {
            p.header.skipBlank = asBool(*x, h.child("skipBlank"));
        }
        if (const json* x = h.optional("namesRow")) {
            p.header.namesRow = asInt32(*x, h.child("namesRow"));
            if (p.header.namesRow < 0) {
                violate(h.child("namesRow"), "namesRow must be >= 0");
            }
        }
        if (v->contains("unitsRow")) {
            const json& x = (*v)["unitsRow"];
            h.skip("unitsRow");
            if (x.is_null()) {
                p.header.unitsRow = "null";
            } else if (x.is_string() && x.get<std::string>() == "@auto") {
                p.header.unitsRow = "@auto";
            } else {
                const int u = asInt32(x, h.child("unitsRow"));
                if (u < 0) {
                    violate(h.child("unitsRow"), "unitsRow must be >= 0, null or \"@auto\"");
                }
                p.header.unitsRow = std::to_string(u);
            }
        }
        if (v->contains("dataRow")) {
            const json& x = (*v)["dataRow"];
            h.skip("dataRow");
            if (x.is_string() && x.get<std::string>() == "auto") {
                p.header.dataRow = "auto";
            } else {
                const int d = asInt32(x, h.child("dataRow"));
                if (d < 0) {
                    violate(h.child("dataRow"), "dataRow must be >= 0 or \"auto\"");
                }
                p.header.dataRow = std::to_string(d);
            }
        }
        if (v->contains("unitInName")) {
            const json& x = (*v)["unitInName"];
            h.skip("unitInName");
            if (!x.is_null()) {
                p.header.unitInName = asString(x, h.child("unitInName"));
                checkRegex(p.header.unitInName, h.child("unitInName"));
            }
        }
        if (const json* x = h.optional("ignoreEmptyNames")) {
            p.header.ignoreEmptyNames = asBool(*x, h.child("ignoreEmptyNames"));
        }
        h.closed();
    }
    if (const json* v = r.optional("time")) {
        ObjectReader t(*v, r.child("time"));
        if (const json* x = t.optional("column")) {
            p.time.column = asString(*x, t.child("column"));
        }
        if (const json* x = t.optional("unit")) {
            p.time.unit = asString(*x, t.child("unit"));
            if (!oneOf(p.time.unit, {"s", "ms", "us", "hhmmss", "epoch_s", "epoch_ms", "@auto"})) {
                violate(t.child("unit"), "unit must be s, ms, us, hhmmss, epoch_s, epoch_ms or @auto");
            }
        }
        if (const json* x = t.optional("rate")) {
            if (x->is_object()) {
                ObjectReader q(*x, t.child("rate"));
                const auto num = jsonio::asU32(q.required("num"), q.child("num"));
                const auto den = jsonio::asU32(q.required("den"), q.child("den"));
                q.closed();
                if (num == 0 || den == 0) {
                    violate(t.child("rate"), "a fixed rate needs num >= 1 and den >= 1");
                }
                p.time.fixedRate = Rate{num, den};
                p.time.rate.clear();
            } else {
                p.time.rate = asString(*x, t.child("rate"));
                if (p.time.rate != "infer" && !p.time.rate.starts_with("fromDetail:")) {
                    violate(t.child("rate"), R"(rate must be "infer", "fromDetail:<input>" or {num, den})");
                }
            }
        }
        if (const json* x = t.optional("origin")) {
            p.time.origin = asString(*x, t.child("origin"));
            if (!oneOf(p.time.origin, {"first", "zero"})) {
                violate(t.child("origin"), "origin must be first or zero");
            }
        }
        t.closed();
    }
    if (const json* v = r.optional("sentinels")) {
        p.sentinels = stringArray(*v, r.child("sentinels"));
    }
    if (const json* v = r.optional("sparse")) {
        const std::string& s = asString(*v, r.child("sparse"));
        if (s == "timestamped") {
            p.sparse = TextProfile::Sparse::Timestamped;
        } else if (s == "hold") {
            p.sparse = TextProfile::Sparse::Hold;
        } else {
            violate(r.child("sparse"), "sparse must be timestamped or hold");
        }
    }
    if (const json* v = r.optional("duplicateNames")) {
        const std::string& s = asString(*v, r.child("duplicateNames"));
        if (s == "suffix") {
            p.duplicateNames = TextProfile::DuplicateNames::Suffix;
        } else if (s == "error") {
            p.duplicateNames = TextProfile::DuplicateNames::Error;
        } else {
            violate(r.child("duplicateNames"), "duplicateNames must be suffix or error");
        }
    }
    if (const json* v = r.optional("columns")) {
        if (!v->is_object()) {
            violate(r.child("columns"), "expected an object of column overrides");
        }
        ObjectReader c(*v, r.child("columns"));
        for (const auto& [key, value] : v->items()) {
            p.columns[key] = parseColumn(value, c.child(key));
        }
    }
    if (const json* v = r.optional("lapSource")) {
        p.lapSource = parseLapSource(*v, r.child("lapSource"), 0);
    }
    if (const json* v = r.optional("blocks")) {
        auto doc = jsonio::toDoc(*v);
        if (!doc) {
            violate(r.child("blocks"), doc.error().message);
        }
        p.blocks = std::move(doc).value();
    }
    json unknown = r.unread();
    if (!unknown.empty()) {
        for (const auto& [key, value] : unknown.items()) {
            log.push_back("importer profile '" + p.id + "': unknown member '" + key + "' kept");
        }
        auto doc = jsonio::toDoc(unknown);
        if (doc) {
            p.unknown = std::move(doc).value();
        }
    }
    return p;
}

} // namespace

Result<TextProfile> TextProfile::parse(std::string_view text, std::vector<std::string>& log) {
    auto j = jsonio::parseJson(text);
    if (!j) {
        return Failure{j.error()};
    }
    return jsonio::guardJson([&]() -> Result<TextProfile> { return parseProfile(*j, log); });
}

Result<TextProfile> TextProfile::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    auto text = jsonio::readFile(path);
    if (!text) {
        return Failure{text.error()};
    }
    auto p = parse(*text, log);
    if (!p) {
        Error e = p.error();
        e.context.path = path;
        e.message = pathToUtf8(path) + ": " + e.message;
        return Failure{std::move(e)};
    }
    return p;
}

Result<std::string> TextProfile::toJson() const {
    json j = json::object();
    if (!unknown.isNull()) {
        j = jsonio::fromDoc(unknown);
        if (!j.is_object()) {
            j = json::object();
        }
    }
    j["$schema"] = std::string(kSchemaId);
    j["schemaVersion"] = 1;
    j["id"] = id;
    if (!importerId.empty()) {
        j["importerId"] = importerId;
    }
    j["name"] = name;
    j["version"] = version;
    json s = json::object();
    if (!sniff.preambleRegex.empty()) {
        s["preambleRegex"] = sniff.preambleRegex;
    }
    s["extensions"] = sniff.extensions;
    s["confidence"] = sniff.confidence;
    j["sniff"] = std::move(s);
    j["encoding"] = std::string(encodingName(encoding));
    j["delimiter"] = delimiter;
    j["decimal"] = decimal;
    j["quote"] = quote ? json(std::string(1, *quote)) : json(nullptr);
    j["trim"] = trim;
    if (trailingDelimiter) {
        j["trailingDelimiter"] = *trailingDelimiter;
    }
    j["layout"] = layout == Layout::Table ? "table" : "blocks";
    json a = json::object();
    if (!preamble.untilRegex.empty()) {
        a["untilRegex"] = preamble.untilRegex;
    } else if (preamble.untilBlank) {
        a["untilBlank"] = true;
    } else {
        a["lines"] = preamble.lines;
    }
    if (!preamble.capture.empty()) {
        json c = json::object();
        for (const auto& [k, v] : preamble.capture) {
            c[k] = v;
        }
        a["capture"] = std::move(c);
    }
    if (!preamble.keyValueColumns.empty()) {
        json kv = json::array();
        for (const auto& t : preamble.keyValueColumns) {
            json row = json::array({t[0], t[1]});
            if (t[2] >= 0) {
                row.push_back(t[2]);
            }
            kv.push_back(std::move(row));
        }
        a["keyValueColumns"] = std::move(kv);
    }
    if (!preamble.unmatchedTo.empty()) {
        a["unmatchedTo"] = preamble.unmatchedTo;
    }
    j["preamble"] = std::move(a);
    json h = json::object();
    h["skipBlank"] = header.skipBlank;
    h["namesRow"] = header.namesRow;
    if (header.unitsRow == "null" || header.unitsRow.empty()) {
        h["unitsRow"] = nullptr;
    } else if (header.unitsRow == "@auto") {
        h["unitsRow"] = "@auto";
    } else {
        h["unitsRow"] = std::stoi(header.unitsRow);
    }
    if (header.dataRow == "auto" || header.dataRow.empty()) {
        h["dataRow"] = "auto";
    } else {
        h["dataRow"] = std::stoi(header.dataRow);
    }
    h["unitInName"] = header.unitInName.empty() ? json(nullptr) : json(header.unitInName);
    h["ignoreEmptyNames"] = header.ignoreEmptyNames;
    j["header"] = std::move(h);
    json t = json::object();
    t["column"] = time.column;
    t["unit"] = time.unit;
    if (time.fixedRate) {
        t["rate"] = json{{"num", time.fixedRate->num}, {"den", time.fixedRate->den}};
    } else {
        t["rate"] = time.rate.empty() ? std::string("infer") : time.rate;
    }
    if (!time.origin.empty()) {
        t["origin"] = time.origin;
    }
    j["time"] = std::move(t);
    j["sentinels"] = sentinels;
    j["sparse"] = sparse == Sparse::Timestamped ? "timestamped" : "hold";
    j["duplicateNames"] = duplicateNames == DuplicateNames::Suffix ? "suffix" : "error";
    json cols = json::object();
    for (const auto& [k, c] : columns) {
        cols[k] = columnJson(c);
    }
    j["columns"] = std::move(cols);
    j["lapSource"] = lapSourceJson(lapSource);
    if (!blocks.isNull()) {
        j["blocks"] = jsonio::fromDoc(blocks);
    }
    return jsonio::canonicalJson(j);
}

Hash128 TextProfile::contentHash() const {
    auto text = toJson();
    if (!text) {
        return hash128({"importer-profile", id, version});
    }
    return hash128(std::string_view(*text));
}

std::string TextProfile::effectiveImporterId() const {
    return importerId.empty() ? std::string(kImporterIdPrefix) + id : importerId;
}

} // namespace esses::formats
