// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AtomicFile (01 section 9 rule 5): every write of a persisted file is temp + flush + rename over the target,
// so a reader (or a crash) never sees a half-written file.
//   Windows: CreateFileW(CREATE_NEW) in the target's directory, WriteFile, FlushFileBuffers, then a rename
//            with POSIX semantics through a second handle that holds delete access only, so a reader that
//            shares delete access (readWholeFile, MappedFile) is never refused with a sharing violation; a
//            refused rename is retried for up to 500 ms, then MoveFileExW(MOVEFILE_REPLACE_EXISTING |
//            MOVEFILE_WRITE_THROUGH) is tried.
//   POSIX:   open(O_CREAT | O_EXCL), write, fsync, rename, fsync of the directory.
// The temporary file is removed on every failure path.
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/result.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>

namespace esses {

class AtomicFile {
public:
    static Result<void> write(const std::filesystem::path& target, std::span<const std::byte> bytes);
    static Result<void> write(const std::filesystem::path& target, std::string_view text);
};

} // namespace esses
