// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// probe_json (05 section 1.2, schema probe-1): ProbeResult <-> JSON. The document is built with nlohmann-json
// and canonicalised by esses_model's writer through model::OpaqueJson, so probe_json, the sidecar and every
// other document share one canonical form (02 section 6.5 rule 1).
#include <esses/base/utf8.hpp>
#include <esses/formats/probe.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <utility>

namespace esses::formats {
namespace {

using json = nlohmann::json;

struct Violation {
    ErrorCode code = ErrorCode::Invalid;
    std::string pointer;
    std::string message;
};

[[noreturn]] void violate(std::string pointer, std::string message, ErrorCode code = ErrorCode::Invalid) {
    throw Violation{code, std::move(pointer), std::move(message)};
}

std::string child(const std::string& parent, std::string_view key) {
    std::string out = parent + "/";
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}
std::string child(const std::string& parent, std::size_t index) {
    return parent + "/" + std::to_string(index);
}

// ---- writing ----------------------------------------------------------------------------------------------

json detailValueJson(const model::DetailValue& v, const std::string& pointer) {
    if (const auto* s = std::get_if<std::string>(&v)) {
        return *s; // the short form of a text value
    }
    json j = json::object();
    if (const auto* q = std::get_if<model::Quantityd>(&v)) {
        if (!std::isfinite(q->v)) {
            violate(pointer + "/v", "non-finite number");
        }
        j["t"] = "number";
        j["v"] = q->v;
        if (!q->symbol.empty()) {
            j["unit"] = q->symbol;
        }
        j["dps"] = q->dps < 0 ? 0 : q->dps;
        return j;
    }
    j["t"] = "datetime";
    j["v"] = std::get<model::DateTime>(v).iso();
    return j;
}

json ctxJson(const ImportLogEntry::Context& c) {
    json j = json::object();
    if (c.layer) {
        j["layer"] = *c.layer;
    }
    if (c.channel) {
        j["channel"] = *c.channel;
    }
    if (c.detail) {
        j["detail"] = *c.detail;
    }
    if (c.line) {
        j["line"] = *c.line;
    }
    if (c.offset) {
        j["offset"] = *c.offset;
    }
    if (c.repeats != 1) {
        j["repeats"] = c.repeats;
    }
    return j;
}

json channelJson(const ChannelInfo& c) {
    json j = json::object();
    j["index"] = c.index;
    j["loggedName"] = c.loggedName;
    j["shortName"] = c.shortName;
    j["vendorUnit"] = c.vendorUnit;
    if (c.vendorId) {
        j["vendorId"] = *c.vendorId;
    }
    j["vendorDtype"] = c.vendorDtype;
    j["dtype"] = std::string(store::dtypeName(c.dtype));
    j["kind"] = std::string(store::sampleKindName(c.kind));
    if (const auto* r = std::get_if<Rate>(&c.timing)) {
        j["timing"] = json{{"rate", json{{"num", r->num}, {"den", r->den}}}};
    } else {
        j["timing"] = json{{"timestamped", true}};
    }
    j["sampleCount"] = c.sampleCount;
    j["t0Us"] = c.t0Us;
    j["dps"] = c.dps < 0 ? 0 : c.dps;
    if (c.enumTable) {
        j["enumTable"] = *c.enumTable;
    }
    j["flags"] = c.flags;
    return j;
}

json markerJson(const MarkerInfo& m) {
    json j = json::object();
    j["class"] = std::string(model::markerClassName(m.cls));
    j["tUs"] = m.tUs;
    j["name"] = m.name;
    j["group"] = m.group;
    j["comment"] = m.comment;
    j["flags"] = m.flags;
    j["ordinal"] = m.ordinal;
    return j;
}

json probeJson(const ProbeResult& p) {
    json j = json::object();
    j["$schema"] = std::string(ProbeResult::kSchemaId);
    j["schemaVersion"] = ProbeResult::kSchemaVersion;
    j["importerId"] = p.importerId;
    j["importerVersion"] = p.importerVersion;
    j["dialect"] = p.dialect;
    json companions = json::array();
    for (const auto& c : p.companions) {
        companions.push_back(pathToUtf8(c.filename()));
    }
    j["companions"] = std::move(companions);
    json details = json::array();
    for (std::size_t i = 0; i < p.details.size(); ++i) {
        const model::Detail& d = p.details[i];
        details.push_back(
            json{{"id", d.id}, {"value", detailValueJson(d.value, "/details/" + std::to_string(i))}});
    }
    j["details"] = std::move(details);
    json layers = json::array();
    for (const LayerInfo& l : p.layers) {
        json lj = json::object();
        lj["name"] = l.name;
        lj["durationUs"] = l.durationUs;
        if (l.utc) {
            json u = json::object();
            u["unixUs"] = l.utc->unixUs;
            if (l.utcAssumedLocal) {
                u["assumedLocal"] = true;
            }
            lj["utc"] = std::move(u);
        }
        json channels = json::array();
        for (const ChannelInfo& c : l.channels) {
            channels.push_back(channelJson(c));
        }
        lj["channels"] = std::move(channels);
        json markers = json::array();
        for (const MarkerInfo& m : l.markers) {
            markers.push_back(markerJson(m));
        }
        lj["markers"] = std::move(markers);
        layers.push_back(std::move(lj));
    }
    j["layers"] = std::move(layers);
    json tables = json::array();
    for (const EnumTableInfo& t : p.enumTables) {
        json entries = json::array();
        for (const model::EnumEntry& e : t.entries) {
            entries.push_back(json{{"value", e.value}, {"label", e.label}, {"severity", e.severity}});
        }
        tables.push_back(json{{"id", t.id}, {"entries", std::move(entries)}});
    }
    j["enumTables"] = std::move(tables);
    json warnings = json::array();
    for (const ImportLogEntry& e : p.warnings) {
        warnings.push_back(json{{"level", std::string(importLogLevelName(e.level))},
                                {"code", e.code},
                                {"message", e.message},
                                {"ctx", ctxJson(e.ctx)}});
    }
    j["warnings"] = std::move(warnings);
    return j;
}

// ---- reading ----------------------------------------------------------------------------------------------

class Obj {
public:
    Obj(const json& j, std::string pointer) : j_(j), pointer_(std::move(pointer)) {
        if (!j_.is_object()) {
            violate(pointer_, "expected an object");
        }
    }
    const json& req(std::string_view key) {
        const json* v = opt(key);
        if (v == nullptr) {
            violate(at(key), "required member is missing");
        }
        return *v;
    }
    // A required member that must be an array.
    const json& reqArray(std::string_view key) {
        const json& v = req(key);
        if (!v.is_array()) {
            violate(at(key), "expected an array");
        }
        return v;
    }
    const json* opt(std::string_view key) {
        seen_.insert(std::string(key));
        const auto it = j_.find(std::string(key));
        return it == j_.end() ? nullptr : &*it;
    }
    std::string at(std::string_view key) const { return child(pointer_, key); }
    void closed() const {
        for (auto it = j_.begin(); it != j_.end(); ++it) {
            if (seen_.count(it.key()) == 0) {
                violate(child(pointer_, it.key()), "unknown member");
            }
        }
    }

private:
    const json& j_;
    std::string pointer_;
    std::set<std::string> seen_;
};

// The pointer is a string_view so that a temporary pointer (o.at("x")) is not bound to a reference parameter
// of a function returning a reference (GCC 13 -Wdangling-reference).
const std::string& str(const json& j, std::string_view pointer) {
    if (!j.is_string()) {
        violate(std::string(pointer), "expected a string");
    }
    return j.get_ref<const std::string&>();
}
std::int64_t i64(const json& j, const std::string& pointer) {
    if (j.is_number_unsigned()) {
        if (j.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            violate(pointer, "integer out of range");
        }
        return static_cast<std::int64_t>(j.get<std::uint64_t>());
    }
    if (!j.is_number_integer()) {
        violate(pointer, "expected an integer");
    }
    return j.get<std::int64_t>();
}
std::uint64_t u64(const json& j, const std::string& pointer) {
    if (j.is_number_unsigned()) {
        return j.get<std::uint64_t>();
    }
    if (!j.is_number_integer() || j.get<std::int64_t>() < 0) {
        violate(pointer, "expected a non-negative integer");
    }
    return static_cast<std::uint64_t>(j.get<std::int64_t>());
}
std::uint32_t u32(const json& j, const std::string& pointer) {
    const std::uint64_t v = u64(j, pointer);
    if (v > 0xFFFFFFFFull) {
        violate(pointer, "integer above 2^32 - 1");
    }
    return static_cast<std::uint32_t>(v);
}
double number(const json& j, const std::string& pointer) {
    if (!j.is_number()) {
        violate(pointer, "expected a number");
    }
    const double v = j.get<double>();
    if (!std::isfinite(v)) {
        violate(pointer, "non-finite number");
    }
    return v;
}

void checkSchema(const json& j, const std::string& pointer) {
    const std::string& s = str(j, pointer);
    std::string_view name = s;
    if (name.starts_with("urn:esses:")) {
        name.remove_prefix(10);
    } else if (name.starts_with("esses:")) {
        name.remove_prefix(6);
    } else if (name.starts_with("https://")) {
        const std::size_t slash = name.rfind('/');
        name = name.substr(slash + 1);
        if (name.ends_with(".schema.json")) {
            name.remove_suffix(12);
        } else if (name.ends_with(".json")) {
            name.remove_suffix(5);
        }
    } else {
        violate(pointer, "unrecognised schema id '" + s + "'");
    }
    if (!name.starts_with("probe-")) {
        violate(pointer, "not a probe-1 document ('" + s + "')");
    }
    if (name != "probe-1") {
        violate(pointer, "schema '" + s + "' is another major version of probe", ErrorCode::Unsupported);
    }
}

// "" or flags of [a-z0-9] words joined by single '-', separated by single ',' (the probe-1 dialect pattern).
bool validDialect(std::string_view d) {
    if (d.empty()) {
        return true;
    }
    char prev = ',';
    for (const char c : d) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!word && ((c != '-' && c != ',') || prev == ',' || prev == '-')) {
            return false;
        }
        prev = c;
    }
    return prev != ',' && prev != '-';
}

