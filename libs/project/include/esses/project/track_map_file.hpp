// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapFile (02 sections 6.1 and 6.9, 02 section 7 undo table; ARCHITECTURE_OUTLINE section 4
// esses_project): the project document trackmaps/<venue>.track.json (schema track-1) around esses_analysis'
// TrackMap. load() migrates, parses (TrackMap::fromJson), checks the map (TrackMap::validate: the 10 m
// minimum, the bounds, unique ids) and validates what it would write against SchemaRegistry (track-1);
// save() writes the canonical form atomically and creates the folder when needed (the copy of the previous
// file in backup/ on the Track Editor's Accept, 06 section 13.5 item 2, is the caller's). fileNameFor()
// slugifies the venue ("Mount Panorama" -> "mount-panorama.track.json"; ASCII letters and digits lower-cased,
// other ASCII runs one '-', non-ASCII kept; a Windows device name gets "-track"). The section CRUD, rename,
// category edits and "regenerate as one command" are esses_qtmodel undo commands over this document. Owner:
// src/track/ (the track work).
#pragma once

#include <esses/analysis/track_map.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

class TrackMapFile {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:track-1";
    static constexpr std::string_view kDirectory = "trackmaps"; // under the project folder

    analysis::TrackMap map;
    int schemaVersion = 1;

    // `log` collects migration and repair notes (Messages, origin Project).
    static Result<TrackMapFile> parse(std::string_view json, std::vector<std::string>& log);
    static Result<TrackMapFile> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text
    Result<void> save(const std::filesystem::path& path) const;
    // "<slug of venue>.track.json"; "track.track.json" for an empty venue.
    static std::string fileNameFor(std::string_view venue);
};

} // namespace esses::project
