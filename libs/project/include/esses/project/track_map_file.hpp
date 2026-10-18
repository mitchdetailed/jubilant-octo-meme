// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapFile (02 sections 6.1 and 6.9, 02 section 7 undo table; ARCHITECTURE_OUTLINE section 4
// esses_project): the project document trackmaps/<venue>.track.json (schema track-1) around esses_analysis'
// TrackMap. load() migrates, parses (TrackMap::fromJson), checks the map (TrackMap::validate: the 10 m
// minimum, the bounds, unique ids) and validates what it would write against SchemaRegistry (track-1);
// save() writes the canonical form atomically and creates the folder when needed (the copy of the previous
// file in backup/ on the Track Editor's Accept, 06 section 13.5 item 2, is the caller's).
// A venue's file (D-205): fileNameFor() is a readable slug of the venue followed by a hash of its exact bytes
// ("Mount Panorama" -> "mount-panorama-<16 hex digits>.track.json"), so two venues never share a name however
// alike their slugs ("Spa / Francorchamps" and "Spa-Francorchamps", every name without letters or digits);
// and a file is a venue's map only when its "venue" member is exactly that venue, which loadFor() checks and
// claimFor() keeps (it refuses a file that holds another venue's map). The name an earlier M3 build wrote,
// legacyFileNameFor() (the slug alone), is still read for its own venue and renamed on the next write.
// The section CRUD, rename, category edits and "regenerate as one command" are esses_qtmodel undo commands
// over this document. Owner: src/track/ (the track work).
#pragma once

#include <esses/analysis/track_map.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

class TrackMapFile {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:track-1";
    static constexpr std::string_view kDirectory = "trackmaps"; // under the project folder
    static constexpr std::size_t kMaxSlugBytes = 48;            // the readable part of fileNameFor()

    analysis::TrackMap map;
    int schemaVersion = 1;

    // `log` collects migration and repair notes (Messages, origin Project).
    static Result<TrackMapFile> parse(std::string_view json, std::vector<std::string>& log);
    static Result<TrackMapFile> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text
    Result<void> save(const std::filesystem::path& path) const;

    // "<slug>-<hash>.track.json" (D-205). The slug: ASCII letters and digits lower-cased; every run of other
    // ASCII, of C1 controls, Unicode spaces, format characters, noncharacters or invalid UTF-8 one '-'; other
    // UTF-8 kept; at most kMaxSlugBytes bytes, cut between characters; "track" when nothing is left. The
    // hash: the first 16 hex digits of xxh3-128 (esses::hash128) of the venue's exact bytes, so distinct
    // venues get distinct names whatever their case, spacing or punctuation. At most 76 bytes, valid UTF-8,
    // never a Windows device name, never a trailing dot or space.
    static std::string fileNameFor(std::string_view venue);
    // The name an M3 build before D-205 wrote: the slug alone (ASCII letters and digits lower-cased, other
    // ASCII runs one '-', other bytes kept, no length limit, "-track" after a Windows device name,
    // "track.track.json" without letters or digits). Venues of one slug share it, so it is read only for a
    // file that holds exactly the venue (loadFor) and renamed to fileNameFor() on that venue's next write
    // (claimFor).
    static std::string legacyFileNameFor(std::string_view venue);
    // `venue`'s map in `folder` (a project's trackmaps/): the file fileNameFor(venue), else the file
    // legacyFileNameFor(venue). A file whose "venue" member (absent: "") is not exactly `venue` holds another
    // venue's map and is passed over. NotFound when the folder holds no map of the venue; the load error
    // (context.path the file) when the venue's file cannot be read: the hashed file when it exists and is not
    // another venue's, else a legacy file that is not another venue's.
    static Result<TrackMapFile> loadFor(const std::filesystem::path& folder, std::string_view venue,
                                        std::vector<std::string>& log);
    // The file to write or remove `venue`'s map at: folder / fileNameFor(venue), after the venue's legacy
    // file (one holding exactly `venue`) has been renamed to it when that name is free, so the map of an
    // earlier build moves instead of staying behind. Io, with nothing changed, when the hashed file holds
    // another venue's map or the rename fails; a file there that cannot be read counts as the venue's.
    static Result<std::filesystem::path> claimFor(const std::filesystem::path& folder,
                                                  std::string_view venue);
};

} // namespace esses::project