model::DetailValue readDetailValue(const json& j, const std::string& pointer) {
    if (j.is_string()) {
        return j.get<std::string>();
    }
    Obj o(j, pointer);
    const std::string& t = str(o.req("t"), o.at("t"));
    model::DetailValue out;
    if (t == "string") {
        out = str(o.req("v"), o.at("v"));
    } else if (t == "number") {
        model::Quantityd q;
        q.v = number(o.req("v"), o.at("v"));
        if (const json* u = o.opt("unit")) {
            q.symbol = str(*u, o.at("unit"));
        }
        if (const json* d = o.opt("dps")) {
            const std::int64_t dps = i64(*d, o.at("dps"));
            if (dps < 0 || dps > 15) {
                violate(o.at("dps"), "dps must be between 0 and 15");
            }
            q.dps = static_cast<int>(dps);
        }
        out = std::move(q);
    } else if (t == "datetime") {
        const std::string& text = str(o.req("v"), o.at("v"));
        const auto dt = model::DateTime::parseIso(text);
        if (!dt) {
            violate(o.at("v"), "not an ISO 8601 date-time with seconds and an offset");
        }
        out = *dt;
    } else {
        violate(o.at("t"), "unknown detail value type '" + t + "'");
    }
    o.closed();
    return out;
}

