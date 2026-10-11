// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MappedFile (02 section 1 behaviour 5): a read-only, whole-file memory map used by the .ld chunk provider.
//   * Windows: CreateFileW(GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE) + CreateFileMappingW +
//     MapViewOfFile. Never FILE_SHARE_WRITE: a writer that replaces the file atomically (temp + rename) can
//     proceed while the view keeps the old bytes; an in-place writer gets a clean sharing error.
//   * POSIX: open(O_RDONLY) + mmap(PROT_READ, MAP_PRIVATE).
//   * An empty file opens successfully with an empty view (it cannot be mapped).
//   * stampAtOpen() records (size, mtime, quick hash); changedOnDisk() re-stats the path and compares
//     size and mtime. The quick hash is xxh3-128 over the size (u64 LE), the first MiB and the last MiB;
//     the importer-level SourceId additionally covers the format header (D-130).
//   * guarded() runs a plain C-style decode routine over the view and turns a page fault inside the view
//     into fail(Io): EXCEPTION_IN_PAGE_ERROR via __try/__except on Windows, SIGBUS via a handler and
//     per-thread sigsetjmp on POSIX. The routine must not own objects with destructors (a fault unwinds by
//     longjmp on POSIX). Faults outside the view are not intercepted.
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace esses {

class MappedFile {
public:
    struct Stamp {
        std::uint64_t size = 0;
        std::int64_t mtimeUnixUs = 0;
        Hash128 quick;
        friend bool operator==(const Stamp&, const Stamp&) = default;
    };

    static Result<MappedFile> open(const std::filesystem::path& path);

    MappedFile() noexcept = default; // closed, empty view
    ~MappedFile();
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool isOpen() const noexcept { return open_; }
    std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }
    std::size_t size() const noexcept { return size_; }
    const std::filesystem::path& path() const noexcept { return path_; }
    const Stamp& stampAtOpen() const noexcept { return stamp_; }

    // Re-stat: true when the size or mtime differs from stampAtOpen(), or the file can no longer be stat'ed.
    bool changedOnDisk() const;

    Result<void> guarded(void (*decode)(void* ctx), void* ctx) const noexcept;

    void close() noexcept;

private:
    void moveFrom(MappedFile& other) noexcept;

    std::filesystem::path path_;
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    Stamp stamp_;
    bool open_ = false;
#if defined(ESSES_OS_WINDOWS)
    void* fileHandle_ = nullptr;    // HANDLE
    void* mappingHandle_ = nullptr; // HANDLE
#endif
};

// (size, mtime) of a path without opening it; fail(Io) when it cannot be stat'ed.
struct FileStat {
    std::uint64_t size = 0;
    std::int64_t mtimeUnixUs = 0;
};
Result<FileStat> statFile(const std::filesystem::path& path);

} // namespace esses
