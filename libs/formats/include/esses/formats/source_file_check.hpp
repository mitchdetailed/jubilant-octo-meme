// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The changed-file check of an open source (M4 contract, D-206; 02 section 1 behaviour 5, 06 section 6
// "Changed files", 07 section 7.1, AC-M4-10). The open pipeline records the file's size and modification time
// in model::SourceProvenance (with MappedFile's stamp for a mapped .ld); checkSourceFile re-stats the path
// and compares, exactly as MappedFile::changedOnDisk() does for the mapping, so a source whose importer keeps
// no mapping (the text importers) is checked the same way. SourceLoader runs it for every open source when
// the application becomes active; the chunk providers keep their own check before each decode batch (the .ld
// provider's MappedFile::changedOnDisk(), ld.source_changed). A replaced file (rename-over, the writer the
// sharing policy lets through) and an in-place change both read as Changed; a file that can no longer be
// stat'ed (deleted, renamed away, a share that dropped) is Missing. Never reads the file's content.
//
// Owner: the M4 reload package (docs/milestones/M4.md section 3; src/m4_reload/source_file_check.cpp). The
// re-stat is esses::statFile, the call MappedFile::changedOnDisk() makes; a source without a path (an
// in-memory source) has nothing to check and reads as Unchanged with the recorded values.
#pragma once

#include <esses/model/source.hpp>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace esses::formats {

enum class SourceFileState : std::uint8_t { Unchanged, Changed, Missing };

// Final (D-206): "unchanged", "changed", "missing".
constexpr std::string_view sourceFileStateName(SourceFileState state) noexcept {
    switch (state) {
    case SourceFileState::Unchanged:
        return "unchanged";
    case SourceFileState::Changed:
        return "changed";
    case SourceFileState::Missing:
        return "missing";
    }
    return "unchanged";
}

struct SourceFileCheck {
    SourceFileState state = SourceFileState::Unchanged;
    std::uint64_t size = 0;       // now (0 when Missing)
    std::int64_t mtimeUnixUs = 0; // now (0 when Missing)
};

// Re-stats `path` against the size and modification time recorded at open: Changed when either differs,
// Missing when the path cannot be stat'ed, else Unchanged.
SourceFileCheck checkSourceFile(const std::filesystem::path& path, std::uint64_t sizeAtOpen,
                                std::int64_t mtimeAtOpenUnixUs);
// The same for an open source's provenance (path, size, mtimeUnixUs).
SourceFileCheck checkSourceFile(const model::SourceProvenance& provenance);

} // namespace esses::formats