ImportLogEntry::Context readCtx(const json& j, const std::string& pointer) {
    Obj o(j, pointer);
    ImportLogEntry::Context c;
    if (const json* v = o.opt("layer")) {
        c.layer = str(*v, o.at("layer"));
    }
    if (const json* v = o.opt("channel")) {
        c.channel = str(*v, o.at("channel"));
    }
    if (const json* v = o.opt("detail")) {
        c.detail = str(*v, o.at("detail"));
    }
    if (const json* v = o.opt("line")) {
        c.line = u64(*v, o.at("line"));
    }
    if (const json* v = o.opt("offset")) {
        c.offset = u64(*v, o.at("offset"));
    }
    if (const json* v = o.opt("repeats")) {
        c.repeats = u32(*v, o.at("repeats"));
        if (c.repeats == 0) {
            violate(o.at("repeats"), "repeats must be at least 1");
        }
    }
    o.closed();
    return c;
}

ChannelInfo readChannel(const json& j, const std::string& pointer) {
    Obj o(j, pointer);
    ChannelInfo c;
    c.index = u32(o.req("index"), o.at("index"));
    c.loggedName = str(o.req("loggedName"), o.at("loggedName"));
    c.shortName = str(o.req("shortName"), o.at("shortName"));
    c.vendorUnit = str(o.req("vendorUnit"), o.at("vendorUnit"));
    if (const json* v = o.opt("vendorId")) {
        c.vendorId = str(*v, o.at("vendorId"));
    }
    c.vendorDtype = str(o.req("vendorDtype"), o.at("vendorDtype"));
    const auto dt = parseDtype(str(o.req("dtype"), o.at("dtype")));
    if (!dt || *dt == store::Dtype::I64) {
        violate(o.at("dtype"), "dtype must be F32, F64 or I32");
    }
    c.dtype = *dt;
    const auto kind = parseSampleKind(str(o.req("kind"), o.at("kind")));
    if (!kind) {
        violate(o.at("kind"), "unknown sample kind");
    }
    c.kind = *kind;
    {
        Obj t(o.req("timing"), o.at("timing"));
        const json* rate = t.opt("rate");
        const json* ts = t.opt("timestamped");
        if ((rate == nullptr) == (ts == nullptr)) {
            violate(o.at("timing"), "timing holds exactly one of rate and timestamped");
        }
        if (rate != nullptr) {
            Obj r(*rate, t.at("rate"));
            Rate value{u32(r.req("num"), r.at("num")), u32(r.req("den"), r.at("den"))};
            if (!value.valid()) {
                violate(t.at("rate"), "rate num and den must be non-zero");
            }
            r.closed();
            c.timing = value;
        } else {
            if (!ts->is_boolean() || !ts->get<bool>()) {
                violate(t.at("timestamped"), "timestamped must be true");
            }
            c.timing = Timestamped{};
        }
        t.closed();
    }
    c.sampleCount = u64(o.req("sampleCount"), o.at("sampleCount"));
    c.t0Us = i64(o.req("t0Us"), o.at("t0Us"));
    const std::int64_t dps = i64(o.req("dps"), o.at("dps"));
    if (dps < 0 || dps > std::numeric_limits<int>::max()) {
        violate(o.at("dps"), "dps must be a non-negative integer");
    }
    c.dps = static_cast<int>(dps);
    if (const json* v = o.opt("enumTable")) {
        c.enumTable = u32(*v, o.at("enumTable"));
    }
    c.flags = u32(o.req("flags"), o.at("flags"));
    if (c.kind == store::SampleKind::Enum && !c.enumTable) {
        violate(pointer, "an Enum channel names its enumTable");
    }
    o.closed();
    return c;
}

