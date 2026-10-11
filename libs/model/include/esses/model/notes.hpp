// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ModelNote: a diagnostic raised while the model merges a companion, applies a sidecar or runs a rule. The
// open pipeline (esses_formats, layer 2) copies each note into the source's ImportLog under the same code, so
// esses_model does not depend on the ImportLog type (01 section 2.2). Codes are stable ids; the ones raised
// here are listed below and in libs/model/README.md.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esses::model {

struct ModelNote {
    enum class Level : std::uint8_t { Info, Warning, Error };
    Level level = Level::Info;
    std::string code;    // stable id, e.g. "sidecar.orphan_marker_edit"
    std::string message; // UTF-8, human readable
    std::string layer;   // the data layer concerned ("" when the note is source-wide)
    std::string ref;     // the id or key concerned (marker id, "group|name"), "" when none

    friend bool operator==(const ModelNote&, const ModelNote&) = default;
};

std::string_view modelNoteLevelName(ModelNote::Level level) noexcept; // "Info", "Warning", "Error"

bool containsNote(const std::vector<ModelNote>& notes, std::string_view code) noexcept;
std::size_t countNotes(const std::vector<ModelNote>& notes, std::string_view code) noexcept;

// ---- note codes ----------------------------------------------------------------------------------------
// A marker edit (offset / ignore) references a marker id absent from the merged marker set (02 section 4.3
// behaviour 7; D-065). The edit stays in the document.
inline constexpr std::string_view kNoteOrphanMarkerEdit = "sidecar.orphan_marker_edit";
// A range override matched no range of its group by start marker or by name (02 section 5.3 rule 5).
inline constexpr std::string_view kNoteOrphanRangeOverride = "sidecar.orphan_range_override";
// A range override matched by its name fallback; the next save writes the resolved start marker.
inline constexpr std::string_view kNoteRangeOverrideByName = "sidecar.range_override_by_name";
// An offsetDistance edit needs Corr Dist and is resolved by a worker (02 section 4.3 behaviour 7a).
inline constexpr std::string_view kNoteOffsetDistancePending = "sidecar.offset_distance_pending";
// The sidecar's source.hash (or source.contentHash) differs from the opened file (02 section 5.3 rule 1).
inline constexpr std::string_view kNoteSourceChanged = "sidecar.source_changed";
// The sidecar holds edits for a layer the source does not have; they stay in the document.
inline constexpr std::string_view kNoteUnknownLayer = "sidecar.unknown_layer";
// The sidecar holds details under a layer other than "Normal"; source details come from "Normal" only.
inline constexpr std::string_view kNoteLayerDetailsIgnored = "sidecar.layer_details_ignored";
// Two markers of one input list share a MarkerId; the later one is dropped.
inline constexpr std::string_view kNoteDuplicateMarkerId = "model.duplicate_marker_id";
// A companion holds markers for a layer the source does not have; they are ignored.
inline constexpr std::string_view kNoteCompanionUnknownLayer = "companion.unknown_layer";

} // namespace esses::model
