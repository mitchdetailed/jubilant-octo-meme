// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/atomic_file.hpp>
#include <esses/base/casefold.hpp>
#include <esses/base/utf8.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/sidecar.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace esses::model {

namespace fs = std::filesystem;

// ---- small types
// ------------------------------------------------------------------------------------------------

bool SidecarLayer::empty() const noexcept {
    return details.empty() && markers.empty() && markerEdits.empty() && rangeOverrides.empty() &&
           !localMaths && annotations.empty() && videoLinks.empty() && !runs && !stages;
}

void SidecarApplyReport::append(SidecarApplyReport other) {
    notes.insert(notes.end(), std::make_move_iterator(other.notes.begin()),
                 std::make_move_iterator(other.notes.end()));
    heals.insert(heals.end(), other.heals.begin(), other.heals.end());
}

std::string hashText(const Hash128& h) {
    return "xxh3:" + h.hex();
}

std::optional<Hash128> parseHashText(std::string_view text) noexcept {
    constexpr std::string_view kPrefix = "xxh3:";
    if (!text.starts_with(kPrefix) || text.size() != kPrefix.size() + 32) {
        return std::nullopt;
    }
    text.remove_prefix(kPrefix.size());
    const auto word = [](std::string_view hex, std::uint64_t& out) {
        out = 0;
        for (const char c : hex) {
            int digit = -1;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            } else {
                return false;
            }
            out = (out << 4) | static_cast<std::uint64_t>(digit);
        }
        return true;
    };
    Hash128 h;
    if (!word(text.substr(0, 16), h.hi) || !word(text.substr(16, 16), h.lo)) {
        return std::nullopt;
    }
    return h;
}

// ---- files
// --------------------------------------------------------------------------------------------------------

fs::path SidecarDocument::pathFor(const fs::path& log) {
    fs::path p = log;
    p += ".sidecar.json";
    return p;
}

Result<SidecarDocument> SidecarDocument::load(const fs::path& path) {
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot stat sidecar: " + ec.message(), std::move(ctx));
    }
    if (!exists) {
        return SidecarDocument{};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot open sidecar for reading", std::move(ctx));
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot read sidecar", std::move(ctx));
    }
    auto doc = parse(text);
    if (!doc) {
        Error e = doc.error();
        e.context.path = path;
        return Failure{std::move(e)};
    }
    return doc;
}

Result<void> SidecarDocument::save(const fs::path& path) const {
    if (empty()) {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            ErrorContext ctx;
            ctx.path = path;
            return fail(ErrorCode::Io, "cannot delete the empty sidecar: " + ec.message(), std::move(ctx));
        }
        return ok();
    }
    auto text = toJson();
    if (!text) {
        Error e = text.error();
        e.context.path = path;
        return Failure{std::move(e)};
    }
    return AtomicFile::write(path, std::string_view(*text));
}

Result<Hash128> SidecarDocument::documentHash() const {
    auto text = toJson();
    if (!text) {
        return Failure{text.error()};
    }
    return hash128(std::string_view(*text));
}

// ---- layers
// ---------------------------------------------------------------------------------------------------

const SidecarLayer& SidecarDocument::layer(std::string_view name) const {
    static const SidecarLayer kEmpty;
    const auto it = layers_.find(canonicalLayerName(name));
    return it == layers_.end() ? kEmpty : it->second;
}

SidecarLayer& SidecarDocument::layer(std::string_view name) {
    const std::string_view key = canonicalLayerName(name);
    auto it = layers_.find(key);
    if (it == layers_.end()) {
        it = layers_.emplace(std::string(key), SidecarLayer{}).first;
    }
    return it->second;
}

bool SidecarDocument::hasLayer(std::string_view name) const {
    return layers_.find(canonicalLayerName(name)) != layers_.end();
}

void SidecarDocument::pruneEmptyLayers() {
    for (auto it = layers_.begin(); it != layers_.end();) {
        it = it->second.empty() ? layers_.erase(it) : std::next(it);
    }
}

bool SidecarDocument::empty() const noexcept {
    if (!unknown.empty()) {
        return false;
    }
    return std::all_of(layers_.begin(), layers_.end(), [](const auto& kv) { return kv.second.empty(); });
}

// ---- application
// ----------------------------------------------------------------------------------------------

