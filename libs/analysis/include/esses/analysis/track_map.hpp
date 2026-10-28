// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMap (track-1; 02 section 6.9 "Track map", ARCHITECTURE_OUTLINE section 4 esses_analysis; D-027): a
// venue's centreline as ENU metres about `origin`, with optional inner/outer lines, and its named sections.
// `centre` rows are [x, y, cumDist] (cumDist from 0 at the start line to `lengthM`); every section is at
// least kMinSectionM long and lies in [0, lengthM]. `set` names a section set ("Default" for the generated
// Turns/Straights; user sets may overlap other sets) and `category` is "corner", "straight" or any user
// category; together they produce the Outings:Laps:Track Sections:<set>[:<category group>] groups of 02
// section 4.5 (TrackSectionsRule). `speedClass` (M4) is "", "slow" or "fast" (kSpeedClasses). Stored as
// trackmaps/<venue>.track.json; esses_project's TrackMapFile wraps the file (load/validate/save) and the undo
// commands. Values are SI base units; the document carries {v, unit} lengths (D-010), converted on read (m,
// km, cm, mm, mi, ft, yd, in; written in m). A closed map's centreline ends with its start point again at
// dist = lengthM (TrackGenerator writes it so); pointAt also wraps across a missing closing row. Owner:
// src/track/ (the track work).
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

enum class TrackDirection : std::uint8_t { Clockwise, CounterClockwise }; // "cw" | "ccw"
enum class TrackMethod : std::uint8_t { None, Gps, SpeedLatG };           // generatedFrom.method

struct TrackOrigin { // WGS-84 degrees and metres
    double lat = 0.0;
    double lon = 0.0;
    double alt = 0.0;
    friend bool operator==(const TrackOrigin&, const TrackOrigin&) = default;
};

struct TrackPoint { // ENU metres about the origin; dist = cumulative centreline distance
    double x = 0.0;
    double y = 0.0;
    double dist = 0.0;
    friend bool operator==(const TrackPoint&, const TrackPoint&) = default;
};

struct TrackSection {
    std::string id; // unique within the map ("t1", "u1")
    std::string set = "Default";
    std::string name;    // "Turn 1", "Hell Corner"
    double startM = 0.0; // track distance from the start line
    double endM = 0.0;
    std::string category = "corner"; // "corner" | "straight" | a user category
    bool userDefined = false;
    // Added by M4 (06 section 13.5 item 4, D-211 (c)): the section's speed class, "" (none), "slow" or
    // "fast"; written only when set, so maps without one are unchanged.
    std::string speedClass{};
    double lengthM() const noexcept { return endM - startM; }
    friend bool operator==(const TrackSection&, const TrackSection&) = default;
};

struct TrackGeneratedFrom {
    TrackMethod method = TrackMethod::None;
    std::string source; // the source id ("xxh3:...")
    std::string lap;    // the lap's range id
    friend bool operator==(const TrackGeneratedFrom&, const TrackGeneratedFrom&) = default;
};

struct TrackMap {
    static constexpr double kMinSectionM = 10.0; // 02 section 6.9, D-027
    static constexpr std::string_view kDefaultSet = "Default";
    // The speed classes a section may carry (M4, D-211 (c)); "" is none.
    static constexpr std::string_view kSpeedClasses[] = {"slow", "fast"};

    std::string name;
    std::string venue; // the Venue detail the map binds to (04 section 14 item 1)
    TrackOrigin origin;
    double rotationDeg = 0.0;
    TrackDirection direction = TrackDirection::Clockwise;
    bool closed = true;
    double lengthM = 0.0;
    TrackGeneratedFrom generatedFrom;
    std::vector<TrackPoint> centre;
    std::vector<TrackPoint> inner; // optional (GPS with inner/outer laps)
    std::vector<TrackPoint> outer;
    std::vector<TrackSection> sections;
    JsonDoc rest; // the unknown top-level members as read ("x-i2", ...): they survive toJson()

    // A track-1 document (validated against schemas/track-1.schema.json by the caller, TrackMapFile). Its
    // "$schema" is checked when present. Invalid with context.detail (a JSON pointer) on a member of the
    // wrong type or a length without a known unit; an absent "length" is the centreline's last dist.
    static Result<TrackMap> fromJson(const JsonDoc& document);
    // The canonical track-1 document ($schema urn:esses:track-1, schemaVersion 1; lengths written in m; every
    // known member written, generatedFrom only when set). The absent value when a number is not finite.
    JsonDoc toJson() const;
    // Invalid with context.detail when a section is shorter than kMinSectionM, leaves [0, lengthM], has an
    // empty name, set or category, a speed class other than those of kSpeedClasses or a duplicate id, the
    // centreline is not ordered by dist, or a number is not finite (1e-6 m tolerance on the bounds and the
    // minimum).
    Result<void> validate() const;
    // The centreline position at a track distance (interpolated; wraps modulo lengthM when closed, clamps
    // otherwise).
    TrackPoint pointAt(double distM) const noexcept;
    // The section sets in use: "Default" first when present, then the others in order of first appearance.
    std::vector<std::string> sectionSets() const;

    friend bool operator==(const TrackMap&, const TrackMap&) = default;
};

} // namespace esses::analysis
