// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapFile (track-1, 02 section 6.9). Owner: the track work (src/track/).
//
// parse: the load pipeline of 02 section 6.6 rule 2 (header, migrations, analysis::TrackMap::fromJson, the
// map's own validate(): the 10 m minimum, the bounds, unique ids; then the schema check of what save()
// writes). save: the canonical text written atomically, creating trackmaps/ when needed. A venue's file
// (D-205): fileNameFor() (slug and hash), legacyFileNameFor() (the slug of the builds before D-205),
// loadFor() (the hashed file, else the legacy one, each only when it holds exactly the venue) and claimFor()
// (the hashed file before a write or a removal, the venue's legacy file renamed to it).

#include "../doc_io.hpp"

#include <esses/base/hash.hpp>
#include <esses/base/utf8.hpp>
#include <esses/project/track_map_file.hpp>

#include <cctype>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

namespace esses::project {

namespace {

using jsonio::violate;

const detail::DocKind kKind{"track", 1};

detail::json fileJson(const TrackMapFile& f) {
    detail::json root = jsonio::fromDoc(f.map.toJson());
    if (!root.is_object()) {
        root = detail::newDocument(kKind.name, f.schemaVersion); // a number that is not finite
    }
    root["schemaVersion"] = f.schemaVersion;
    return root;
}

bool reservedDeviceName(std::string_view stem) {
    static constexpr std::string_view kNames[] = {"con", "prn", "aux", "nul"};
    for (const std::string_view n : kNames) {
        if (stem == n) {
            return true;
        }
    }
    if (stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") && stem[3] >= '1' &&
        stem[3] <= '9') {
        return true;
    }
    return false;
}

bool asciiAlnum(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// A non-ASCII code point the readable part of a file name leaves out (a separator, like ASCII punctuation):
// C1 controls and NO-BREAK SPACE, the other Unicode spaces, format characters (soft hyphen, the combining
// grapheme joiner, bidirectional marks and isolates, zero-width characters, the byte order mark, tags),
// noncharacters and U+FFFD, which is also what invalid UTF-8 decodes to. They are invisible or ambiguous in a
// file name, and macOS refuses a name that is not valid UTF-8.
bool separatorCodePoint(char32_t cp) noexcept {
    return cp <= 0xA0 || cp == 0xAD || cp == 0x34F || cp == 0x61C || cp == 0x1680 || cp == 0x180E ||
           (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) ||
           (cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF || (cp >= 0xFDD0 && cp <= 0xFDEF) ||
           (cp >= 0xFFF9 && cp <= 0xFFFD) || (cp & 0xFFFEu) == 0xFFFEu || (cp >= 0xE0000 && cp <= 0xE007F);
}

// The readable part of fileNameFor() (see the header).
std::string readableSlug(std::string_view venue) {
    std::string slug;
    bool dash = false;
    std::size_t pos = 0;
    while (pos < venue.size()) {
        const auto c = static_cast<unsigned char>(venue[pos]);
        const std::size_t at = pos;
        if (c < 0x80) {
            ++pos;
            if (!asciiAlnum(c)) {
                dash = true;
                continue;
            }
        } else if (separatorCodePoint(utf8::decodeNext(venue, pos))) {
            dash = true;
            continue;
        }
        const bool joint = dash && !slug.empty();
        const std::size_t bytes = pos - at; // the character's own UTF-8 (valid: it did not decode to U+FFFD)
        if (slug.size() + bytes + (joint ? 1 : 0) > TrackMapFile::kMaxSlugBytes) {
            break; // cut between characters, never after a '-'
        }
        if (joint) {
            slug += '-';
        }
        dash = false;
        if (c < 0x80) {
            slug += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        } else {
            slug.append(venue.substr(at, bytes));
        }
    }
    return slug.empty() ? std::string("track") : slug;
}

bool present(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

// The "venue" member of the track document at `path` ("" when absent, as TrackMap::fromJson reads it);
// nullopt when the file cannot be read or parsed, is not a JSON object or holds a venue that is not a string.
std::optional<std::string> storedVenue(const std::filesystem::path& path) {
    Result<std::string> text = jsonio::readFile(path);
    if (!text) {
        return std::nullopt;
    }
    Result<detail::json> root = jsonio::parseJson(*text);
    if (!root || !root->is_object()) {
        return std::nullopt;
    }
    const auto it = root->find("venue");
    if (it == root->end()) {
        return std::string();
    }
    if (!it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

// The file at `path`, which could not be loaded, names a venue other than `venue`.
bool heldByAnother(const std::filesystem::path& path, std::string_view venue) {
    const std::optional<std::string> held = storedVenue(path);
    return held.has_value() && *held != venue;
}

} // namespace

Result<TrackMapFile> TrackMapFile::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::readDocument<TrackMapFile>(
        json, kKind, log,
        [&](const detail::json& root, const detail::DocVersion& version) -> Result<TrackMapFile> {
            auto doc = jsonio::toDoc(root);
            if (!doc) {
                violate("", doc.error().message);
            }
            auto read = analysis::TrackMap::fromJson(*doc);
            if (!read) {
                return Failure{read.error()};
            }
            if (auto valid = read->validate(); !valid) {
                return Failure{valid.error()};
            }
            TrackMapFile f;
            f.map = std::move(read).value();
            f.schemaVersion = version.write;
            return f;
        },
        [](const TrackMapFile& f) { return fileJson(f); });
}

Result<TrackMapFile> TrackMapFile::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    return detail::loadDocument<TrackMapFile>(
        path, [&](std::string_view text) { return TrackMapFile::parse(text, log); });
}

std::string TrackMapFile::toJson() const {
    return detail::canonicalOrEmpty(fileJson(*this));
}

Result<void> TrackMapFile::save(const std::filesystem::path& path) const {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            Error e{ErrorCode::Io, "cannot create the folder of the track map: " + ec.message(), {}};
            e.context.path = path.parent_path();
            return Failure{std::move(e)};
        }
    }
    return detail::saveCanonical(fileJson(*this), path);
}

std::string TrackMapFile::fileNameFor(std::string_view venue) {
    std::string name = readableSlug(venue);
    name += '-';
    name += hash128(venue).hex().substr(0, 16); // the high 64 bits, in xxHash's canonical order
    name += ".track.json";
    return name;
}

std::string TrackMapFile::legacyFileNameFor(std::string_view venue) {
    // The builds before D-205, unchanged so that their files are still found: ASCII letters and digits
    // lower-cased, every other ASCII run a single '-', non-ASCII bytes kept as they are.
    std::string slug;
    bool dash = false;
    for (const char ch : venue) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80) {
            if (dash && !slug.empty()) {
                slug += '-';
            }
            dash = false;
            slug += ch;
        } else if (std::isalnum(c) != 0) {
            if (dash && !slug.empty()) {
                slug += '-';
            }
            dash = false;
            slug += static_cast<char>(std::tolower(c));
        } else {
            dash = true;
        }
    }
    if (slug.empty()) {
        slug = "track";
    } else if (reservedDeviceName(slug)) {
        slug += "-track"; // a Windows device name
    }
    return slug + ".track.json";
}

