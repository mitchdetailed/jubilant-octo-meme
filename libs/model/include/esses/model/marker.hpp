// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Markers and marker edits (02 section 4.3; D-014, D-064, D-065, D-132). Device events are markers of class
// DeviceEvent in group "DeviceEvents"; range rules never see them because rules list their marker groups.
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/time.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/ids.hpp>
#include <esses/model/notes.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::model {

using esses::Micros;

// The eight D-014 classes plus DeviceEvent (D-132 (d)). The declaration order is the class order of the
// marker sort (t, cls, id), so SOL sorts first among markers at the same time.
enum class MarkerClass : std::uint8_t {
    SOL,
    EOL,
    BCN,
    SPLTBCN,
    IGRDBCN,
    RESET,
    STGSTRT,
    STGEND,
    DeviceEvent
};
std::string_view markerClassName(MarkerClass c) noexcept;                    // "SOL", ..., "DeviceEvent"
std::optional<MarkerClass> parseMarkerClass(std::string_view text) noexcept; // exact spelling only

inline constexpr std::string_view kGroupIntrinsic = "";                // SOL, EOL, RESET
inline constexpr std::string_view kGroupBeacons = "Beacons";           // BCN, SPLTBCN, IGRDBCN
inline constexpr std::string_view kGroupStageMarkers = "StageMarkers"; // STGSTRT, STGEND
inline constexpr std::string_view kGroupDeviceEvents = "DeviceEvents"; // DeviceEvent

struct Marker { // D-014
    MarkerId id;
    MarkerClass cls = MarkerClass::SOL;
    std::string group; // "" intrinsic, "Beacons", "StageMarkers", "DeviceEvents" or a companion group name
    Micros t = 0;      // effective time (after an Offset edit)
    std::string name;
    std::string comment;
    Provenance prov = Provenance::Logged;
    std::uint32_t flags = 0; // vendor flags passed through uninterpreted (.ldx Flags="77"; .ld event code)
    std::optional<Micros> originalT; // the logged time, set when an Offset edit is applied
    bool enabled = true;             // false after an Ignore edit

    Micros loggedT() const noexcept { return originalT.value_or(t); }
    friend bool operator==(const Marker&, const Marker&) = default;
};

enum class MarkerEditOp : std::uint8_t { Offset, Ignore, Restore, OffsetDistance };
std::string_view
markerEditOpName(MarkerEditOp op) noexcept; // "offset", "ignore", "restore", "offsetDistance"
std::optional<MarkerEditOp> parseMarkerEditOp(std::string_view text) noexcept;

struct MarkerEdit {
    MarkerId ref; // nil for OffsetDistance, which applies to every BCN of the layer
    MarkerEditOp op = MarkerEditOp::Offset;
    Micros dtUs = 0;          // Offset only
    double dM = 0.0;          // OffsetDistance only: positive = beacon further along the track, in dMUnit
    std::string dMUnit = "m"; // the unit as written (D-074); "m" for edits Esses creates

    friend bool operator==(const MarkerEdit&, const MarkerEdit&) = default;
};

// = uuidFromHash(hash128({"marker", className, decimal(loggedT), group, decimal(ordinal)})). `ordinal` is the
// marker's index among markers of the same (class, group, loggedT) in file order, 0 for the first.
MarkerId markerIdLogged(MarkerClass cls, Micros loggedT, std::string_view group, std::uint32_t ordinal);

// Model order (02 section 4.3 behaviour 8): t, then class in declaration order, then id.
bool markerOrderLess(const Marker& a, const Marker& b) noexcept;
void sortMarkers(std::vector<Marker>& markers);

// Importer helper: gives every marker its logged id in list order (the list is in file order), counting the
// ordinal per (class, group, loggedT()). Markers with prov == User keep their id.
void assignLoggedIds(std::vector<Marker>& markers);

// A logged marker with its id (prov Logged unless given).
Marker makeLoggedMarker(MarkerClass cls, Micros t, std::string group, std::string name,
                        std::string comment = {}, std::uint32_t flags = 0, std::uint32_t ordinal = 0,
                        Provenance prov = Provenance::Logged);

// Device events (port specification section 4): name = Esses' own label for the logger code, flags = code,
// comment = "", group "DeviceEvents", prov Logged.
std::string deviceEventName(std::uint32_t code);
Marker makeDeviceEventMarker(std::uint32_t code, Micros t, std::uint32_t ordinal);

// Union keyed by MarkerId (02 section 5.3 rule 2): a companion marker with the id of a file marker replaces
// it and the result has prov = Companion; other companion markers are added with prov = Companion. A repeated
// id inside one list keeps the first and raises kNoteDuplicateMarkerId. The result is in model order.
std::vector<Marker> mergeMarkers(std::vector<Marker> file, const std::vector<Marker>& companion,
                                 std::vector<ModelNote>* notes = nullptr, std::string_view layer = {});

// Applies offset and ignore edits in list order (02 section 4.3 behaviour 7): Offset sets originalT (when
// unset) and t = loggedT + dtUs; Ignore clears enabled. Restore removes nothing here (it is a document
// command, SidecarDocument::applyEdit). An edit whose ref is absent raises kNoteOrphanMarkerEdit;
// OffsetDistance raises kNoteOffsetDistancePending (resolved by a worker through Corr Dist). Not re-sorted.
std::vector<ModelNote> applyMarkerEdits(std::vector<Marker>& markers, std::span<const MarkerEdit> edits,
                                        std::string_view layer = {});

// Identity of a layer's marker set for the phase-2 range task key ranges/<sourceId>/<layer>/<markerSetHash>
// (02 section 4.4 rule 13): xxh3-128 over (id, class, group, t, enabled) of every marker in model order.
Hash128 markerSetHash(std::span<const Marker> markers);

} // namespace esses::model
