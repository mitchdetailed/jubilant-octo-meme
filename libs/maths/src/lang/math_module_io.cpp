// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathModuleIo (03 sections 1 and 4.1, 02 sections 6.5-6.6, D-010, D-019, D-072): maths-1 documents <->
// MathModule, through esses_base's nlohmann-json bridge (typed readers, the one canonical writer).
//
// Reading: every number is {v, unit} through the UnitRegistry (an unknown unit spelling is Invalid at its
// JSON pointer), `rate` is "auto", {num, den} or a plain Hz number, quantities are registry names or aliases,
// unknown top-level members go to MathModule::unknown, and the maths-1 rules the schema cannot express
// (duplicate enumeration values) are checked here. Item members are closed (imported extras live in x-i2).
//
// Writing (the written form): "$schema" and "schemaVersion", the module members, and per item its kind's
// core members always (expression: expr, rate, quantity, resultUnit, display, interp; filter: window,
// display; scale_offset: scale, offset; table2d/table3d: the axes, values, interpolation, outOfRange;
// builtin/plugin: module, version) plus every other member whose value differs from its default, so nothing
// a document says is lost and save(load(save(load(f)))) == save(load(f)) (02 section 6.5 rule 3). Units are
// written as the registry's symbols, quantities by their registry names.

#include <esses/base/atomic_file.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/base/result.hpp>
#include <esses/maths/math_module_io.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace esses::maths {

