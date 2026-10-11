// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// sidecar-1 reading and writing (02 section 5.2, schemas/sidecar-1.schema.json).
#include "json_canonical.hpp"
#include "json_read.hpp"
#include "marker_json.hpp"

#include <esses/model/sidecar.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace esses::model {

namespace {

using detail::asBool;
using detail::asInt;
using detail::asNumber;
using detail::asString;
using detail::asU64;
using detail::asUuid;
using detail::ObjectReader;
using detail::pointerChild;
using detail::pointerIndex;
using detail::violate;
using nlohmann::json;

constexpr std::string_view kKnownTopLevel[] = {"$schema", "schemaVersion", "source", "edited", "layers"};

bool isKnownTopLevel(std::string_view key) noexcept {
    return std::find(std::begin(kKnownTopLevel), std::end(kKnownTopLevel), key) != std::end(kKnownTopLevel);
}

OpaqueJson opaqueOrViolate(const json& j, const std::string& pointer) {
    auto o = detail::toOpaque(j);
    if (!o) {
        violate(pointer, o.error().message);
    }
    return std::move(o).value();
}

void requireArray(const json& j, const std::string& pointer) {
    if (!j.is_array()) {
        violate(pointer, "expected an array");
    }
}

std::string nonEmptyString(const json& j, const std::string& pointer, std::string_view what) {
    const std::string& s = asString(j, pointer);
    if (s.empty()) {
        violate(pointer, std::string(what) + " must not be empty");
    }
    return s;
}

// ---- reading
// ------------------------------------------------------------------------------------------------------

DetailValue detailValueFromJson(const json& j, const std::string& pointer) {
    if (j.is_string()) {
        return j.get<std::string>(); // short form of a text value (probe-1 writes it)
    }
    ObjectReader r(j, pointer);
    const std::string& t = asString(r.required("t"), r.child("t"));
    DetailValue out;
    if (t == "string") {
        out = asString(r.required("v"), r.child("v"));
    } else if (t == "number") {
        Quantityd q;
        q.v = asNumber(r.required("v"), r.child("v"));
        if (const json* unit = r.optional("unit")) {
            q.symbol = asString(*unit, r.child("unit"));
            if (q.symbol.empty() || q.symbol.front() == ' ' || q.symbol.back() == ' ') {
                violate(r.child("unit"),
                        "a unit symbol has no leading or trailing white space and is not empty");
            }
        }
        if (const json* dps = r.optional("dps")) {
            const std::int64_t v = asInt(*dps, r.child("dps"));
            if (v < 0 || v > 15) {
                violate(r.child("dps"), "dps must be between 0 and 15");
            }
            q.dps = static_cast<int>(v);
        }
        out = std::move(q);
    } else if (t == "datetime") {
        const std::string& text = asString(r.required("v"), r.child("v"));
        const auto dt = DateTime::parseIso(text);
        if (!dt) {
            violate(r.child("v"),
                    "expected an ISO 8601 date-time with seconds and a UTC offset, found '" + text + "'");
        }
        out = *dt;
    } else {
        violate(r.child("t"), "unknown detail value type '" + t + "' (string, number or datetime)");
    }
    r.closed();
    return out;
}

Detail detailFromJson(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    Detail d;
    d.id = nonEmptyString(r.required("id"), r.child("id"), "a detail id");
    d.value = detailValueFromJson(r.required("value"), r.child("value"));
    if (const json* original = r.optional("original")) {
        d.original = detailValueFromJson(*original, r.child("original"));
    }
    d.prov = Provenance::User;
    r.closed();
    return d;
}

MarkerEdit markerEditFromJson(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    const std::string& opText = asString(r.required("op"), r.child("op"));
    const auto op = parseMarkerEditOp(opText);
    if (!op) {
        violate(r.child("op"), "unknown marker edit '" + opText + "'");
    }
    MarkerEdit e;
    e.op = *op;
    switch (e.op) {
    case MarkerEditOp::Offset:
        e.ref = MarkerId{asUuid(r.required("ref"), r.child("ref"))};
        e.dtUs = asInt(r.required("dtUs"), r.child("dtUs"));
        break;
    case MarkerEditOp::Ignore:
        e.ref = MarkerId{asUuid(r.required("ref"), r.child("ref"))};
        break;
    case MarkerEditOp::Restore:
        violate(r.child("op"), "restore is a command and is never stored");
    case MarkerEditOp::OffsetDistance: {
        ObjectReader q(r.required("dM"), r.child("dM"));
        e.dM = asNumber(q.required("v"), q.child("v"));
        e.dMUnit = nonEmptyString(q.required("unit"), q.child("unit"), "a unit symbol");
        q.closed();
        break;
    }
    }
    r.closed();
    return e;
}

RangeOverride rangeOverrideFromJson(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    RangeOverride ov;
    ov.groupPath = nonEmptyString(r.required("group"), r.child("group"), "a group path");
    if (ov.groupPath.front() == ':' || ov.groupPath.back() == ':' ||
        ov.groupPath.find("::") != std::string::npos) {
        violate(r.child("group"), "a group path has non-empty colon-separated segments");
    }
    if (const json* sm = r.optional("startMarker")) {
        ov.startMarker = MarkerId{asUuid(*sm, r.child("startMarker"))};
    }
    ov.name = nonEmptyString(r.required("name"), r.child("name"), "the override name");
    if (const json* t = r.optional("trusted")) {
        ov.trusted = asBool(*t, r.child("trusted"));
    }
    if (const json* o = r.optional("offsetUs")) {
        ov.offsetUs = asInt(*o, r.child("offsetUs"));
    }
    if (const json* m = r.optional("offsetM")) {
        ov.offsetM = asNumber(*m, r.child("offsetM"));
    }
    r.closed();
    return ov;
}

Annotation annotationFromJson(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    Annotation a;
    a.id = asUuid(r.required("id"), r.child("id"));
    a.tUs = asInt(r.required("tUs"), r.child("tUs"));
    a.text = asString(r.required("text"), r.child("text"));
    if (const json* c = r.optional("channel")) {
        a.channel = nonEmptyString(*c, r.child("channel"), "the annotation channel");
    }
    r.closed();
    return a;
}

VideoLink videoLinkFromJson(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    VideoLink v;
    v.file = nonEmptyString(r.required("file"), r.child("file"), "the media file");
    v.cameraView = asString(r.required("cameraView"), r.child("cameraView"));
    v.offsetUs = asInt(r.required("offsetUs"), r.child("offsetUs"));
    const std::string spPtr = r.child("syncPoints");
    const json& sp = r.required("syncPoints");
    requireArray(sp, spPtr);
    for (std::size_t i = 0; i < sp.size(); ++i) {
        const std::string p = pointerIndex(spPtr, i);
        const json& pair = sp[i];
        requireArray(pair, p);
        if (pair.size() != 2) {
            violate(p, "a sync point is a pair [log time, media time]");
        }
        v.syncPoints.emplace_back(asInt(pair[0], pointerIndex(p, 0)), asInt(pair[1], pointerIndex(p, 1)));
    }
    r.closed();
    return v;
}

OpaqueJson localMathsFromJson(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    detail::checkSchemaId(j, "maths", 1, pointer);
    r.optional("$schema");
    const std::int64_t version = asInt(r.required("schemaVersion"), r.child("schemaVersion"));
    if (version < 1) {
        violate(r.child("schemaVersion"), "schemaVersion starts at 1");
    }
    const std::string& id = asString(r.required("id"), r.child("id"));
    if (id != "local") {
        violate(r.child("id"), "the local maths module has id 'local', found '" + id + "'");
    }
    return opaqueOrViolate(j, pointer); // the module body is validated against maths-1 by esses_project
}

template <class T, class F> std::vector<T> readArray(ObjectReader& r, std::string_view key, F&& readOne) {
    std::vector<T> out;
    if (const json* a = r.optional(key)) {
        const std::string p = r.child(key);
        requireArray(*a, p);
        out.reserve(a->size());
        for (std::size_t i = 0; i < a->size(); ++i) {
            out.push_back(readOne((*a)[i], pointerIndex(p, i)));
        }
    }
    return out;
}

void checkEditUniqueness(const std::vector<MarkerEdit>& edits, const std::string& pointer) {
    std::set<std::pair<MarkerId, MarkerEditOp>> seen;
    bool distanceSeen = false;
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const MarkerEdit& e = edits[i];
        if (e.op == MarkerEditOp::OffsetDistance) {
            if (distanceSeen) {
                violate(pointerIndex(pointer, i), "a layer holds at most one offsetDistance edit");
            }
            distanceSeen = true;
        } else if (!seen.emplace(e.ref, e.op).second) {
            violate(pointerIndex(pointer, i),
                    "a second " + std::string(markerEditOpName(e.op)) + " edit for marker " + e.ref.str());
        }
    }
}

