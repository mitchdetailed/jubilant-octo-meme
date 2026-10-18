// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataSource (02 section 4.2; D-015, D-031, D-130) and the merge steps of the open pipeline (05 section 2
// step 5): file details and markers, the companion (.ldx), then the sidecar, with precedence
// sidecar > companion > file.
//
// Build order (worker, before publication):
//   1. the importer fills id, prov, details (prov Logged) and every layer's channels and markers (file order,
//      ids from markerIdLogged, SOL/EOL synthesised with prov Derived when the file lacks them);
//   2. mergeCompanion(src, companion) once (an empty CompanionData when there is none): sets baseDetails and
//      each layer's baseMarkers;
//   3. applySidecar(src, doc): details and markers with the sidecar's edits (repeatable, always from the
//   base);
//   4. the phase-1 rules build each layer's ranges (M0: rebuildWholeOutingRanges, whole_outing.hpp), then
//      SidecarDocument::applyRangeOverrides.
// A published DataSource is immutable (std::shared_ptr<const DataSource>); an edit produces a new snapshot
// through withSidecar() with version + 1 that shares the channels and their columns.
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/notes.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc
#include <esses/model/sidecar.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::model {

struct SourceProvenance { // 02 section 4.2 Provenance_Source
    std::filesystem::path path;
    std::vector<std::filesystem::path> companions;
    std::string importerId;
    std::string importerVersion;
    std::uint64_t size = 0;
    SourceId hash;                      // quick hash
    std::optional<Hash128> contentHash; // full xxh3-128, filled by a Background task
    std::int64_t mtimeUnixUs = 0;
    std::vector<std::string> dialectFlags;
    JsonDoc probe;                    // probe_json (probe-1): sniff/probe/open entries only
    std::vector<std::byte> rawHeader; // the importer's verbatim header bytes for a writer round trip (.ld:
                                      // 0x5DE..0x5E1 held as 0, D-143); never serialised
};
using Provenance_Source = SourceProvenance; // the 02 spelling

// What the companion reader reports (05 section 4): details (prov Companion), markers per layer name (""
// accepted for "Normal"; ids from markerIdLogged) and the unknown elements kept for the export writer.
struct CompanionData {
    std::vector<Detail> details;
    std::map<std::string, std::vector<Marker>, std::less<>> markers;
    std::vector<std::string> opaque;
};

struct DataSource {
    SourceId id;
    SourceProvenance prov;
    std::vector<Detail> details;     // merged file + companion + sidecar ("Normal" layer edits)
    std::vector<Detail> baseDetails; // file + companion, before the sidecar (addition to 02 section 4.2)
    std::vector<DataLayer> layers;   // layers[0].name == "Normal"
    SidecarDocument sidecar;         // the edits (owned copy of the document state)
    std::vector<std::string> companionOpaque; // unknown .ldx elements as XML strings (export round trip)
    std::uint64_t version = 0;                // bumps on every sidecar commit

    const DataLayer* layer(std::string_view name) const; // "" accepted as an alias of "Normal"
    DataLayer* layer(std::string_view name);

    // Phase-1 range rebuild of a whole source: runs the rules on every layer and applies the sidecar's range
    // overrides. The default (an empty function) is rebuildWholeOutingRanges, the only rule of M0;
    // esses_analysis passes its RuleRunner from M1 on.
    using RangeRebuild = std::function<SidecarApplyReport(DataSource&)>;

    // A new snapshot with `doc` applied (details and markers rebuilt from the base, then `rebuild`; channels
    // and their columns shared), version + 1. Range-group versions are inherited from this snapshot, so only
    // changed group paths bump (D-140). `report` receives the notes when non-null.
    std::shared_ptr<const DataSource> withSidecar(SidecarDocument doc, SidecarApplyReport* report = nullptr,
                                                  const RangeRebuild& rebuild = {}) const;
};

// Step 2. Details: file then companion (companion values win, prov Companion, the file value becomes
// `original`). Markers: per layer, mergeMarkers(file, companion). Companion markers of a layer the source
// lacks raise kNoteCompanionUnknownLayer. Sets baseDetails / baseMarkers and companionOpaque.
std::vector<ModelNote> mergeCompanion(DataSource& src, CompanionData companion);

// Step 3. Checks the source block (sidecar.source_changed), sets details = baseDetails overlaid with the
// "Normal" layer's sidecar details, rebuilds every layer's markers through doc.applyMarkers, and stores the
// document in src.sidecar. Range overrides are not applied here (the rules have not run yet).
SidecarApplyReport applySidecar(DataSource& src, SidecarDocument doc);

// The derived File group (02 section 4.2 behaviour 2): "File Name", "File Path" (text) and "File Size"
// (count) from prov.path and prov.size, prov Derived; replaces earlier File details. Never written to the
// sidecar.
void deriveFileDetails(DataSource& src);

// A new sidecar for this source: source path relative to the sidecar's directory (the file name), size,
// quick hash, content hash when known, importer id and version, companion file names; edited = now (UTC).
SidecarDocument newSidecarFor(const DataSource& src);

} // namespace esses::model
