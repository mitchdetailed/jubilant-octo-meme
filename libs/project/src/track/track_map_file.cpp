// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapFile (track-1, 02 section 6.9). Owner: the track work (src/track/).
//
// parse: the load pipeline of 02 section 6.6 rule 2 (header, migrations, analysis::TrackMap::fromJson, the
// map's own validate(): the 10 m minimum, the bounds, unique ids; then the schema check of what save()
// writes). save: the canonical text written atomically, creating trackmaps/ when needed.

#include "../doc_io.hpp"

#include <esses/project/track_map_file.hpp>

#include <cctype>
#include <filesystem>
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
    // ASCII letters and digits lower-cased, every other ASCII run a single '-', non-ASCII UTF-8 kept as is.
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

} // namespace esses::project
