// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SidecarDocument (02 section 5; D-015, D-065, D-069, D-130): the per-log edit file
// "<log>.<ext>.sidecar.json" (schema sidecar-1). It holds the user's edits per data layer: details, user
// markers, marker edits, range overrides, local maths, annotations and video links, plus the source block
// that identifies the log. Esses never touches the log or its companion.
//
// Persistence. load() is lenient where the form does not matter (UUIDs in either case, null for an absent
// optional, the bare "esses:sidecar-1" and https $schema forms) and strict where it does (types, required
// members, enumerations, closed records); toJson() writes the canonical form of 02 section 6.5 rule 1, so
// load(canonical) -> toJson() reproduces the bytes. Unknown top-level members are kept verbatim and
// re-emitted; nested records are closed. save() writes atomically (AtomicFile) and deletes the file when the
// document is empty().
//
// Application. apply(layer) rebuilds layer.markers from layer.baseMarkers: marker edits, then the user
// markers, then model order (02 section 5.3 rule 2); applyRangeOverrides(layer) then stamps trusted and the
// alignment offsets on the ranges the rules produced, matching (group, startMarker) first and (group, name)
// second (rule 5). Nothing is dropped: orphaned edits and overrides stay in the document and are reported.
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/marker.hpp>
#include <esses/model/notes.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::model {

struct DataLayer;

struct Annotation {
    Uuid id;        // uuidRandom() when created
    Micros tUs = 0; // absolute Main-source time since the layer origin (rule 6)
    std::string text;
    std::optional<std::string> channel; // the channel the note is about

    friend bool operator==(const Annotation&, const Annotation&) = default;
};

struct VideoLink {
    std::string file;       // media path relative to the sidecar's directory, UTF-8 as written
    std::string cameraView; // "Driver"
    Micros offsetUs = 0;
    std::vector<std::pair<Micros, Micros>> syncPoints; // (log t, media pts)

    friend bool operator==(const VideoLink&, const VideoLink&) = default;
};

struct RangeOverride {
    std::string groupPath;               // "Outings:Laps" (JSON key "group")
    std::optional<MarkerId> startMarker; // primary key with groupPath
    std::string name;                    // fallback key ("Lap 3"), also used by workspace imports
    std::optional<bool> trusted;
    std::optional<Micros> offsetUs;
    std::optional<double> offsetM;

    // True when the override sets anything; an override without effect is removed instead of written.
    bool hasEffect() const noexcept {
        return trusted.has_value() || offsetUs.has_value() || offsetM.has_value();
    }
    friend bool operator==(const RangeOverride&, const RangeOverride&) = default;
};

struct SidecarLayer {
    std::vector<Detail> details;         // prov == User only
    std::vector<Marker> markers;         // prov == User only
    std::vector<MarkerEdit> markerEdits; // offset / ignore per marker id, offsetDistance per layer
    std::vector<RangeOverride> rangeOverrides;
    std::optional<JsonDoc> localMaths; // a complete maths-1 module with id "local"
    std::vector<Annotation> annotations;
    std::vector<VideoLink> videoLinks;
    std::optional<JsonDoc> runs;   // reserved (drag profile, v1.0); preserved verbatim
    std::optional<JsonDoc> stages; // reserved (rally profile, v1.0); preserved verbatim

    bool empty() const noexcept;
    friend bool operator==(const SidecarLayer&, const SidecarLayer&) = default;
};

// A range override matched by name whose start marker the next save should record (02 section 5.3 rule 5).
struct RangeOverrideHeal {
    std::string layer;
    std::size_t index = 0; // position in SidecarLayer::rangeOverrides
    MarkerId startMarker;

    friend bool operator==(const RangeOverrideHeal&, const RangeOverrideHeal&) = default;
};

struct SidecarApplyReport {
    std::vector<ModelNote> notes;
    std::vector<RangeOverrideHeal> heals;

    bool has(std::string_view code) const noexcept { return containsNote(notes, code); }
    void append(SidecarApplyReport other);
};

class SidecarDocument {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:sidecar-1";
    static constexpr int kSchemaMajor = 1;
    static constexpr int kSchemaVersion = 1; // newest minor this build writes and understands

    // "<log>.<ext>.sidecar.json" next to the log.
    static std::filesystem::path pathFor(const std::filesystem::path& log);

    // A missing file is an empty document (ok). Errors: Io (context.path), Format (JSON syntax;
    // context.offset), Unsupported (another schema major; context.schemaId), Invalid (a sidecar-1 rule;
    // context.detail = JSON pointer of the offending member). A caller must not save over a file that failed
    // to load.
    static Result<SidecarDocument> load(const std::filesystem::path& path);
    static Result<SidecarDocument> parse(std::string_view json);

