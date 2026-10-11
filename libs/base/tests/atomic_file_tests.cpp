// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "test_support.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/base/mapped_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <sstream>

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

TEST_CASE("atomic file: failures are reported and clean up", "[unit][atomicfile]") {
    esses::test::TempDir dir("atomic-fail");
    const esses::Result<void> r =
        esses::AtomicFile::write(dir.path() / "missing-dir" / "x.json", std::string_view("x"));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == esses::ErrorCode::Io);
    CHECK(countEntries(dir.path()) == 0u);
    CHECK_FALSE(esses::AtomicFile::write(fs::path(), std::string_view("x")).ok());
}
