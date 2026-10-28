// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The changed-file check (source_file_check.hpp; the M4 reload package's root in esses_formats,
// docs/milestones/M4.md section 3): a re-stat of the path through esses::statFile, the call
// MappedFile::changedOnDisk() makes, compared with the size and modification time the open recorded.

#include <esses/base/mapped_file.hpp>
#include <esses/formats/source_file_check.hpp>

namespace esses::formats {

SourceFileCheck checkSourceFile(const std::filesystem::path& path, std::uint64_t sizeAtOpen,
                                std::int64_t mtimeAtOpenUnixUs) {
    SourceFileCheck check;
    if (path.empty()) { // an in-memory source: nothing on disk to compare
        check.size = sizeAtOpen;
        check.mtimeUnixUs = mtimeAtOpenUnixUs;
        return check;
    }
    const Result<FileStat> now = statFile(path);
    if (!now.ok()) {
        check.state = SourceFileState::Missing; // deleted, renamed away, or its share dropped
        return check;
    }
    check.size = now.value().size;
    check.mtimeUnixUs = now.value().mtimeUnixUs;
    const bool changed = check.size != sizeAtOpen || check.mtimeUnixUs != mtimeAtOpenUnixUs;
    check.state = changed ? SourceFileState::Changed : SourceFileState::Unchanged;
    return check;
}

SourceFileCheck checkSourceFile(const model::SourceProvenance& provenance) {
    return checkSourceFile(provenance.path, provenance.size, provenance.mtimeUnixUs);
}

} // namespace esses::formats