namespace {

ModelNote makeNote(ModelNote::Level level, std::string_view code, std::string message, std::string_view layer,
                   std::string ref) {
    return ModelNote{level, std::string(code), std::move(message), std::string(layer), std::move(ref)};
}

// The markers a layer had before any sidecar application: user markers dropped, edits undone. Exact because
// the sidecar is the only source of edits (offset sets originalT, ignore clears enabled).
std::vector<Marker> unapplied(const std::vector<Marker>& markers) {
    std::vector<Marker> out;
    out.reserve(markers.size());
    for (const Marker& m : markers) {
        if (m.prov == Provenance::User) {
            continue;
        }
        Marker b = m;
        b.t = m.loggedT();
        b.originalT.reset();
        b.enabled = true;
        out.push_back(std::move(b));
    }
    sortMarkers(out);
    return out;
}

Range* matchOverride(RangeGroup& group, const RangeOverride& ov, bool& byName) {
    byName = false;
    if (ov.startMarker) {
        Range* first = nullptr;
        for (Range& r : group.ranges) {
            if (r.startMarker == *ov.startMarker) {
                if (first == nullptr) {
                    first = &r;
                }
                if (r.name == ov.name) {
                    return &r; // several ranges share an anchor (sections of one lap): the name decides
                }
            }
        }
        if (first != nullptr) {
            return first;
        }
    }
    if (ov.name.empty()) {
        return nullptr;
    }
    for (Range& r : group.ranges) {
        if (r.name == ov.name) {
            byName = true;
            return &r;
        }
    }
    for (Range& r : group.ranges) {
        if (ciEquals(r.name, ov.name)) {
            byName = true;
            return &r;
        }
    }
    return nullptr;
}

} // namespace

SidecarApplyReport SidecarDocument::applyMarkers(DataLayer& target) const {
    SidecarApplyReport report;
    if (target.baseMarkers.empty() && !target.markers.empty()) {
        target.baseMarkers = unapplied(target.markers);
    }
    const SidecarLayer& edits = layer(target.name);
    std::vector<Marker> markers = target.baseMarkers;
    report.notes = applyMarkerEdits(markers, edits.markerEdits, canonicalLayerName(target.name));
    for (const Marker& u : edits.markers) {
        Marker m = u;
        m.prov = Provenance::User;
        markers.push_back(std::move(m));
    }
    sortMarkers(markers);
    target.markers = std::move(markers);
    return report;
}

SidecarApplyReport SidecarDocument::applyRangeOverrides(DataLayer& target) const {
    SidecarApplyReport report;
    const std::string_view layerName = canonicalLayerName(target.name);
    const SidecarLayer& edits = layer(layerName);
    for (std::size_t i = 0; i < edits.rangeOverrides.size(); ++i) {
        const RangeOverride& ov = edits.rangeOverrides[i];
        if (!ov.hasEffect()) {
            continue;
        }
        RangeGroup* group = target.ranges.find(ov.groupPath);
        bool byName = false;
        Range* r = group == nullptr ? nullptr : matchOverride(*group, ov, byName);
        const std::string key = ov.groupPath + "|" + (ov.startMarker ? ov.startMarker->str() : ov.name);
        if (r == nullptr) {
            report.notes.push_back(makeNote(ModelNote::Level::Warning, kNoteOrphanRangeOverride,
                                            "range override '" + ov.name + "' in '" + ov.groupPath +
                                                "' matches no range; it is kept",
                                            layerName, key));
            continue;
        }
        if (ov.trusted) {
            r->trusted = *ov.trusted;
        }
        if (ov.offsetUs) {
            r->offsetUs = *ov.offsetUs;
        }
        if (ov.offsetM) {
            r->offsetM = *ov.offsetM;
        }
        if (byName) {
            report.notes.push_back(
                makeNote(ModelNote::Level::Info, kNoteRangeOverrideByName,
                         "range override '" + ov.name + "' in '" + ov.groupPath + "' matched by name",
                         layerName, key));
            if (!r->startMarker.isNil()) {
                report.heals.push_back(RangeOverrideHeal{std::string(layerName), i, r->startMarker});
            }
        }
    }
    return report;
}

SidecarApplyReport SidecarDocument::apply(DataLayer& target) const {
    SidecarApplyReport report = applyMarkers(target);
    report.append(applyRangeOverrides(target));
    return report;
}

void SidecarDocument::applyHeals(const SidecarApplyReport& report) {
    for (const RangeOverrideHeal& h : report.heals) {
        if (!hasLayer(h.layer)) {
            continue;
        }
        SidecarLayer& l = layer(h.layer);
        if (h.index < l.rangeOverrides.size()) {
            l.rangeOverrides[h.index].startMarker = h.startMarker;
        }
    }
}