Result<TrackMapFile> TrackMapFile::loadFor(const std::filesystem::path& folder, std::string_view venue,
                                           std::vector<std::string>& log) {
    const std::filesystem::path hashed = folder / pathFromUtf8(fileNameFor(venue));
    const std::filesystem::path legacy = folder / pathFromUtf8(legacyFileNameFor(venue));
    for (const std::filesystem::path* candidate : {&hashed, &legacy}) {
        if ((candidate == &legacy && legacy == hashed) || !present(*candidate)) {
            continue;
        }
        std::vector<std::string> notes; // the notes of another venue's file are not this venue's
        Result<TrackMapFile> file = load(*candidate, notes);
        if (file ? file->map.venue != venue : heldByAnother(*candidate, venue)) {
            continue; // another venue's map (every venue of one slug shares a legacy name)
        }
        log.insert(log.end(), notes.begin(), notes.end());
        return file; // the venue's map, or why the venue's file cannot be read
    }
    Error e{
        ErrorCode::NotFound, "the project holds no track map of venue \"" + std::string(venue) + "\"", {}};
    e.context.path = hashed;
    return Failure{std::move(e)};
}

Result<std::filesystem::path> TrackMapFile::claimFor(const std::filesystem::path& folder,
                                                     std::string_view venue) {
    const std::filesystem::path hashed = folder / pathFromUtf8(fileNameFor(venue));
    if (present(hashed)) {
        if (const std::optional<std::string> other = storedVenue(hashed); other && *other != venue) {
            Error e{ErrorCode::Io,
                    "the track map file of venue \"" + std::string(venue) + "\" holds the map of venue \"" +
                        *other + "\"",
                    {}};
            e.context.path = hashed;
            return Failure{std::move(e)};
        }
        return hashed;
    }
    const std::filesystem::path legacy = folder / pathFromUtf8(legacyFileNameFor(venue));
    if (legacy != hashed && present(legacy)) {
        if (const std::optional<std::string> held = storedVenue(legacy); held && *held == venue) {
            std::error_code ec;
            std::filesystem::rename(legacy, hashed, ec); // the earlier build's map moves to the new name
            if (ec) {
                Error e{
                    ErrorCode::Io, "cannot rename the track map to its new file name: " + ec.message(), {}};
                e.context.path = legacy;
                return Failure{std::move(e)};
            }
        }
    }
    return hashed;
}

} // namespace esses::project