namespace {

using jsonio::ObjectReader;
using jsonio::violate;
using nlohmann::json;

constexpr std::string_view kDocName = "maths";
constexpr int kDocMajor = 1;

// ---- reading
// ---------------------------------------------------------------------------------------------------

units::UnitId readUnit(const json& j, const std::string& pointer, const units::UnitRegistry& units) {
    const std::string& s = jsonio::asString(j, pointer);
    if (s.empty()) {
        return units::kNoUnit;
    }
    const std::optional<units::UnitId> u = units.find(s);
    if (!u) {
        violate(pointer, "unknown unit '" + s + "'");
    }
    return *u;
}

UnitValue readUnitValue(const json& j, const std::string& pointer, const units::UnitRegistry& units) {
    ObjectReader r(j, pointer);
    UnitValue v;
    v.v = jsonio::asNumber(r.required("v"), r.child("v"));
    v.unit = readUnit(r.required("unit"), r.child("unit"), units);
    r.closed();
    return v;
}

RateSpec readRate(const json& j, const std::string& pointer) {
    RateSpec spec;
    if (j.is_string()) {
        if (j.get<std::string>() != "auto") {
            violate(pointer, "a rate is \"auto\", {num, den} or a number of hertz");
        }
        return spec;
    }
    spec.automatic = false;
    if (j.is_object()) {
        ObjectReader r(j, pointer);
        spec.rate.num = jsonio::asU32(r.required("num"), r.child("num"));
        spec.rate.den = jsonio::asU32(r.required("den"), r.child("den"));
        r.closed();
        if (spec.rate.num == 0 || spec.rate.den == 0) {
            violate(pointer, "num and den of a rate are at least 1");
        }
        return spec;
    }
    const double hz = jsonio::asNumber(j, pointer);
    if (!(hz > 0.0) || hz > 1e9) {
        violate(pointer, "a rate in hertz is above 0");
    }
    if (std::trunc(hz) == hz) {
        spec.rate = Rate{static_cast<std::uint32_t>(hz), 1};
    } else if (std::trunc(hz * 1000.0) == hz * 1000.0) {
        spec.rate = Rate{static_cast<std::uint32_t>(hz * 1000.0), 1000}.normalised(); // 12.5 -> {25, 2}
    } else {
        violate(pointer, "a fractional rate is written {num, den}");
    }
    return spec;
}

DisplayDefaults readDisplay(const json& j, const std::string& pointer, const units::UnitRegistry& units) {
    ObjectReader r(j, pointer);
    DisplayDefaults d;
    if (const json* u = r.optional("unit")) {
        d.unit = readUnit(*u, r.child("unit"), units);
    }
    if (const json* v = r.optional("dps")) {
        d.dps = jsonio::asInt32(*v, r.child("dps"));
    }
    if (const json* v = r.optional("colourIndex")) {
        d.colourIndex = jsonio::asInt32(*v, r.child("colourIndex"));
    }
    r.closed();
    return d;
}

TableSpec::Axis readAxis(const json& j, const std::string& pointer, const units::UnitRegistry& units) {
    ObjectReader r(j, pointer);
    TableSpec::Axis a;
    a.expr = jsonio::asString(r.required("expr"), r.child("expr"));
    a.unit = readUnit(r.required("unit"), r.child("unit"), units);
    const json& bp = r.required("breakpoints");
    const std::string bpp = r.child("breakpoints");
    if (!bp.is_array()) {
        violate(bpp, "breakpoints are an array of numbers");
    }
    for (std::size_t i = 0; i < bp.size(); ++i) {
        a.breakpoints.push_back(jsonio::asNumber(bp[i], jsonio::pointerIndex(bpp, i)));
    }
    r.closed();
    return a;
}

void readValues(const json& j, const std::string& pointer, const units::UnitRegistry& units, TableSpec& t) {
    ObjectReader r(j, pointer);
    t.valuesUnit = readUnit(r.required("unit"), r.child("unit"), units);
    const json& data = r.required("data");
    const std::string dp = r.child("data");
    if (!data.is_array()) {
        violate(dp, "values.data is an array");
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        const std::string ip = jsonio::pointerIndex(dp, i);
        if (data[i].is_array()) {
            for (std::size_t k = 0; k < data[i].size(); ++k) {
                t.values.push_back(jsonio::asNumber(data[i][k], jsonio::pointerIndex(ip, k)));
            }
        } else {
            t.values.push_back(jsonio::asNumber(data[i], ip));
        }
    }
    if (const json* rm = r.optional("rowMajor")) {
        t.rowMajor = jsonio::asBool(*rm, r.child("rowMajor"));
    }
    r.closed();
}

struct KindName {
    std::string_view name;
    ItemKind kind;
};
constexpr std::array<KindName, 8> kKinds{{{"expression", ItemKind::Expression},
                                          {"filter", ItemKind::Filter},
                                          {"scale_offset", ItemKind::ScaleOffset},
                                          {"table2d", ItemKind::Table2D},
                                          {"table3d", ItemKind::Table3D},
                                          {"builtin", ItemKind::Builtin},
                                          {"plugin", ItemKind::Plugin},
                                          {"regression", ItemKind::Regression}}};

ItemKind readKind(const json& j, const std::string& pointer) {
    const std::string& k = jsonio::asString(j, pointer);
    for (const KindName& entry : kKinds) {
        if (k == entry.name) {
            return entry.kind;
        }
    }
    violate(pointer, "unknown item kind '" + k + "'");
}

std::string_view kindName(ItemKind k) {
    for (const KindName& entry : kKinds) {
        if (entry.kind == k) {
            return entry.name;
        }
    }
    return "expression";
}

store::Interp readInterp(const json& j, const std::string& pointer) {
    const std::string& s = jsonio::asString(j, pointer);
    for (const store::Interp i :
         {store::Interp::Linear, store::Interp::Closest, store::Interp::Previous, store::Interp::Next}) {
        if (s == store::interpName(i)) {
            return i;
        }
    }
    violate(pointer, "interp is Linear, Closest, Previous or Next");
}

JsonDoc docOf(const json& j, const std::string& pointer) {
    Result<JsonDoc> d = jsonio::toDoc(j);
    if (!d) {
        violate(pointer, d.error().message);
    }
    return std::move(d).value();
}

void readTableMembers(ObjectReader& r, const units::UnitRegistry& units, TableSpec& table) {
    if (const json* v = r.optional("axis")) {
        table.x = readAxis(*v, r.child("axis"), units);
    }
    if (const json* v = r.optional("x")) {
        table.x = readAxis(*v, r.child("x"), units);
    }
    if (const json* v = r.optional("y")) {
        table.y = readAxis(*v, r.child("y"), units);
    }
    if (const json* v = r.optional("values")) {
        readValues(*v, r.child("values"), units, table);
    }
    if (const json* v = r.optional("interpolation")) {
        const std::string& s = jsonio::asString(*v, r.child("interpolation"));
        if (s == "linear") {
            table.interpolation = TableSpec::Interp::Linear;
        } else if (s == "next") {
            table.interpolation = TableSpec::Interp::Next;
        } else if (s == "previous") {
            table.interpolation = TableSpec::Interp::Previous;
        } else {
            violate(r.child("interpolation"), "interpolation is linear, next or previous");
        }
    }
    if (const json* v = r.optional("outOfRange")) {
        const std::string& s = jsonio::asString(*v, r.child("outOfRange"));
        if (s == "clamp") {
            table.outOfRange = TableSpec::OutOfRange::Clamp;
        } else if (s == "invalid") {
            table.outOfRange = TableSpec::OutOfRange::Invalid;
        } else {
            violate(r.child("outOfRange"), "outOfRange is clamp or invalid");
        }
    }
}

MathItem readItem(const json& j, const std::string& pointer, const units::UnitRegistry& units) {
    ObjectReader r(j, pointer);
    MathItem item;
    item.kind = readKind(r.required("kind"), r.child("kind"));
    if (const json* v = r.optional("name")) {
        item.name = jsonio::asString(*v, r.child("name"));
        if (item.name.find('\'') != std::string::npos) {
            violate(r.child("name"), "a channel name cannot contain a single quote");
        }
    }
    if (const json* v = r.optional("enabled")) {
        item.enabled = jsonio::asBool(*v, r.child("enabled"));
    }
    if (const json* v = r.optional("comment")) {
        item.comment = jsonio::asString(*v, r.child("comment"));
    }
    if (const json* v = r.optional("expr")) {
        item.expr = jsonio::asString(*v, r.child("expr"));
    }
    if (const json* v = r.optional("rate")) {
        item.rate = readRate(*v, r.child("rate"));
    }
    if (const json* v = r.optional("quantity")) {
        const std::string& q = jsonio::asString(*v, r.child("quantity"));
        const std::optional<units::QuantityId> id = units.quantityByName(q);
        if (!id) {
            violate(r.child("quantity"), "unknown quantity '" + q + "'");
        }
        item.quantity = *id;
    }
    if (const json* v = r.optional("resultUnit")) {
        item.resultUnit = readUnit(*v, r.child("resultUnit"), units);
    }
    if (const json* v = r.optional("display")) {
        item.display = readDisplay(*v, r.child("display"), units);
    }
    if (const json* v = r.optional("interp")) {
        item.interp = readInterp(*v, r.child("interp"));
    }
    if (const json* v = r.optional("enumeration")) {
        item.enumeration = jsonio::asString(*v, r.child("enumeration"));
    }
    if (const json* v = r.optional("applies")) {
        ObjectReader a(*v, r.child("applies"));
        if (const json* l = a.optional("logged")) {
            item.applies.logged = jsonio::asBool(*l, a.child("logged"));
        }
        if (const json* t = a.optional("telemetry")) {
            item.applies.telemetry = jsonio::asBool(*t, a.child("telemetry"));
        }
        a.closed();
    }
    if (const json* v = r.optional("window")) {
        ObjectReader w(*v, r.child("window"));
        if (const json* p = w.optional("points")) {
            item.window.points = jsonio::asInt32(*p, w.child("points"));
        }
        if (const json* s = w.optional("seconds")) {
            item.window.seconds = jsonio::asNumber(*s, w.child("seconds"));
        }
        w.closed();
    }
    if (const json* v = r.optional("scale")) {
        item.scale = jsonio::asNumber(*v, r.child("scale"));
    }
    if (const json* v = r.optional("offset")) {
        item.offset = readUnitValue(*v, r.child("offset"), units);
    }
    readTableMembers(r, units, item.table);
    if (const json* v = r.optional("module")) {
        item.moduleId = jsonio::asString(*v, r.child("module"));
    }
    if (const json* v = r.optional("version")) {
        item.moduleVersion = jsonio::asInt32(*v, r.child("version"));
    }
    if (const json* v = r.optional("settings")) {
        item.settings = docOf(*v, r.child("settings"));
    }
    if (const json* v = r.optional("precision")) {
        const std::string& p = jsonio::asString(*v, r.child("precision"));
        if (p != "single" && p != "double") {
            violate(r.child("precision"), "precision is single or double");
        }
        item.precision = p;
    }
    if (const json* v = r.optional("x-i2")) {
        item.xI2 = docOf(*v, r.child("x-i2"));
    }
    r.closed();
    return item;
}

MathConstant readConstant(const json& j, const std::string& pointer, const units::UnitRegistry& units) {
    ObjectReader c(j, pointer);
    MathConstant k;
    k.name = jsonio::asString(c.required("name"), c.child("name"));
    k.value = readUnitValue(c.required("value"), c.child("value"), units);
    if (const json* e = c.optional("enabled")) {
        k.enabled = jsonio::asBool(*e, c.child("enabled"));
    }
    if (const json* e = c.optional("comment")) {
        k.comment = jsonio::asString(*e, c.child("comment"));
    }
    c.closed();
    return k;
}

MathEnumeration readEnumeration(const json& j, const std::string& pointer) {
    ObjectReader e(j, pointer);
    MathEnumeration en;
    en.name = jsonio::asString(e.required("name"), e.child("name"));
    const json& entries = e.required("entries");
    const std::string entp = e.child("entries");
    if (!entries.is_array()) {
        violate(entp, "entries is an array");
    }
    std::set<std::int64_t> values;
    for (std::size_t k = 0; k < entries.size(); ++k) {
        ObjectReader x(entries[k], jsonio::pointerIndex(entp, k));
        std::string name = jsonio::asString(x.required("name"), x.child("name"));
        const std::int64_t value = jsonio::asInt(x.required("value"), x.child("value"));
        x.closed();
        // Duplicate enumeration values are refused (maths-1, 03 section 4.1).
        if (!values.insert(value).second) {
            violate(x.child("value"), "enumeration value " + std::to_string(value) + " is used twice");
        }
        en.entries.emplace_back(std::move(name), value);
    }
    e.closed();
    return en;
}

void requireArray(const json& v, const std::string& pointer) {
    if (!v.is_array()) {
        violate(pointer, "an array is expected");
    }
}

MathModule readModule(const json& root, const units::UnitRegistry& units, std::vector<std::string>& log) {
    if (!root.is_object()) {
        violate("", "a maths-1 document is a JSON object");
    }
    ObjectReader r(root, "");
    if (root.contains("$schema")) {
        jsonio::checkSchemaId(root, kDocName, kDocMajor);
    } else {
        log.emplace_back("maths: no $schema; read as urn:esses:maths-1");
    }
    r.skip("$schema");
    if (const json* v = r.optional("schemaVersion")) {
        const int version = jsonio::asInt32(*v, r.child("schemaVersion"));
        if (version < 1) {
            violate(r.child("schemaVersion"), "schemaVersion is at least 1");
        }
        if (version > MathModuleIo::kSchemaVersion) {
            log.push_back("maths: schemaVersion " + std::to_string(version) + " is newer than " +
                          std::to_string(MathModuleIo::kSchemaVersion) + "; unknown members are preserved");
        }
    }
    MathModule m;
    m.id = jsonio::asString(r.required("id"), r.child("id"));
    if (m.id.empty()) {
        violate(r.child("id"), "a module id is not empty");
    }
    if (const json* v = r.optional("enabled")) {
        m.enabled = jsonio::asBool(*v, r.child("enabled"));
    }
    if (const json* v = r.optional("condition")) {
        m.condition = jsonio::asString(*v, r.child("condition"));
    }
    if (const json* v = r.optional("precision")) {
        m.precision = jsonio::asString(*v, r.child("precision"));
        if (m.precision != "single" && m.precision != "double") {
            violate(r.child("precision"), "precision is single or double");
        }
    }
    if (const json* v = r.optional("constants")) {
        const std::string cp = r.child("constants");
        requireArray(*v, cp);
        for (std::size_t i = 0; i < v->size(); ++i) {
            m.constants.push_back(readConstant((*v)[i], jsonio::pointerIndex(cp, i), units));
        }
    }
    if (const json* v = r.optional("enumerations")) {
        const std::string ep = r.child("enumerations");
        requireArray(*v, ep);
        for (std::size_t i = 0; i < v->size(); ++i) {
            m.enumerations.push_back(readEnumeration((*v)[i], jsonio::pointerIndex(ep, i)));
        }
    }
    if (const json* v = r.optional("items")) {
        const std::string ip = r.child("items");
        requireArray(*v, ip);
        for (std::size_t i = 0; i < v->size(); ++i) {
            m.items.push_back(readItem((*v)[i], jsonio::pointerIndex(ip, i), units));
        }
    }
    const json unknown = r.unread();
    if (!unknown.empty()) {
        m.unknown = docOf(unknown, "");
    }
    return m;
}

// ---- writing
// ---------------------------------------------------------------------------------------------------

std::string unitText(units::UnitId u, const units::UnitRegistry& units, const std::string& pointer) {
    if (u == units::kNoUnit) {
        return {};
    }
    if (!units.contains(u)) {
        violate(pointer, "unit id " + std::to_string(u) + " is not in the unit registry");
    }
    return units.unit(u).symbol;
}

json unitValueJson(const UnitValue& v, const units::UnitRegistry& units, const std::string& pointer) {
    if (!std::isfinite(v.v)) {
        violate(pointer, "a value is not finite");
    }
    return json{{"v", v.v}, {"unit", unitText(v.unit, units, pointer + "/unit")}};
}

json rateJson(const RateSpec& r) {
    if (r.automatic) {
        return "auto";
    }
    return json{{"num", r.rate.num}, {"den", r.rate.den}};
}

json axisJson(const TableSpec::Axis& a, const units::UnitRegistry& units, const std::string& pointer) {
    return json{{"expr", a.expr},
                {"unit", unitText(a.unit, units, pointer + "/unit")},
                {"breakpoints", a.breakpoints}};
}

void writeTable(json& j, const MathItem& item, const units::UnitRegistry& units, const std::string& pointer) {
    const TableSpec& t = item.table;
    if (t.y || item.kind == ItemKind::Table3D) {
        j["x"] = axisJson(t.x, units, pointer + "/x");
        j["y"] = axisJson(t.y.value_or(TableSpec::Axis{}), units, pointer + "/y");
    } else {
        j["axis"] = axisJson(t.x, units, pointer + "/axis");
    }
    json values = json::object();
    values["unit"] = unitText(t.valuesUnit, units, pointer + "/values/unit");
    const std::size_t cols = t.y ? t.y->breakpoints.size() : 0;
    if (t.y && t.rowMajor && cols > 0 && !t.values.empty() && t.values.size() % cols == 0) {
        json rows = json::array();
        for (std::size_t row = 0; row < t.values.size(); row += cols) {
            rows.push_back(std::vector<double>(t.values.begin() + static_cast<std::ptrdiff_t>(row),
                                               t.values.begin() + static_cast<std::ptrdiff_t>(row + cols)));
        }
        values["data"] = std::move(rows);
    } else {
        values["data"] = t.values;
    }
    if (t.y || !t.rowMajor) {
        values["rowMajor"] = t.rowMajor;
    }
    j["values"] = std::move(values);
    j["interpolation"] = t.interpolation == TableSpec::Interp::Linear
                             ? "linear"
                             : (t.interpolation == TableSpec::Interp::Next ? "next" : "previous");
    j["outOfRange"] = t.outOfRange == TableSpec::OutOfRange::Clamp ? "clamp" : "invalid";
}

json itemJson(const MathItem& item, const units::UnitRegistry& units, const std::string& pointer) {
    const MathItem def;
    const ItemKind k = item.kind;
    const bool expression = k == ItemKind::Expression;
    const bool table = k == ItemKind::Table2D || k == ItemKind::Table3D;
    const bool moduleRef = k == ItemKind::Builtin || k == ItemKind::Plugin;
    json j = json::object();
    j["kind"] = kindName(k);
    j["enabled"] = item.enabled;
    if (!item.name.empty()) {
        j["name"] = item.name;
    }
    if (!item.comment.empty()) {
        j["comment"] = item.comment;
    }
    if (expression || item.expr != def.expr) {
        j["expr"] = item.expr;
    }
    if (expression || table || item.rate != def.rate) {
        j["rate"] = rateJson(item.rate);
    }
    if (expression || table || item.quantity != def.quantity) {
        if (!units.containsQuantity(item.quantity)) {
            violate(pointer + "/quantity", "quantity id is not in the unit registry");
        }
        j["quantity"] = units.quantity(item.quantity).name;
    }
    if (expression || table || item.resultUnit != def.resultUnit) {
        j["resultUnit"] = unitText(item.resultUnit, units, pointer + "/resultUnit");
    }
    if (expression || table || k == ItemKind::Filter || item.display != def.display) {
        j["display"] = json{{"unit", unitText(item.display.unit, units, pointer + "/display/unit")},
                            {"dps", item.display.dps},
                            {"colourIndex", item.display.colourIndex}};
    }
    if (expression || table || item.interp != def.interp) {
        j["interp"] = store::interpName(item.interp);
    }
    if (item.enumeration) {
        j["enumeration"] = *item.enumeration;
    }
    if (item.applies != def.applies) {
        j["applies"] = json{{"logged", item.applies.logged}, {"telemetry", item.applies.telemetry}};
    }
    if (k == ItemKind::Filter || item.window != def.window) {
        json w = json::object();
        if (item.window.points) {
            w["points"] = *item.window.points;
        }
        if (item.window.seconds) {
            w["seconds"] = *item.window.seconds;
        }
        j["window"] = std::move(w);
    }
    if (k == ItemKind::ScaleOffset || item.scale != def.scale) {
        j["scale"] = item.scale;
    }
    if (k == ItemKind::ScaleOffset || item.offset != def.offset) {
        j["offset"] = unitValueJson(item.offset, units, pointer + "/offset");
    }
    if (table || item.table != def.table) {
        writeTable(j, item, units, pointer);
    }
    if (moduleRef || !item.moduleId.empty()) {
        j["module"] = item.moduleId;
    }
    if (moduleRef || item.moduleVersion != def.moduleVersion) {
        j["version"] = item.moduleVersion;
    }
    if (!item.settings.isNull()) {
        j["settings"] = jsonio::fromDoc(item.settings);
    }
    if (item.precision) {
        j["precision"] = *item.precision;
    }
    if (!item.xI2.isNull()) {
        j["x-i2"] = jsonio::fromDoc(item.xI2);
    }
    return j;
}

json moduleJson(const MathModule& m, const units::UnitRegistry& units) {
    json root = jsonio::fromDoc(m.unknown);
    if (!root.is_object()) {
        root = json::object();
    }
    root["$schema"] = std::string(MathModuleIo::kSchemaId);
    root["schemaVersion"] = MathModuleIo::kSchemaVersion;
    root["id"] = m.id;
    root["enabled"] = m.enabled;
    if (m.condition) {
        root["condition"] = *m.condition;
    }
    root["precision"] = m.precision;
    json constants = json::array();
    for (std::size_t i = 0; i < m.constants.size(); ++i) {
        const MathConstant& c = m.constants[i];
        const std::string p = "/constants/" + std::to_string(i);
        json cj{
            {"name", c.name}, {"value", unitValueJson(c.value, units, p + "/value")}, {"enabled", c.enabled}};
        if (!c.comment.empty()) {
            cj["comment"] = c.comment;
        }
        constants.push_back(std::move(cj));
    }
    root["constants"] = std::move(constants);
    json enumerations = json::array();
    for (const MathEnumeration& e : m.enumerations) {
        json entries = json::array();
        for (const auto& [name, value] : e.entries) {
            entries.push_back(json{{"name", name}, {"value", value}});
        }
        enumerations.push_back(json{{"name", e.name}, {"entries", std::move(entries)}});
    }
    root["enumerations"] = std::move(enumerations);
    json items = json::array();
    for (std::size_t i = 0; i < m.items.size(); ++i) {
        items.push_back(itemJson(m.items[i], units, "/items/" + std::to_string(i)));
    }
    root["items"] = std::move(items);
    return root;
}

} // namespace