MarkerInfo readMarker(const json& j, const std::string& pointer) {
    Obj o(j, pointer);
    MarkerInfo m;
    const auto cls = model::parseMarkerClass(str(o.req("class"), o.at("class")));
    if (!cls) {
        violate(o.at("class"), "unknown marker class");
    }
    m.cls = *cls;
    m.tUs = i64(o.req("tUs"), o.at("tUs"));
    m.name = str(o.req("name"), o.at("name"));
    m.group = str(o.req("group"), o.at("group"));
    m.comment = str(o.req("comment"), o.at("comment"));
    m.flags = u32(o.req("flags"), o.at("flags"));
    m.ordinal = u32(o.req("ordinal"), o.at("ordinal"));
    o.closed();
    return m;
}

ProbeResult readProbe(const json& j) {
    Obj o(j, "");
    checkSchema(o.req("$schema"), o.at("$schema"));
    if (i64(o.req("schemaVersion"), o.at("schemaVersion")) < 1) {
        violate(o.at("schemaVersion"), "schemaVersion must be at least 1");
    }
    ProbeResult p;
    p.importerId = str(o.req("importerId"), o.at("importerId"));
    p.importerVersion = str(o.req("importerVersion"), o.at("importerVersion"));
    p.dialect = str(o.req("dialect"), o.at("dialect"));
    if (!validDialect(p.dialect)) {
        violate(o.at("dialect"), "dialect flags are lower-case words joined by '-', separated by ',' alone");
    }
    {
        const std::string ptr = o.at("companions");
        const json& a = o.reqArray("companions");
        for (std::size_t i = 0; i < a.size(); ++i) {
            const std::string& name = str(a[i], child(ptr, i));
            if (name.empty() || name.find_first_of("/\\") != std::string::npos) {
                violate(child(ptr, i), "a companion is a bare file name");
            }
            p.companions.push_back(pathFromUtf8(name));
        }
    }
    {
        const std::string ptr = o.at("details");
        const json& a = o.reqArray("details");
        for (std::size_t i = 0; i < a.size(); ++i) {
            Obj d(a[i], child(ptr, i));
            model::Detail detail;
            detail.id = str(d.req("id"), d.at("id"));
            detail.value = readDetailValue(d.req("value"), d.at("value"));
            detail.prov = model::Provenance::Logged;
            d.closed();
            p.details.push_back(std::move(detail));
        }
    }
    {
        const std::string ptr = o.at("layers");
        const json& a = o.reqArray("layers");
        if (a.empty()) {
            violate(ptr, "at least one layer");
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const std::string lp = child(ptr, i);
            Obj l(a[i], lp);
            LayerInfo layer;
            layer.name = str(l.req("name"), l.at("name"));
            if (i == 0 && layer.name != "Normal") {
                violate(l.at("name"), "the first layer is \"Normal\"");
            }
            layer.durationUs = i64(l.req("durationUs"), l.at("durationUs"));
            if (layer.durationUs < 0) {
                violate(l.at("durationUs"), "durationUs must not be negative");
            }
            if (const json* u = l.opt("utc")) {
                Obj uo(*u, l.at("utc"));
                layer.utc = UtcInstant{i64(uo.req("unixUs"), uo.at("unixUs"))};
                if (const json* al = uo.opt("assumedLocal")) {
                    if (!al->is_boolean()) {
                        violate(uo.at("assumedLocal"), "expected a boolean");
                    }
                    layer.utcAssumedLocal = al->get<bool>();
                }
                uo.closed();
            }
            const json& ch = l.reqArray("channels");
            for (std::size_t k = 0; k < ch.size(); ++k) {
                layer.channels.push_back(readChannel(ch[k], child(l.at("channels"), k)));
            }
            const json& mk = l.reqArray("markers");
            for (std::size_t k = 0; k < mk.size(); ++k) {
                layer.markers.push_back(readMarker(mk[k], child(l.at("markers"), k)));
            }
            l.closed();
            p.layers.push_back(std::move(layer));
        }
    }
    {
        const std::string ptr = o.at("enumTables");
        const json& a = o.reqArray("enumTables");
        for (std::size_t i = 0; i < a.size(); ++i) {
            Obj t(a[i], child(ptr, i));
            EnumTableInfo table;
            table.id = u32(t.req("id"), t.at("id"));
            const json& entries = t.reqArray("entries");
            for (std::size_t k = 0; k < entries.size(); ++k) {
                Obj e(entries[k], child(t.at("entries"), k));
                model::EnumEntry entry;
                const std::int64_t v = i64(e.req("value"), e.at("value"));
                if (v < std::numeric_limits<std::int32_t>::min() ||
                    v > std::numeric_limits<std::int32_t>::max()) {
                    violate(e.at("value"), "value outside the 32-bit range");
                }
                entry.value = static_cast<std::int32_t>(v);
                entry.label = str(e.req("label"), e.at("label"));
                const std::uint64_t sev = u64(e.req("severity"), e.at("severity"));
                if (sev > 3) {
                    violate(e.at("severity"), "severity is 0 to 3");
                }
                entry.severity = static_cast<std::uint8_t>(sev);
                e.closed();
                table.entries.push_back(std::move(entry));
            }
            t.closed();
            p.enumTables.push_back(std::move(table));
        }
    }
    {
        const std::string ptr = o.at("warnings");
        const json& a = o.reqArray("warnings");
        for (std::size_t i = 0; i < a.size(); ++i) {
            Obj w(a[i], child(ptr, i));
            ImportLogEntry e;
            const auto level = parseImportLogLevel(str(w.req("level"), w.at("level")));
            if (!level) {
                violate(w.at("level"), "level is Info, Warning or Error");
            }
            e.level = *level;
            e.code = str(w.req("code"), w.at("code"));
            e.message = str(w.req("message"), w.at("message"));
            e.ctx = readCtx(w.req("ctx"), w.at("ctx"));
            w.closed();
            p.warnings.push_back(std::move(e));
        }
    }
    (void)o.opt("x-esses-legacy"); // fields a migration removed: accepted, not kept
    o.closed();
    return p;
}

} // namespace

