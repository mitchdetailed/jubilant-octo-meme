// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/uuid.hpp>
#include <esses/model/marker.hpp>

#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace esses::model {

namespace {

constexpr MarkerClass kAllClasses[] = {MarkerClass::SOL,     MarkerClass::EOL,     MarkerClass::BCN,
                                       MarkerClass::SPLTBCN, MarkerClass::IGRDBCN, MarkerClass::RESET,
                                       MarkerClass::STGSTRT, MarkerClass::STGEND,  MarkerClass::DeviceEvent};

ModelNote note(ModelNote::Level level, std::string_view code, std::string message, std::string_view layer,
               std::string ref) {
    return ModelNote{level, std::string(code), std::move(message), std::string(layer), std::move(ref)};
}

} // namespace

std::string_view markerClassName(MarkerClass c) noexcept {
    switch (c) {
    case MarkerClass::SOL:
        return "SOL";
    case MarkerClass::EOL:
        return "EOL";
    case MarkerClass::BCN:
        return "BCN";
    case MarkerClass::SPLTBCN:
        return "SPLTBCN";
    case MarkerClass::IGRDBCN:
        return "IGRDBCN";
    case MarkerClass::RESET:
        return "RESET";
    case MarkerClass::STGSTRT:
        return "STGSTRT";
    case MarkerClass::STGEND:
        return "STGEND";
    case MarkerClass::DeviceEvent:
        return "DeviceEvent";
    }
    return "SOL";
}

std::optional<MarkerClass> parseMarkerClass(std::string_view text) noexcept {
    for (const MarkerClass c : kAllClasses) {
        if (text == markerClassName(c)) {
            return c;
        }
    }
    return std::nullopt;
}

std::string_view markerEditOpName(MarkerEditOp op) noexcept {
    switch (op) {
    case MarkerEditOp::Offset:
        return "offset";
    case MarkerEditOp::Ignore:
        return "ignore";
    case MarkerEditOp::Restore:
        return "restore";
    case MarkerEditOp::OffsetDistance:
        return "offsetDistance";
    }
    return "offset";
}

std::optional<MarkerEditOp> parseMarkerEditOp(std::string_view text) noexcept {
    for (const MarkerEditOp op :
         {MarkerEditOp::Offset, MarkerEditOp::Ignore, MarkerEditOp::Restore, MarkerEditOp::OffsetDistance}) {
        if (text == markerEditOpName(op)) {
            return op;
        }
    }
    return std::nullopt;
}

MarkerId markerIdLogged(MarkerClass cls, Micros loggedT, std::string_view group, std::uint32_t ordinal) {
    const std::string t = std::to_string(loggedT);
    const std::string ord = std::to_string(ordinal);
    return MarkerId{uuidFromHash(hash128({"marker", markerClassName(cls), t, group, ord}))};
}

namespace {

// The tie rank at equal times: an outing's EOL first, then the next outing's SOL, then the other classes in
// declaration order (D-158), so an outing that ends where the next one starts never yields an empty outing.
int tieRank(MarkerClass c) noexcept {
    switch (c) {
    case MarkerClass::EOL:
        return 0;
    case MarkerClass::SOL:
        return 1;
    default:
        return 2 + static_cast<int>(c);
    }
}

} // namespace

bool markerOrderLess(const Marker& a, const Marker& b) noexcept {
    if (a.t != b.t) {
        return a.t < b.t;
    }
    if (a.cls != b.cls) {
        return tieRank(a.cls) < tieRank(b.cls);
    }
    return a.id < b.id;
}

void sortMarkers(std::vector<Marker>& markers) {
    std::stable_sort(markers.begin(), markers.end(), markerOrderLess);
}

void assignLoggedIds(std::vector<Marker>& markers) {
    std::map<std::tuple<MarkerClass, std::string, Micros>, std::uint32_t> ordinals;
    for (Marker& m : markers) {
        if (m.prov == Provenance::User) {
            continue;
        }
        std::uint32_t& next = ordinals[std::make_tuple(m.cls, m.group, m.loggedT())];
        m.id = markerIdLogged(m.cls, m.loggedT(), m.group, next);
        ++next;
    }
}

