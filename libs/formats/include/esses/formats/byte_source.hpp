// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ByteSource (05 section 1.1): the read-only view of a log every importer reads through - a memory-mapped
// file or an in-memory buffer (fuzzing, tests, companions) - with the source identity of D-130.
//
// Identity. quickHash(formatHeader) is xxh3-128 over the size (u64 little-endian), the first MiB, the last
// MiB (the whole file twice when it is shorter, exactly as MappedFile's stamp) and, when given, the format
// header the importer located (u64 length, then the bytes; .ld: the header and every channel record). It
// reads at most about 2 MiB plus the header and never the whole file; mtime is excluded, so a copied or moved
// log keeps its SourceId. contentHash() is the full-content xxh3-128 (sequential, cancellable, Background
// only).
#pragma once

#include <esses/base/cancel.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/mapped_file.hpp>
#include <esses/base/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace esses::formats {

class ByteSource {
public:
    ByteSource() = default;
    ByteSource(const ByteSource&) = delete;
    ByteSource& operator=(const ByteSource&) = delete;
    virtual ~ByteSource() = default;

    virtual const std::filesystem::path& path() const = 0; // may be empty for buffers
    virtual std::uint64_t size() const = 0;
    // Zero-copy view of [offset, offset + len) clamped to the source; empty beyond the end. Never throws.
    virtual std::span<const std::byte> span(std::uint64_t offset, std::uint64_t len) const = 0;
    virtual Hash128 quickHash(std::span<const std::byte> formatHeader = {}) const = 0;
    // Io on a page fault (a file truncated under the mapping), Cancelled when `cancel` fires between blocks.
    virtual Result<Hash128> contentHash(CancelToken cancel) const = 0;
    // Files in the same directory whose stem equals this file's stem (case-insensitive on Windows), the file
    // itself excluded, sorted by name: companion discovery (.ldx beside .ld).
    virtual std::vector<std::filesystem::path> siblings() const = 0;

    // Additions to 05 section 1.1. Modification time at open (0 for buffers); part of the open task key only,
    // never of the identity.
    virtual std::int64_t mtimeUnixUs() const { return 0; }
    // The mapping behind a file source (nullptr for buffers): the .ld chunk provider guards its decodes and
    // checks changedOnDisk() through it.
    virtual std::shared_ptr<const MappedFile> mappedFile() const { return nullptr; }
    // The buffer behind an in-memory source (nullptr for files).
    virtual std::shared_ptr<const std::vector<std::byte>> buffer() const { return nullptr; }
};

// Maps `path` read-only (MappedFile). Errors: Io (context.path) when it cannot be opened or mapped.
Result<std::shared_ptr<const ByteSource>> openByteSource(const std::filesystem::path& path);

// An in-memory source. `path` is informational (a file name gives the extension sniffing looks at);
// `siblings` is what siblings() returns.
std::shared_ptr<const ByteSource> makeMemorySource(std::vector<std::byte> bytes,
                                                   std::filesystem::path path = {},
                                                   std::vector<std::filesystem::path> siblings = {});

// The directory listing behind ByteSource::siblings() for a path on disk (empty when the directory cannot be
// read).
std::vector<std::filesystem::path> listSiblings(const std::filesystem::path& file);

// The sibling named <stem of file><extension> (extension with its dot, e.g. ".ldx"), compared
// case-insensitively on Windows and exactly elsewhere; nullopt when there is none.
std::optional<std::filesystem::path> findCompanion(std::span<const std::filesystem::path> siblings,
                                                   const std::filesystem::path& file,
                                                   std::string_view extension);

// The quick-hash algorithm over a whole in-memory image (what ByteSource::quickHash computes for it).
Hash128 quickHashOf(std::span<const std::byte> bytes, std::span<const std::byte> formatHeader = {});

} // namespace esses::formats
