// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Whole-file reads of documents (JSON documents, schemas, unit tables, fixture expectations): the one reader
// every library and the CLI use, so that a path that is not a regular file fails the same way everywhere.
// Standard libraries disagree about a directory: MSVC's stream refuses to open it; on POSIX the open succeeds
// and then libstdc++'s file buffer throws std::ios_base::failure (EISDIR) from the read whatever the stream's
// exception mask, while libc++ reads it as an empty file without any error. readWholeFile() therefore checks
// the file type before it opens anything, so none of the three behaviours is ever reached, and a FIFO is
// never opened (that would block). AtomicFile is the write side.
// On Windows the file is opened with CreateFileW sharing read, write and delete access, and read with
// ReadFile. A std::ifstream shares read and write only: it is refused with a sharing violation while the
// handle through which AtomicFile renamed the file into place is still open (that handle holds delete
// access), and while the stream is open, AtomicFile cannot rename a new version over the file. While a rename
// over the file is under way its name can also be missing (the old file loses the name before the new one
// gets it: measured gaps of about 10 ms, and renames that took up to 200 ms on a loaded machine), so on
// Windows a file that is not found, or is being deleted, is looked for again for up to 500 ms (as long as
// AtomicFile retries a refused rename) before the error is reported; a sharing violation is reported at once.
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/result.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace esses {

// ok() when `path` names an existing regular file (symbolic links are followed). Otherwise Io with
// context.path = path:
//   "cannot open <what>"              it does not exist (nor does a directory on its path)
//   "cannot stat <what>: <reason>"    its type cannot be determined (permissions on a parent, a link loop)
//   "<what> is not a regular file"    a directory, device, FIFO or socket
// `what` names the file in the messages ("the unit table"); empty names it by its path in quotes.
Result<void> requireRegularFile(const std::filesystem::path& path, std::string_view what = {});

// The bytes of a regular file, read in binary mode (no newline translation). The requireRegularFile() errors
// first, then, with context.path = path:
//   Io "cannot open <what>"                 the open failed (permissions, a sharing violation)
//   Io "cannot read <what>"                 a read failed
//   OutOfMemory "out of memory reading <what>"
// No exception escapes. A file that changes while it is read gives whatever bytes the reads returned.
Result<std::string> readWholeFile(const std::filesystem::path& path, std::string_view what = {});

// The first `maxBytes` bytes of a regular file (all of it when it is shorter), with the checks, the sharing
// and the errors of readWholeFile(). Importer sniffing reads the head of a file this way.
Result<std::string> readFileHead(const std::filesystem::path& path, std::size_t maxBytes,
                                 std::string_view what = {});

} // namespace esses