    // Canonical text (02 section 6.5 rule 1, trailing LF). Errors: Invalid when a number is not finite.
    Result<std::string> toJson() const;
    // Atomic write of toJson(); an empty() document deletes `path` instead (D-069).
    Result<void> save(const std::filesystem::path& path) const;
    // xxh3-128 of toJson(): the journal's baseHash / hash of this document (D-070).
    Result<Hash128> documentHash() const;

    // ---- application --------------------------------------------------------------------------------------
    // layer.markers = baseMarkers + marker edits + user markers, in model order; then applyRangeOverrides.
    SidecarApplyReport apply(DataLayer& layer) const;
    SidecarApplyReport applyMarkers(DataLayer& layer) const;
    SidecarApplyReport applyRangeOverrides(DataLayer& layer) const;
    // Writes the start markers that a name match found into the overrides (rule 5 self-healing).
    void applyHeals(const SidecarApplyReport& report);
    // sidecar.source_changed when the quick hash differs, or when both content hashes are known and differ.
    std::vector<ModelNote> checkSource(const SourceId& quickHash,
                                       const std::optional<Hash128>& contentHash) const;

    // ---- edit commands ------------------------------------------------------------------------------------
    // Offset: sets the marker's offset edit (dtUs == 0 removes it). Ignore: adds the ignore edit. Restore:
    // removes every edit of the marker (it is a command, never a record). OffsetDistance: sets the layer's
    // one offsetDistance edit (dM == 0 removes it). Errors: Invalid for a nil ref (Offset/Ignore/Restore) or
    // a non-finite dM.
    Result<void> applyEdit(std::string_view layerName, const MarkerEdit& edit);
    // Inserts or replaces the override with the same (group, startMarker), else (group, name); one without
    // effect removes the matching entry.
    void setRangeOverride(std::string_view layerName, RangeOverride ov);
    // Sets (or replaces, by id) a user detail; prov is forced to User.
    void setDetail(std::string_view layerName, Detail d);
    bool revertDetail(std::string_view layerName, std::string_view id);
    // Adds a user marker (prov forced to User; a nil id gets uuidRandom()). Returns its id.
    MarkerId addUserMarker(std::string_view layerName, Marker m);
    bool removeUserMarker(std::string_view layerName, const MarkerId& id);
    // M2 (02 section 7, D-032 undoable sidecar edits): inserts or replaces (by id) an annotation; a nil id
    // gets uuidRandom(). Returns its id.
    Uuid setAnnotation(std::string_view layerName, Annotation a);
    bool removeAnnotation(std::string_view layerName, const Uuid& id);
    // Replaces the layer's local maths (a complete maths-1 module with id "local"); nullopt removes it.
    void setLocalMaths(std::string_view layerName, std::optional<JsonDoc> module);
    // "Restore logged laps" (06 section 13.3 item 6): removes every marker edit and user marker of the layer
    // (one command). Returns whether anything was removed.
    bool restoreLogged(std::string_view layerName);

    // ---- layers -------------------------------------------------------------------------------------------
    // "" is an alias of "Normal". The const overload returns an empty layer when absent.
    const SidecarLayer& layer(std::string_view name) const;
    SidecarLayer& layer(std::string_view name);
    bool hasLayer(std::string_view name) const;
    const std::map<std::string, SidecarLayer, std::less<>>& layers() const noexcept { return layers_; }
    // Drops layers without content (they are never written anyway).
    void pruneEmptyLayers();

    // True when no layer has content and no unknown top-level member exists: save() deletes the file.
    bool empty() const noexcept;

    // ---- source block and metadata ------------------------------------------------------------------------
    std::string sourcePathRel; // log path relative to the sidecar's directory (UTF-8)
    std::uint64_t sourceSize = 0;
    SourceId sourceHash;                      // quick hash (D-130)
    std::optional<Hash128> sourceContentHash; // full hash, written once the Background task produced it
    std::string importerId;
    std::string importerVersion;
    std::vector<std::string> companions;                 // companion file names found beside the log
    DateTime edited;                                     // time of the last committed edit
    int schemaVersion = kSchemaVersion;                  // as read; a newer minor is kept on save
    std::map<std::string, JsonDoc, std::less<>> unknown; // unknown top-level members, x-esses-legacy included

    friend bool operator==(const SidecarDocument&, const SidecarDocument&) = default;

private:
    std::map<std::string, SidecarLayer, std::less<>> layers_;
};

// "xxh3:" + 32 lower-case hex digits (the sidecar-1 / probe-1 hash form) and its inverse.
std::string hashText(const Hash128& h);
std::optional<Hash128> parseHashText(std::string_view text) noexcept;

} // namespace esses::model
