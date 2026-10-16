// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "test_support.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/base/read_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <fstream>
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
#else
#include <unistd.h>
#endif

using esses::AtomicFile;
using esses::ErrorCode;
using esses::readFileHead;
using esses::readWholeFile;
using esses::requireRegularFile;
using esses::Result;
namespace fs = std::filesystem;

namespace {

void writeBytes(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("read file: the bytes of a regular file, in binary mode and across read blocks",
          "[unit][readfile]") {
    esses::test::TempDir dir("readfile");
    const fs::path small = dir.path() / "small.json";
    // CR LF, NUL and Ctrl-Z (end of file for a Windows text-mode stream) come through unchanged.
    const std::string text = std::string("a\r\nb") + '\0' + "c\x1A" + "d\n";
    REQUIRE(text.size() == 9);
    writeBytes(small, text);
    Result<std::string> r = readWholeFile(small);
    REQUIRE(r.ok());
    CHECK(*r == text);

    // Larger than one 64 KiB read block, and not a multiple of it.
    std::string large(3 * 65536 + 17, '\0');
    std::uint32_t state = 0x2026'1014u;
    for (char& c : large) {
        state = state * 1664525u + 1013904223u; // a fixed LCG: every byte value occurs
        c = static_cast<char>(state >> 24);
    }
    const fs::path big = dir.path() / "big.bin";
    writeBytes(big, large);
    r = readWholeFile(big, "the big file");
    REQUIRE(r.ok());
    CHECK(r->size() == large.size());
    CHECK(*r == large);
    CHECK(requireRegularFile(big).ok());

    // The head: a limit inside one block, across blocks, beyond the end, and zero.
    for (const std::size_t limit :
         {std::size_t{100}, std::size_t{65536 + 5}, large.size() + 1, std::size_t{0}}) {
        r = readFileHead(big, limit, "the big file");
        REQUIRE(r.ok());
        CHECK(*r == large.substr(0, limit));
    }
    r = readFileHead(small, 4);
    REQUIRE(r.ok());
    CHECK(*r == "a\r\nb");
}

TEST_CASE("read file: an empty file is an empty string", "[unit][readfile]") {
    esses::test::TempDir dir("readfile-empty");
    const fs::path empty = dir.path() / "empty.json";
    writeBytes(empty, std::string());
    const Result<std::string> r = readWholeFile(empty);
    REQUIRE(r.ok());
    CHECK(r->empty());
}

TEST_CASE("read file: a missing file is an Io error naming it", "[unit][readfile]") {
    esses::test::TempDir dir("readfile-missing");
    for (const fs::path& missing : {dir.path() / "none.json", dir.path() / "no-folder" / "none.json"}) {
        Result<std::string> r = readWholeFile(missing);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Io);
        CHECK(r.error().context.path == missing);
        CHECK(r.error().message.starts_with("cannot open '"));
        r = readWholeFile(missing, "the unit table");
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message == "cannot open the unit table");
        const Result<void> regular = requireRegularFile(missing);
        REQUIRE_FALSE(regular.ok());
        CHECK(regular.error().code == ErrorCode::Io);
        CHECK(regular.error().context.path == missing);
    }
    // A regular file where a folder of the path belongs.
    writeBytes(dir.path() / "file", "x");
    const Result<std::string> r = readWholeFile(dir.path() / "file" / "none.json");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Io);
    const Result<std::string> head = readFileHead(dir.path() / "none.json", 16, "the head");
    REQUIRE_FALSE(head.ok());
    CHECK(head.error().code == ErrorCode::Io);
    CHECK(head.error().message == "cannot open the head");
}

TEST_CASE("read file: a directory is an Io error on every standard library, never an exception",
          "[unit][readfile]") {
    // MSVC refuses to open a directory; on POSIX the stream opens it, then libstdc++ throws from the read and
    // libc++ reads it as an empty file. The type check comes first, so the result is the same everywhere.
    esses::test::TempDir dir("readfile-dir");
    const fs::path folder = dir.path() / "document.json";
    fs::create_directories(folder);
    Result<std::string> r = esses::fail(ErrorCode::Internal, "not called");
    REQUIRE_NOTHROW(r = readWholeFile(folder));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Io);
    CHECK(r.error().context.path == folder);
    CHECK(r.error().message.ends_with("' is not a regular file"));
    REQUIRE_NOTHROW(r = readWholeFile(folder, "the settings schema"));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message == "the settings schema is not a regular file");
    const Result<void> regular = requireRegularFile(folder);
    REQUIRE_FALSE(regular.ok());
    CHECK(regular.error().code == ErrorCode::Io);
    CHECK(regular.error().context.path == folder);
    REQUIRE_NOTHROW(r = readFileHead(folder, 16, "the head"));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message == "the head is not a regular file");
    CHECK(fs::is_directory(folder)); // untouched
}