SidecarLayer layerFromJson(const json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    SidecarLayer l;
    l.details = readArray<Detail>(r, "details", detailFromJson);
    l.markers = readArray<Marker>(r, "markers", [](const json& m, const std::string& p) {
        Marker marker = detail::markerFromJson(m, p);
        if (marker.prov != Provenance::User) {
            violate(pointerChild(p, "prov"), "sidecar markers are user markers (prov User)");
        }
        return marker;
    });
    l.markerEdits = readArray<MarkerEdit>(r, "markerEdits", markerEditFromJson);
    checkEditUniqueness(l.markerEdits, r.child("markerEdits"));
    l.rangeOverrides = readArray<RangeOverride>(r, "rangeOverrides", rangeOverrideFromJson);
    std::erase_if(l.rangeOverrides, [](const RangeOverride& ov) { return !ov.hasEffect(); }); // never written
    if (const json* m = r.optional("localMaths")) {
        l.localMaths = localMathsFromJson(*m, r.child("localMaths"));
    }
    l.annotations = readArray<Annotation>(r, "annotations", annotationFromJson);
    l.videoLinks = readArray<VideoLink>(r, "videoLinks", videoLinkFromJson);
    if (const json* runs = r.optional("runs")) {
        l.runs = opaqueOrViolate(*runs, r.child("runs"));
    }
    if (const json* stages = r.optional("stages")) {
        l.stages = opaqueOrViolate(*stages, r.child("stages"));
    }
    r.closed();
    return l;
}