std::optional<store::Dtype> parseDtype(std::string_view text) noexcept {
    for (const store::Dtype d :
         {store::Dtype::F32, store::Dtype::F64, store::Dtype::I32, store::Dtype::I64}) {
        if (store::dtypeName(d) == text) {
            return d;
        }
    }
    return std::nullopt;
}

std::optional<store::SampleKind> parseSampleKind(std::string_view text) noexcept {
    using K = store::SampleKind;
    for (const K k : {K::Real, K::Integer, K::Enum, K::Bool, K::Bitfield, K::Text}) {
        if (store::sampleKindName(k) == text) {
            return k;
        }
    }
    return std::nullopt;
}

std::optional<ImportLogEntry::Level> parseImportLogLevel(std::string_view text) noexcept {
    using L = ImportLogEntry::Level;
    for (const L l : {L::Info, L::Warning, L::Error}) {
        if (importLogLevelName(l) == text) {
            return l;
        }
    }
    return std::nullopt;
}

Result<model::OpaqueJson> ProbeResult::toJson() const {
    try {
        const json j = probeJson(*this);
        // Invalid UTF-8 cannot reach this point (readers transcode); replace it rather than throw if it does.
        return model::OpaqueJson::parse(j.dump(-1, ' ', false, json::error_handler_t::replace));
    } catch (const Violation& v) {
        ErrorContext ctx;
        ctx.detail = v.pointer;
        return fail(v.code, "probe_json: " + v.message + " at '" + v.pointer + "'", std::move(ctx));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory writing probe_json");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("probe_json: ") + e.what());
    }
}

Result<std::string> ProbeResult::canonicalJson() const {
    auto j = toJson();
    if (!j) {
        return Failure{j.error()};
    }
    return j->canonical();
}

Result<ProbeResult> ProbeResult::fromJson(const model::OpaqueJson& value) {
    try {
        if (value.isNull()) {
            return fail(ErrorCode::Invalid, "probe_json: no document");
        }
        return readProbe(json::parse(value.compact()));
    } catch (const Violation& v) {
        ErrorContext ctx;
        ctx.detail = v.pointer;
        ctx.schemaId = std::string(kSchemaId);
        const std::string where =
            v.pointer.empty() ? std::string("the document root") : "'" + v.pointer + "'";
        return fail(v.code, "probe_json: " + v.message + " at " + where, std::move(ctx));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory reading probe_json");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("probe_json: ") + e.what());
    }
}

Result<ProbeResult> ProbeResult::fromJsonText(std::string_view text) {
    auto parsed = model::OpaqueJson::parse(text);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    return fromJson(*parsed);
}

} // namespace esses::formats
