// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ByteSource (05 section 1.1): spans clamp, the quick hash is MappedFile's stamp plus the format header and
// ignores the file name and mtime (D-130), the content hash is the full xxh3-128, and siblings drive
// companion discovery.
#include "formats_test_support.hpp"

#include <esses/base/hash.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::test;
namespace fs = std::filesystem;

namespace {

std::vector<std::byte> pattern(std::size_t n) {
    std::vector<std::byte> v(n);
    std::uint32_t x = 2463534242u;
    for (auto& b : v) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<std::byte>(x & 0xFFu);
    }
    return v;
}

} // namespace

TEST_CASE("byte source: memory sources clamp spans and hash like the file algorithm", "[unit][byte_source]") {
    const std::vector<std::byte> bytes = pattern(3000);
    const auto src = makeMemorySource(bytes, "run.ld", {fs::path("run.ldx")});
    CHECK(src->size() == 3000);
    CHECK(src->path() == fs::path("run.ld"));
    CHECK(src->span(0, 10).size() == 10);
    CHECK(src->span(2995, 100).size() == 5);
    CHECK(src->span(3000, 1).empty());
    CHECK(src->span(std::uint64_t{1} << 40, 1).empty());
    CHECK(src->buffer() != nullptr);
    CHECK(src->mappedFile() == nullptr);
    CHECK(src->siblings() == std::vector<fs::path>{fs::path("run.ldx")});
    CHECK(src->quickHash() == quickHashOf(bytes));
    const std::vector<std::byte> header = pattern(124);
    CHECK(src->quickHash(header) == quickHashOf(bytes, header));
    CHECK(src->quickHash(header) != src->quickHash());
    auto content = src->contentHash({});
    REQUIRE(content.ok());
    CHECK(*content == hash128(std::span<const std::byte>(bytes)));
    CancelSource cancel;
    cancel.cancel();
    CHECK(src->contentHash(cancel.token()).error().code == ErrorCode::Cancelled);
}

TEST_CASE("byte source: a mapped file's identity covers size, both MiB windows and the header",
          "[unit][byte_source]") {
    ld::test::TempDir dir("bytesource");
    const std::vector<std::byte> bytes = pattern((std::size_t{3} << 20) + 77);
    writeBytes(dir.path() / "big.ld", bytes);
    auto src = openByteSource(dir.path() / "big.ld");
    REQUIRE(src.ok());
    const ByteSource& s = **src;
    CHECK(s.size() == bytes.size());
    CHECK(s.mappedFile() != nullptr);
    CHECK(s.buffer() == nullptr);
    CHECK(s.mtimeUnixUs() != 0);
    CHECK(s.quickHash() == s.mappedFile()->stampAtOpen().quick);
    CHECK(s.quickHash() == quickHashOf(bytes));
    const std::vector<std::byte> header(bytes.begin(), bytes.begin() + 0x6E2);
    CHECK(s.quickHash(header) == quickHashOf(bytes, header));
    auto content = s.contentHash({});
    REQUIRE(content.ok());
    CHECK(*content == hash128(std::span<const std::byte>(bytes)));

    // A byte in the middle (outside both windows) changes the content hash, not the quick hash.
    std::vector<std::byte> middle = bytes;
    middle[middle.size() / 2] ^= std::byte{1};
    CHECK(quickHashOf(middle, header) == s.quickHash(header));
    CHECK(hash128(std::span<const std::byte>(middle)) != *content);

    // A copy under another name has the same identity (mtime and path are not part of it).
    fs::copy_file(dir.path() / "big.ld", dir.path() / "copy.ld");
    auto copy = openByteSource(dir.path() / "copy.ld");
    REQUIRE(copy.ok());
    CHECK((*copy)->quickHash(header) == s.quickHash(header));

    CHECK(openByteSource(dir.path() / "missing.ld").error().code == ErrorCode::Io);
}

TEST_CASE("byte source: siblings share the stem and find the companion", "[unit][byte_source]") {
    ld::test::TempDir dir("siblings");
    for (const char* name :
         {"run.ld", "run.ldx", "run.mp4", "run.ld.sidecar.json", "other.ldx", "runs.ldx"}) {
        writeText(dir.path() / name, "x");
    }
    const std::vector<fs::path> sib = listSiblings(dir.path() / "run.ld");
    REQUIRE(sib.size() == 2);
    CHECK(sib[0].filename() == fs::path("run.ldx"));
    CHECK(sib[1].filename() == fs::path("run.mp4"));
    const auto ldx = findCompanion(sib, dir.path() / "run.ld", ".ldx");
    REQUIRE(ldx.has_value());
    CHECK(ldx->filename() == fs::path("run.ldx"));
    CHECK_FALSE(findCompanion(sib, dir.path() / "run.ld", ".csv").has_value());
    auto src = openByteSource(dir.path() / "run.ld");
    REQUIRE(src.ok());
    CHECK((*src)->siblings() == sib);
    CHECK(listSiblings(dir.path() / "nowhere" / "x.ld").empty());

#if defined(ESSES_OS_WINDOWS)
    // Companion discovery is case-insensitive on Windows (05 section 3.1).
    writeText(dir.path() / "Lap.LD", "x");
    writeText(dir.path() / "lap.LDX", "x");
    const auto upper = findCompanion(listSiblings(dir.path() / "Lap.LD"), dir.path() / "Lap.LD", ".ldx");
    REQUIRE(upper.has_value());
    CHECK(upper->filename() == fs::path("lap.LDX"));
#endif
}