// ---- writing
// ------------------------------------------------------------------------------------------------------

json detailValueToJson(const DetailValue& v) {
    json j = json::object();
    if (const auto* s = std::get_if<std::string>(&v)) {
        j["t"] = "string";
        j["v"] = *s;
    } else if (const auto* q = std::get_if<Quantityd>(&v)) {
        j["t"] = "number";
        j["v"] = q->v;
        if (!q->symbol.empty()) {
            j["unit"] = q->symbol;
        }
        j["dps"] = std::clamp(q->dps, 0, 15);
    } else {
        j["t"] = "datetime";
        j["v"] = std::get<DateTime>(v).iso();
    }
    return j;
}

json layerToJson(const SidecarLayer& l) {
    json j = json::object();
    if (!l.details.empty()) {
        json a = json::array();
        for (const Detail& d : l.details) {
            json e = json::object();
            e["id"] = d.id;
            e["value"] = detailValueToJson(d.value);
            if (d.original) {
                e["original"] = detailValueToJson(*d.original);
            }
            a.push_back(std::move(e));
        }
        j["details"] = std::move(a);
    }
    if (!l.markers.empty()) {
        json a = json::array();
        for (const Marker& m : l.markers) {
            Marker u = m;
            u.prov = Provenance::User;
            a.push_back(detail::markerToJson(u));
        }
        j["markers"] = std::move(a);
    }
    if (!l.markerEdits.empty()) {
        json a = json::array();
        for (const MarkerEdit& e : l.markerEdits) {
            json o = json::object();
            o["op"] = std::string(markerEditOpName(e.op));
            switch (e.op) {
            case MarkerEditOp::Offset:
                o["ref"] = e.ref.str();
                o["dtUs"] = e.dtUs;
                break;
            case MarkerEditOp::Ignore:
                o["ref"] = e.ref.str();
                break;
            case MarkerEditOp::OffsetDistance:
                o["dM"] = json{{"v", e.dM}, {"unit", e.dMUnit.empty() ? std::string("m") : e.dMUnit}};
                break;
            case MarkerEditOp::Restore:
                continue; // never stored
            }
            a.push_back(std::move(o));
        }
        if (!a.empty()) {
            j["markerEdits"] = std::move(a);
        }
    }
    json overrides = json::array();
    for (const RangeOverride& ov : l.rangeOverrides) {
        if (!ov.hasEffect()) {
            continue;
        }
        json o = json::object();
        o["group"] = ov.groupPath;
        if (ov.startMarker) {
            o["startMarker"] = ov.startMarker->str();
        }
        o["name"] = ov.name;
        if (ov.trusted) {
            o["trusted"] = *ov.trusted;
        }
        if (ov.offsetUs) {
            o["offsetUs"] = *ov.offsetUs;
        }
        if (ov.offsetM) {
            o["offsetM"] = *ov.offsetM;
        }
        overrides.push_back(std::move(o));
    }
    if (!overrides.empty()) {
        j["rangeOverrides"] = std::move(overrides);
    }
    if (l.localMaths) {
        j["localMaths"] = detail::fromOpaque(*l.localMaths);
    }
    if (!l.annotations.empty()) {
        json a = json::array();
        for (const Annotation& an : l.annotations) {
            json o = json::object();
            o["id"] = an.id.str();
            o["tUs"] = an.tUs;
            o["text"] = an.text;
            if (an.channel) {
                o["channel"] = *an.channel;
            }
            a.push_back(std::move(o));
        }
        j["annotations"] = std::move(a);
    }
    if (!l.videoLinks.empty()) {
        json a = json::array();
        for (const VideoLink& v : l.videoLinks) {
            json sync = json::array();
            for (const auto& [logT, mediaT] : v.syncPoints) {
                sync.push_back(json::array({logT, mediaT}));
            }
            json o = json::object();
            o["file"] = v.file;
            o["cameraView"] = v.cameraView;
            o["offsetUs"] = v.offsetUs;
            o["syncPoints"] = std::move(sync);
            a.push_back(std::move(o));
        }
        j["videoLinks"] = std::move(a);
    }
    if (l.runs) {
        j["runs"] = detail::fromOpaque(*l.runs);
    }
    if (l.stages) {
        j["stages"] = detail::fromOpaque(*l.stages);
    }
    return j;
}

} // namespace