Marker makeLoggedMarker(MarkerClass cls, Micros t, std::string group, std::string name, std::string comment,
                        std::uint32_t flags, std::uint32_t ordinal, Provenance prov) {
    Marker m;
    m.id = markerIdLogged(cls, t, group, ordinal);
    m.cls = cls;
    m.group = std::move(group);
    m.t = t;
    m.name = std::move(name);
    m.comment = std::move(comment);
    m.prov = prov;
    m.flags = flags;
    return m;
}

std::string deviceEventName(std::uint32_t code) {
    switch (code) {
    case 0:
        return "Firmware start";
    case 1:
        return "Firmware stop";
    case 2:
        return "Power cycled";
    case 3:
        return "Logging on";
    case 4:
        return "Logging off";
    case 5:
        return "Log discontinuity";
    default:
        return "Event " + std::to_string(code);
    }
}

Marker makeDeviceEventMarker(std::uint32_t code, Micros t, std::uint32_t ordinal) {
    return makeLoggedMarker(MarkerClass::DeviceEvent, t, std::string(kGroupDeviceEvents),
                            deviceEventName(code), {}, code, ordinal, Provenance::Logged);
}

std::vector<Marker> mergeMarkers(std::vector<Marker> file, const std::vector<Marker>& companion,
                                 std::vector<ModelNote>* notes, std::string_view layer) {
    std::vector<Marker> out;
    out.reserve(file.size() + companion.size());
    std::unordered_map<MarkerId, std::size_t> index;
    for (Marker& m : file) {
        if (!index.emplace(m.id, out.size()).second) {
            if (notes != nullptr) {
                notes->push_back(
                    note(ModelNote::Level::Warning, kNoteDuplicateMarkerId,
                         "the file lists marker " + m.id.str() + " twice; the later one is dropped", layer,
                         m.id.str()));
            }
            continue;
        }
        out.push_back(std::move(m));
    }
    std::unordered_set<MarkerId> companionSeen;
    for (const Marker& c : companion) {
        if (!companionSeen.insert(c.id).second) {
            if (notes != nullptr) {
                notes->push_back(
                    note(ModelNote::Level::Warning, kNoteDuplicateMarkerId,
                         "the companion lists marker " + c.id.str() + " twice; the later one is dropped",
                         layer, c.id.str()));
            }
            continue;
        }
        Marker merged = c;
        merged.prov = Provenance::Companion;
        if (const auto it = index.find(c.id); it != index.end()) {
            out[it->second] = std::move(merged); // identical ids collapse to the companion's marker
        } else {
            index.emplace(c.id, out.size());
            out.push_back(std::move(merged));
        }
    }
    sortMarkers(out);
    return out;
}

std::vector<ModelNote> applyMarkerEdits(std::vector<Marker>& markers, std::span<const MarkerEdit> edits,
                                        std::string_view layer) {
    std::vector<ModelNote> notes;
    std::unordered_map<MarkerId, std::size_t> index;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        index.emplace(markers[i].id, i);
    }
    for (const MarkerEdit& e : edits) {
        if (e.op == MarkerEditOp::OffsetDistance) {
            notes.push_back(note(ModelNote::Level::Info, kNoteOffsetDistancePending,
                                 "beacon offset by distance waits for Corr Dist", layer, {}));
            continue;
        }
        if (e.op == MarkerEditOp::Restore) {
            continue; // a command on the document, never applied as a record
        }
        const auto it = index.find(e.ref);
        if (it == index.end()) {
            notes.push_back(note(ModelNote::Level::Warning, kNoteOrphanMarkerEdit,
                                 std::string(markerEditOpName(e.op)) + " edit references marker " +
                                     e.ref.str() + ", which this layer does not have; the edit is kept",
                                 layer, e.ref.str()));
            continue;
        }
        Marker& m = markers[it->second];
        if (e.op == MarkerEditOp::Offset) {
            const Micros logged = m.loggedT();
            m.originalT = logged;
            m.t = saturatingAdd(logged, e.dtUs);
        } else {
            m.enabled = false;
        }
    }
    return notes;
}

Hash128 markerSetHash(std::span<const Marker> markers) {
    Hash128Stream s;
    s.updateU64(markers.size());
    for (const Marker& m : markers) {
        s.update(std::as_bytes(std::span<const std::uint8_t>(m.id.v.b)));
        s.updateU64(static_cast<std::uint64_t>(m.cls));
        s.updatePart(m.group);
        s.updateU64(static_cast<std::uint64_t>(m.t));
        s.updateU64(m.enabled ? 1u : 0u);
    }
    return s.digest();
}

} // namespace esses::model
