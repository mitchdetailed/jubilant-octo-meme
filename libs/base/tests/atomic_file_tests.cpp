// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "test_support.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/base/mapped_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

#if defined(ESSES_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
std::size_t countEntries(const fs::path& dir) {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}
} // namespace

TEST_CASE("atomic file: writes, replaces and leaves no temporary behind", "[unit][atomicfile]") {
    esses::test::TempDir dir("atomic");
    const fs::path target = dir.path() / "doc.json";
    REQUIRE(esses::AtomicFile::write(target, std::string_view("{\"a\": 1}\n")).ok());
    CHECK(readAll(target) == "{\"a\": 1}\n");
    REQUIRE(esses::AtomicFile::write(target, std::string_view("{\"a\": 2}\n")).ok());
    CHECK(readAll(target) == "{\"a\": 2}\n");
    CHECK(countEntries(dir.path()) == 1u);
    REQUIRE(esses::AtomicFile::write(target, std::string_view()).ok());
    CHECK(fs::file_size(target) == 0u);
}

TEST_CASE("atomic file: a mapped reader keeps its bytes when the file is replaced", "[unit][atomicfile]") {
    esses::test::TempDir dir("atomic-mapped");
    const fs::path target = dir.path() / "session.ld";
    REQUIRE(esses::AtomicFile::write(target, std::string_view("old contents")).ok());
    esses::Result<esses::MappedFile> mapped = esses::MappedFile::open(target);
    REQUIRE(mapped.ok());
    // Rename-over is permitted by FILE_SHARE_DELETE (02 section 1 behaviour 5).
    const esses::Result<void> w = esses::AtomicFile::write(target, std::string_view("new contents!"));
    REQUIRE(w.ok());
    CHECK(std::string(reinterpret_cast<const char*>(mapped.value().bytes().data()), mapped.value().size()) ==
          "old contents");
    CHECK(mapped.value().changedOnDisk());
    CHECK(readAll(target) == "new contents!");
}

// On Windows the rename goes through a handle with delete access only, closed a moment after the rename: a
// MappedFile opened in that moment (it shares read and delete access, never write access) is not refused.
// Through the writing handle, which holds write access and shared nothing, about one open in ten failed with
// a sharing violation while another thread replaced the file in a loop. A name missing while the rename is
// under way is looked for again (read_file.hpp).
TEST_CASE("atomic file: a mapped reader opens the file at every instant of a replacement",
          "[unit][atomicfile]") {
    esses::test::TempDir dir("atomic-concurrent");
    const fs::path target = dir.path() / "session.ld";
    const std::string first(4096, 'a');
    const std::string second(5000, 'b');
    REQUIRE(esses::AtomicFile::write(target, first).ok());

#if defined(ESSES_OS_WINDOWS)
    // The handle states of the moment after the rename, held still: the renaming handle (delete access only)
    // lets the mapping open; a handle with write access refuses it however it shares (MappedFile never shares
    // write access, 02 section 1 behaviour 5), which is why the writing handle is closed before the rename.
    for (const DWORD access : {DWORD{DELETE}, DWORD{GENERIC_WRITE | DELETE}}) {
        const HANDLE holder =
            CreateFileW(target.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(holder != INVALID_HANDLE_VALUE);
        const bool opened = esses::MappedFile::open(target).ok();
        CloseHandle(holder);
        CHECK(opened == (access == DELETE));
    }
#endif

    std::atomic<bool> stop{false};
    std::atomic<int> writes{0};
    std::atomic<int> writeFailures{0};
    std::thread writer([&] {
        for (int i = 0; !stop.load(); ++i) {
            if (esses::AtomicFile::write(target, i % 2 == 0 ? second : first).ok()) {
                ++writes;
            } else {
                ++writeFailures;
            }
        }
    });
    int opens = 0;
    int openFailures = 0;
    int wrongBytes = 0;
    // At least 2000 opens, overlapping at least 50 replacements. Whether every replacement succeeds is
    // AtomicFile's concern (the read-file test): with readers mapping the file without a pause, a heavily
    // loaded machine can refuse one for longer than AtomicFile retries (1 of 30 runs with ten copies at
    // once).
    while (opens < 2000 || (writes.load() < 50 && writeFailures.load() < 1000)) {
        const esses::Result<esses::MappedFile> mapped = esses::MappedFile::open(target);
        ++opens;
        if (!mapped.ok()) {
            ++openFailures;
            continue;
        }
        const std::string bytes(reinterpret_cast<const char*>(mapped->bytes().data()), mapped->size());
        if (bytes != first && bytes != second) {
            ++wrongBytes;
        }
    }
    stop = true;
    writer.join();
    CHECK(openFailures == 0);
    CHECK(wrongBytes == 0);
    CHECK(writes.load() >= 50);
}

TEST_CASE("atomic file: failures are reported and clean up", "[unit][atomicfile]") {
    esses::test::TempDir dir("atomic-fail");
    const esses::Result<void> r =
        esses::AtomicFile::write(dir.path() / "missing-dir" / "x.json", std::string_view("x"));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == esses::ErrorCode::Io);
    CHECK(countEntries(dir.path()) == 0u);
    CHECK_FALSE(esses::AtomicFile::write(fs::path(), std::string_view("x")).ok());
}