TEST_CASE("read file: a file that cannot be opened is an Io error", "[unit][readfile]") {
    esses::test::TempDir dir("readfile-locked");
    const fs::path locked = dir.path() / "locked.json";
    writeBytes(locked, "{}\n");
#if defined(ESSES_OS_WINDOWS)
    // Another process holds the file without sharing it: the open fails with a sharing violation.
    const HANDLE holder =
        CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(holder != INVALID_HANDLE_VALUE);
    const Result<std::string> r = readWholeFile(locked, "the locked file");
    CloseHandle(holder);
#else
    // No read permission. The superuser reads it all the same, so the case cannot be made there.
    if (::geteuid() == 0) {
        SKIP("running as the superuser: permissions do not stop the open");
    }
    fs::permissions(locked, fs::perms::none);
    const Result<std::string> r = readWholeFile(locked, "the locked file");
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_write);
#endif
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Io);
    CHECK(r.error().context.path == locked);
    CHECK(r.error().message == "cannot open the locked file");
    CHECK(requireRegularFile(locked).ok()); // it is a regular file; only the open fails
    const Result<std::string> after = readWholeFile(locked);
    REQUIRE(after.ok());
    CHECK(*after == "{}\n");
}

// Regression (Windows): the read went through a std::ifstream, which shares read and write access but not
// delete access, and AtomicFile renamed its temporary into place through its writing handle, opened without
// sharing and closed a moment after the rename. A read in that moment failed with a sharing violation (about
// one read in ten while another thread replaced the file in a loop), and an open stream made the rename fail.
// Now the read shares delete access, the rename goes through a handle with delete access only, and a name
// missing while the rename is under way is looked for again (read_file.hpp).
TEST_CASE("read file: a document that AtomicFile replaces reads whole at every instant", "[unit][readfile]") {
    esses::test::TempDir dir("readfile-atomic");
    const fs::path target = dir.path() / "document.json";
    const std::string first = std::string(300, 'a') + "\n";
    const std::string second = std::string(600, 'b') + "\n";
    const auto versionAt = [&](int i) -> const std::string& { return i % 2 == 0 ? first : second; };
    REQUIRE(AtomicFile::write(target, first).ok());

#if defined(ESSES_OS_WINDOWS)
    // Another handle holds the file, sharing everything: a writer's (write and delete access), or the one
    // through which AtomicFile renamed the file into place (delete access only). The read shares delete
    // access too, so neither refuses it; a std::ifstream is refused by both, every time.
    for (const DWORD access : {DWORD{GENERIC_WRITE | DELETE}, DWORD{DELETE}}) {
        const HANDLE holder =
            CreateFileW(target.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(holder != INVALID_HANDLE_VALUE);
        const Result<std::string> held = readWholeFile(target);
        const Result<std::string> heldHead = readFileHead(target, 3);
        CloseHandle(holder);
        CHECK(held.ok());
        CHECK(held.valueOr(std::string()) == first);
        CHECK(heldHead.ok());
        CHECK(heldHead.valueOr(std::string()) == "aaa");
    }
    // The other way round: a handle opened as the read opens its own (read access, sharing everything) does
    // not keep a replacement out, and it goes on reading the old bytes.
    {
        const HANDLE reader =
            CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(reader != INVALID_HANDLE_VALUE);
        const bool replaced = AtomicFile::write(target, second).ok();
        char old[3] = {};
        DWORD got = 0;
        const bool readOld = ReadFile(reader, old, sizeof old, &got, nullptr) != 0;
        CloseHandle(reader);
        CHECK(replaced);
        CHECK(readOld);
        CHECK(std::string(old, got) == "aaa");
        CHECK(readWholeFile(target).valueOr(std::string()) == second);
    }
#endif

    // Written and read back at once, again and again.
    int writeFailures = 0;
    int readFailures = 0;
    int wrongBytes = 0;
    for (int i = 0; i < 500; ++i) {
        if (!AtomicFile::write(target, versionAt(i)).ok()) {
            ++writeFailures;
            continue;
        }
        const Result<std::string> r = readWholeFile(target);
        if (!r.ok()) {
            ++readFailures;
        } else if (*r != versionAt(i)) {
            ++wrongBytes;
        }
    }
    CHECK(writeFailures == 0);
    CHECK(readFailures == 0);
    CHECK(wrongBytes == 0);

    // Read while another thread replaces the file in a loop: every read gives one whole version.
    std::atomic<bool> stop{false};
    std::atomic<int> writes{0};
    std::atomic<int> concurrentWriteFailures{0};
    std::thread writer([&] {
        for (int i = 0; !stop.load(); ++i) {
            if (AtomicFile::write(target, versionAt(i)).ok()) {
                ++writes;
            } else {
                ++concurrentWriteFailures;
            }
        }
    });
    int reads = 0;
    int concurrentReadFailures = 0;
    wrongBytes = 0;
    // At least 2000 reads, overlapping at least 50 replacements. That an open reader does not keep a
    // replacement out is checked above, with the handle held still; with a reader that never pauses, a
    // heavily loaded machine can still refuse one replacement for longer than AtomicFile retries (once in 20
    // runs at
    // --parallel 16 beside other builds), so this loop counts the replacements that went through.
    while (reads < 2000 || (writes.load() < 50 && concurrentWriteFailures.load() < 1000)) {
        const Result<std::string> r = readWholeFile(target);
        ++reads;
        if (!r.ok()) {
            ++concurrentReadFailures;
        } else if (*r != first && *r != second) {
            ++wrongBytes;
        }
    }
    stop = true;
    writer.join();
    CHECK(concurrentReadFailures == 0);
    CHECK(wrongBytes == 0);
    CHECK(writes.load() >= 50);
}