Result<MathModule> MathModuleIo::parse(std::string_view json, const units::UnitRegistry& units,
                                       std::vector<std::string>& log) {
    Result<nlohmann::json> root = jsonio::parseJson(json);
    if (!root) {
        return Failure{root.error()};
    }
    return jsonio::guardJson([&]() -> Result<MathModule> { return readModule(*root, units, log); });
}

Result<MathModule> MathModuleIo::fromJson(const model::OpaqueJson& doc, const units::UnitRegistry& units,
                                          std::vector<std::string>& log) {
    const nlohmann::json root = jsonio::fromDoc(doc);
    return jsonio::guardJson([&]() -> Result<MathModule> { return readModule(root, units, log); });
}

Result<MathModule> MathModuleIo::load(const std::filesystem::path& path, const units::UnitRegistry& units,
                                      std::vector<std::string>& log) {
    Result<std::string> text = jsonio::readFile(path);
    if (!text) {
        return Failure{text.error()};
    }
    Result<MathModule> m = parse(*text, units, log);
    if (!m) {
        Error e = m.error();
        e.context.path = path;
        return Failure{std::move(e)};
    }
    return m;
}

Result<model::OpaqueJson> MathModuleIo::toJsonDoc(const MathModule& module,
                                                  const units::UnitRegistry& units) {
    return jsonio::guardJson(
        [&]() -> Result<model::OpaqueJson> { return jsonio::toDoc(moduleJson(module, units)); });
}

Result<std::string> MathModuleIo::toJson(const MathModule& module, const units::UnitRegistry& units) {
    return jsonio::guardJson(
        [&]() -> Result<std::string> { return jsonio::canonicalJson(moduleJson(module, units)); });
}

Result<void> MathModuleIo::save(const MathModule& module, const std::filesystem::path& path,
                                const units::UnitRegistry& units) {
    if (module.origin == MathModule::Origin::Transient || module.id == MathModule::kTransientId) {
        return fail(ErrorCode::Invalid, "a transient maths module is never saved");
    }
    Result<std::string> text = toJson(module, units);
    if (!text) {
        return Failure{text.error()};
    }
    return AtomicFile::write(path, *text);
}

} // namespace esses::maths
