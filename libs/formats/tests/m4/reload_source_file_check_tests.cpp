// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// formats::checkSourceFile (M4 reload package, AC-M4-10; 02 section 1 behaviour 5, 06 section 6 "Changed
// files"): the re-stat of an open source against the size and modification time its open recorded. The
// expectations come from what the test does to the file (the byte counts it writes, the exact hour it moves
// the modification time by), never from the function's own earlier answers except as the baseline it is asked
// to compare against.

#include "m4/reload_test_support.hpp"

#include <esses/base/mapped_file.hpp>
#include <esses/formats/byte_source.hpp>
#include <esses/formats/source_file_check.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

using namespace esses;
using namespace esses::formats;
using esses::formats::reload_test::renameOver;
using esses::formats::reload_test::shiftMtime;
using esses::formats::reload_test::writeBytes;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kHourUs = 3'600'000'000LL;

class Dir {
public:
    explicit Dir(const std::string& tag) {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("esses-formats-reload-" + tag + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++counter));
        fs::create_directories(path_);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// The provenance the open pipeline records for a file (registry step 3: path, size and mtime from the
// ByteSource, i.e. from the mapping's stamp); the mapping is closed again before the test touches the file.
model::SourceProvenance provenanceOf(const fs::path& p) {
    auto source = openByteSource(p);
    REQUIRE(source.ok());
    model::SourceProvenance prov;
    prov.path = p;
    prov.size = source.value()->size();
    prov.mtimeUnixUs = source.value()->mtimeUnixUs();
    return prov;
}

} // namespace

TEST_CASE("reload: an untouched file is unchanged and reports its size", "[unit][m4][reload]") {
    Dir dir("untouched");
    const fs::path p = dir.path() / "session.ld";
    writeBytes(p, std::string(1234, 'a'));
    const model::SourceProvenance prov = provenanceOf(p);
    CHECK(prov.size == 1234u);
    const SourceFileCheck c = checkSourceFile(prov);
    CHECK(c.state == SourceFileState::Unchanged);
    CHECK(c.size == 1234u);
    CHECK(c.mtimeUnixUs == prov.mtimeUnixUs);
    CHECK(sourceFileStateName(c.state) == "unchanged");
}

TEST_CASE("reload: a new modification time alone is a change", "[unit][m4][reload]") {
    Dir dir("mtime");
    const fs::path p = dir.path() / "session.ld";
    writeBytes(p, std::string(4096, 'b'));
    const model::SourceProvenance prov = provenanceOf(p);
    REQUIRE(shiftMtime(p, std::chrono::hours(1)));
    const SourceFileCheck c = checkSourceFile(prov);
    CHECK(c.state == SourceFileState::Changed);
    CHECK(c.size == 4096u);
    CHECK(c.mtimeUnixUs - prov.mtimeUnixUs == kHourUs); // exactly the hour the test moved it by
    CHECK(sourceFileStateName(c.state) == "changed");
    // Moved back to the recorded time, the file reads as unchanged again (the check holds no state).
    REQUIRE(shiftMtime(p, -std::chrono::hours(1)));
    CHECK(checkSourceFile(prov).state == SourceFileState::Unchanged);
}

TEST_CASE("reload: a replacement of another size is a change, even at the same mtime", "[unit][m4][reload]") {
    Dir dir("size");
    const fs::path p = dir.path() / "session.ld";
    const fs::path next = dir.path() / "session.ld.tmp";
    writeBytes(p, std::string(4096, 'c'));
    // A whole second (some standard libraries set file times to the second only), an hour back.
    const fs::file_time_type oldTime =
        std::chrono::floor<std::chrono::seconds>(fs::last_write_time(p)) - std::chrono::hours(1);
    fs::last_write_time(p, oldTime);
    const model::SourceProvenance prov = provenanceOf(p);
    // A writer that keeps the old modification time (a copy that preserves it) but not the size.
    writeBytes(next, std::string(4100, 'd'));
    fs::last_write_time(next, oldTime);
    REQUIRE(renameOver(next, p));
    const SourceFileCheck c = checkSourceFile(prov);
    REQUIRE(c.mtimeUnixUs == prov.mtimeUnixUs); // the precondition: the same modification time
    CHECK(c.state == SourceFileState::Changed);
    CHECK(c.size == 4100u);
}

TEST_CASE("reload: a file replaced while mapped is a change; the mapping keeps the old bytes",
          "[unit][m4][reload]") {
    Dir dir("mapped");
    const fs::path p = dir.path() / "session.ld";
    const fs::path next = dir.path() / "session.ld.tmp";
    writeBytes(p, std::string(3000, 'e'));
    REQUIRE(shiftMtime(p, -std::chrono::hours(1))); // so that the replacement is newer on any file system
    auto source = openByteSource(p);
    REQUIRE(source.ok());
    model::SourceProvenance prov;
    prov.path = p;
    prov.size = source.value()->size();
    prov.mtimeUnixUs = source.value()->mtimeUnixUs();
    writeBytes(next, std::string(3000, 'f')); // the same size: only the modification time differs
    REQUIRE(renameOver(next, p));
    CHECK(checkSourceFile(prov).state == SourceFileState::Changed);
    const std::shared_ptr<const MappedFile> mapping = source.value()->mappedFile();
    REQUIRE(mapping != nullptr);
    CHECK(mapping->changedOnDisk());
    CHECK(static_cast<char>(source.value()->span(0, 1)[0]) == 'e');
}

TEST_CASE("reload: a deleted or renamed-away file is missing", "[unit][m4][reload]") {
    Dir dir("missing");
    const fs::path p = dir.path() / "session.ld";
    writeBytes(p, std::string(10, 'g'));
    const model::SourceProvenance prov = provenanceOf(p);
    fs::rename(p, dir.path() / "elsewhere.ld");
    const SourceFileCheck c = checkSourceFile(prov);
    CHECK(c.state == SourceFileState::Missing);
    CHECK(c.size == 0u);
    CHECK(c.mtimeUnixUs == 0);
    CHECK(sourceFileStateName(c.state) == "missing");
    // The path overload answers the same.
    CHECK(checkSourceFile(prov.path, prov.size, prov.mtimeUnixUs).state == SourceFileState::Missing);
}

TEST_CASE("reload: a source without a path has nothing to check", "[unit][m4][reload]") {
    model::SourceProvenance prov; // an in-memory source
    prov.size = 77;
    prov.mtimeUnixUs = 5;
    const SourceFileCheck c = checkSourceFile(prov);
    CHECK(c.state == SourceFileState::Unchanged);
    CHECK(c.size == 77u);
    CHECK(c.mtimeUnixUs == 5);
}