Result<SidecarDocument> SidecarDocument::parse(std::string_view text) {
    auto parsed = detail::parseJson(text);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    const json& root = *parsed;
    return detail::guardJson([&root]() -> Result<SidecarDocument> {
        ObjectReader r(root, "");
        detail::checkSchemaId(root, "sidecar", kSchemaMajor);
        r.optional("$schema");
        SidecarDocument doc;
        const std::int64_t version = asInt(r.required("schemaVersion"), r.child("schemaVersion"));
        if (version < 1 || version > 1'000'000) {
            violate(r.child("schemaVersion"), "schemaVersion must be a positive minor version");
        }
        doc.schemaVersion = static_cast<int>(version);

        ObjectReader s(r.required("source"), r.child("source"));
        doc.sourcePathRel = nonEmptyString(s.required("path"), s.child("path"), "the source path");
        doc.sourceSize = asU64(s.required("size"), s.child("size"));
        const auto hashOf = [](const json& j, const std::string& p) {
            const std::string& t = asString(j, p);
            const auto h = parseHashText(t);
            if (!h) {
                violate(p, "expected 'xxh3:' followed by 32 hex digits, found '" + t + "'");
            }
            return *h;
        };
        doc.sourceHash = hashOf(s.required("hash"), s.child("hash"));
        if (const json* ch = s.optional("contentHash")) {
            doc.sourceContentHash = hashOf(*ch, s.child("contentHash"));
        }
        ObjectReader imp(s.required("importer"), s.child("importer"));
        doc.importerId = nonEmptyString(imp.required("id"), imp.child("id"), "the importer id");
        doc.importerVersion =
            nonEmptyString(imp.required("version"), imp.child("version"), "the importer version");
        imp.closed();
        if (const json* comps = s.optional("companions")) {
            const std::string p = s.child("companions");
            requireArray(*comps, p);
            for (std::size_t i = 0; i < comps->size(); ++i) {
                std::string name = nonEmptyString((*comps)[i], pointerIndex(p, i), "a companion name");
                if (name.find_first_of("/\\") != std::string::npos) {
                    violate(pointerIndex(p, i), "a companion is a bare file name");
                }
                doc.companions.push_back(std::move(name));
            }
        }
        s.closed();

        const std::string editedPtr = r.child("edited");
        const std::string& edited = asString(r.required("edited"), editedPtr);
        const auto dt = DateTime::parseIso(edited);
        if (!dt) {
            violate(editedPtr,
                    "expected an ISO 8601 date-time with seconds and a UTC offset, found '" + edited + "'");
        }
        doc.edited = *dt;

        const std::string layersPtr = r.child("layers");
        const json& layers = r.required("layers");
        if (!layers.is_object()) {
            violate(layersPtr, "expected an object keyed by layer name");
        }
        for (auto it = layers.begin(); it != layers.end(); ++it) {
            if (it.key().empty()) {
                violate(layersPtr, "the empty layer alias is never a sidecar key (use \"Normal\")");
            }
            SidecarLayer l = layerFromJson(it.value(), pointerChild(layersPtr, it.key()));
            if (!l.empty()) { // an empty layer is never written, so it is not kept either
                doc.layers_.emplace(it.key(), std::move(l));
            }
        }

        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!isKnownTopLevel(it.key())) {
                doc.unknown.emplace(it.key(), opaqueOrViolate(it.value(), pointerChild("", it.key())));
            }
        }
        return doc;
    });
}

Result<std::string> SidecarDocument::toJson() const {
    json root = json::object();
    root["$schema"] = std::string(kSchemaId);
    root["schemaVersion"] = std::max(schemaVersion, 1);
    json source = json::object();
    source["path"] = sourcePathRel;
    source["size"] = sourceSize;
    source["hash"] = hashText(sourceHash);
    if (sourceContentHash) {
        source["contentHash"] = hashText(*sourceContentHash);
    }
    source["importer"] = json{{"id", importerId}, {"version", importerVersion}};
    if (!companions.empty()) {
        source["companions"] = companions;
    }
    root["source"] = std::move(source);
    root["edited"] = edited.iso();
    json layers = json::object();
    for (const auto& [name, l] : layers_) {
        if (!l.empty()) {
            layers[name] = layerToJson(l);
        }
    }
    root["layers"] = std::move(layers);
    for (const auto& [key, value] : unknown) {
        if (!isKnownTopLevel(key)) {
            root[key] = detail::fromOpaque(value);
        }
    }
    return detail::canonicalJson(root);
}

} // namespace esses::model
