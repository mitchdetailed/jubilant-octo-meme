// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Ranges, range groups and range references (02 section 4.4; D-014, D-066, D-132, D-140). A range is
// identified by (group path, start-marker id[, qualifier]), so beacon edits never orphan a trust flag or an
// alignment offset; a RangeGroupTree keeps one version per group path so maths reads only the groups it uses.
#pragma once

#include <esses/base/time.hpp>
#include <esses/model/ids.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::model {

using esses::Micros;

enum class RangeKind : std::uint8_t { Outing, Lap, Run, Stage, Section, Split, Data };
enum class LapClass : std::uint8_t {
    Flying,
    Out,
    In,
    Partial,
    Pit /*reserved: never assigned by the circuit rule*/,
    None
};
std::string_view rangeKindName(RangeKind k) noexcept; // "Outing", "Lap", ...
std::string_view lapClassName(LapClass c) noexcept;   // "Flying", "Out", "In", "Partial", "Pit", "None"
std::optional<RangeKind> parseRangeKind(std::string_view text) noexcept;
std::optional<LapClass> parseLapClass(std::string_view text) noexcept;

struct Range { // D-014; [start, end)
    RangeId id;
    RangeKind kind = RangeKind::Outing;
    LapClass lapClass = LapClass::None;
    MarkerId startMarker; // anchor markers (rule 10); nil when the bound has no marker
    MarkerId endMarker;
    Micros start = 0;
    Micros end = 0;
    std::string name;   // "Lap 3", "Out Lap"
    std::string abbrev; // "L3", "Out"
    int number = 0;
    bool trusted = true;
    bool enabled = true;
    std::uint16_t stint = 0;       // phase-2 annotation: 1-based stint, 0 = unknown
    bool pitStop = false;          // phase-2 annotation; never changes lapClass, number or trusted
    Micros offsetUs = 0;           // alignment offset, time domain (sidecar)
    double offsetM = 0.0;          // alignment offset, distance domain (sidecar)
    std::optional<double> stretch; // derived by LapStretch, never persisted

    Micros duration() const noexcept { return end - start; }
    friend bool operator==(const Range&, const Range&) = default;
};

struct RangeGroup {
    std::string path;          // "Outings:Laps:Track Sections:Default"
    std::string name;          // last segment
    std::vector<Range> ranges; // time-ordered
    std::vector<RangeGroup> children;
    bool supportsTrackGeneration = false;
    bool supportsReporting = true;
    std::string detailTemplate; // "'Driver' Lap {n}" for Data-window labels
    std::uint64_t version = 0;  // per group path (D-140): bumps only when this group's ranges change

    // A group named after the last segment of `path`.
    static RangeGroup make(std::string path);
    friend bool operator==(const RangeGroup&, const RangeGroup&) = default;
};

// The parent path of "Outings:Laps" is "Outings"; of a root, "".
std::string_view parentGroupPath(std::string_view path) noexcept;
std::string_view lastGroupSegment(std::string_view path) noexcept;

struct RangeGroupTree {
    std::vector<RangeGroup> roots;

    const RangeGroup* find(std::string_view path) const noexcept; // exact path match, depth first
    RangeGroup* find(std::string_view path) noexcept;
    // 03 section 7.2's rangeVersion(g): the version of that group path, 0 when absent.
    std::uint64_t rangeVersion(std::string_view path) const noexcept;

    // Inserts or replaces the group with g.path (children of a replaced group are kept when g has none).
    // A root goes to `roots`; any other path goes under its parent, which must exist (returns false and
    // changes nothing otherwise).
    bool setGroup(RangeGroup g);
    bool removeGroup(std::string_view path);

    // Carries versions over from the tree this one replaces (02 section 4.4 rule 13, D-140): a group whose
    // ranges equal those of the same path in `previous` keeps that version, a changed group gets
    // previous version + 1, and a new group gets 1. Children are visited recursively.
    void inheritVersions(const RangeGroupTree& previous);

    // Depth-first, pre-order visit of every group.
    void forEach(const std::function<void(const RangeGroup&)>& fn) const;
    void forEach(const std::function<void(RangeGroup&)>& fn);

    friend bool operator==(const RangeGroupTree&, const RangeGroupTree&) = default;
};

struct RangeRef { // what selection and view state hold (D-014)
    SourceId src;
    std::string layer;
    std::string groupPath;
    RangeId range;
    std::string name; // fallback key for persistence and workspace import only

    // Selection equality: (src, canonical layer, groupPath, range); name is ignored (02 section 4.4 rule 14).
    friend bool operator==(const RangeRef& a, const RangeRef& b) noexcept {
        return a.src == b.src && canonicalLayerName(a.layer) == canonicalLayerName(b.layer) &&
               a.groupPath == b.groupPath && a.range == b.range;
    }
};

// = uuidFromHash(hash128({"range", groupPath, anchor.v.str(), qualifier})). Marker-bounded kinds (Outing,
// Lap, Run, Stage, Split) use their start marker and no qualifier; Section and Data use the start marker of
// the enclosing marker-bounded range and the section name as qualifier (D-066).
RangeId rangeId(std::string_view groupPath, const MarkerId& anchor, std::string_view qualifier = {});

} // namespace esses::model

template <> struct std::hash<esses::model::RangeRef> {
    std::size_t operator()(const esses::model::RangeRef& r) const noexcept {
        std::size_t h = std::hash<esses::Hash128>{}(r.src);
        const auto mix = [&h](std::size_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
        mix(std::hash<std::string_view>{}(esses::model::canonicalLayerName(r.layer)));
        mix(std::hash<std::string>{}(r.groupPath));
        mix(std::hash<esses::model::RangeId>{}(r.range));
        return h;
    }
};