std::vector<ModelNote> SidecarDocument::checkSource(const SourceId& quickHash,
                                                    const std::optional<Hash128>& contentHash) const {
    std::vector<ModelNote> notes;
    if (empty()) {
        return notes;
    }
    if (sourceHash != quickHash) {
        notes.push_back(makeNote(ModelNote::Level::Warning, kNoteSourceChanged,
                                 "the sidecar was written for " + hashText(sourceHash) + ", the file is " +
                                     hashText(quickHash) + "; the edits are applied anyway",
                                 {}, sourcePathRel));
    } else if (sourceContentHash && contentHash && *sourceContentHash != *contentHash) {
        notes.push_back(makeNote(ModelNote::Level::Warning, kNoteSourceChanged,
                                 "the file content changed since the sidecar was written (" +
                                     hashText(*sourceContentHash) + " vs " + hashText(*contentHash) +
                                     "); the edits are applied anyway",
                                 {}, sourcePathRel));
    }
    return notes;
}

// ---- edit commands
// ----------------------------------------------------------------------------------------------

Result<void> SidecarDocument::applyEdit(std::string_view layerName, const MarkerEdit& edit) {
    if (edit.op != MarkerEditOp::OffsetDistance && edit.ref.isNil()) {
        return fail(ErrorCode::Invalid, std::string(markerEditOpName(edit.op)) + " edit without a marker id");
    }
    if (edit.op == MarkerEditOp::OffsetDistance && !std::isfinite(edit.dM)) {
        return fail(ErrorCode::Invalid, "offsetDistance edit with a non-finite distance");
    }
    std::vector<MarkerEdit>& edits = layer(layerName).markerEdits;
    const auto sameKind = [&edit](const MarkerEdit& e) {
        return e.op == edit.op && (edit.op == MarkerEditOp::OffsetDistance || e.ref == edit.ref);
    };
    switch (edit.op) {
    case MarkerEditOp::Offset:
    case MarkerEditOp::OffsetDistance: {
        const bool zero = edit.op == MarkerEditOp::Offset ? edit.dtUs == 0 : edit.dM == 0.0;
        const auto it = std::find_if(edits.begin(), edits.end(), sameKind);
        if (zero) {
            if (it != edits.end()) {
                edits.erase(it);
            }
        } else if (it != edits.end()) {
            *it = edit;
        } else {
            edits.push_back(edit);
        }
        break;
    }
    case MarkerEditOp::Ignore:
        if (std::none_of(edits.begin(), edits.end(), sameKind)) {
            MarkerEdit e;
            e.ref = edit.ref;
            e.op = MarkerEditOp::Ignore;
            edits.push_back(e);
        }
        break;
    case MarkerEditOp::Restore:
        std::erase_if(edits, [&edit](const MarkerEdit& e) {
            return e.op != MarkerEditOp::OffsetDistance && e.ref == edit.ref;
        });
        break;
    }
    return ok();
}

void SidecarDocument::setRangeOverride(std::string_view layerName, RangeOverride ov) {
    std::vector<RangeOverride>& list = layer(layerName).rangeOverrides;
    auto it = list.end();
    if (ov.startMarker) {
        it = std::find_if(list.begin(), list.end(), [&ov](const RangeOverride& e) {
            return e.groupPath == ov.groupPath && e.startMarker == ov.startMarker;
        });
    }
    if (it == list.end()) {
        it = std::find_if(list.begin(), list.end(), [&ov](const RangeOverride& e) {
            return e.groupPath == ov.groupPath && e.name == ov.name && (!e.startMarker || !ov.startMarker);
        });
    }
    if (!ov.hasEffect()) {
        if (it != list.end()) {
            list.erase(it);
        }
        return;
    }
    if (it != list.end()) {
        *it = std::move(ov);
    } else {
        list.push_back(std::move(ov));
    }
}

void SidecarDocument::setDetail(std::string_view layerName, Detail d) {
    d.prov = Provenance::User;
    std::vector<Detail>& list = layer(layerName).details;
    if (Detail* existing = findDetail(list, d.id)) {
        *existing = std::move(d);
    } else {
        list.push_back(std::move(d));
    }
}

bool SidecarDocument::revertDetail(std::string_view layerName, std::string_view id) {
    if (!hasLayer(layerName)) {
        return false;
    }
    std::vector<Detail>& list = layer(layerName).details;
    const Detail* d = findDetail(list, id);
    if (d == nullptr) {
        return false;
    }
    list.erase(list.begin() + (d - list.data()));
    return true;
}

MarkerId SidecarDocument::addUserMarker(std::string_view layerName, Marker m) {
    m.prov = Provenance::User;
    if (m.id.isNil()) {
        m.id = MarkerId{uuidRandom()};
    }
    const MarkerId id = m.id;
    layer(layerName).markers.push_back(std::move(m));
    return id;
}

bool SidecarDocument::removeUserMarker(std::string_view layerName, const MarkerId& id) {
    if (!hasLayer(layerName)) {
        return false;
    }
    return std::erase_if(layer(layerName).markers, [&id](const Marker& m) { return m.id == id; }) != 0;
}

} // namespace esses::model
