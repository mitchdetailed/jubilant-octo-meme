// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "test_support.hpp"

#include <esses/base/mapped_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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

using esses::MappedFile;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

struct SumJob {
    std::span<const std::byte> bytes;
    unsigned long long sum;
};

void sumDecode(void* ctx) {
    auto* job = static_cast<SumJob*>(ctx);
    for (const std::byte b : job->bytes) {
        job->sum += static_cast<unsigned char>(b);
    }
}

#if defined(ESSES_OS_WINDOWS)
struct FaultJob {
    const std::byte* address;
};
// Simulates what the memory manager raises when a mapped page cannot be read (dropped share, truncation).
void faultDecode(void* ctx) {
    const auto* job = static_cast<FaultJob*>(ctx);
    const ULONG_PTR args[3] = {0, reinterpret_cast<ULONG_PTR>(job->address),
                               static_cast<ULONG_PTR>(0xC000009C)};
    RaiseException(EXCEPTION_IN_PAGE_ERROR, 0, 3, args);
}
#else
struct TouchJob {
    const volatile std::byte* address;
    unsigned value;
};
void touchDecode(void* ctx) {
    auto* job = static_cast<TouchJob*>(ctx);
    job->value = static_cast<unsigned>(job->address[0]); // SIGBUS: the page is beyond the truncated end
}
#endif

} // namespace

TEST_CASE("mapped file: maps the whole file read-only", "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped");
    const fs::path p = dir.path() / "data.bin";
    std::string content(3 * 1024 * 1024 + 17, '\0');
    for (std::size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<char>(i * 31u);
    }
    writeFile(p, content);
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    MappedFile mf = std::move(r).value();
    CHECK(mf.isOpen());
    REQUIRE(mf.size() == content.size());
    CHECK(std::memcmp(mf.bytes().data(), content.data(), content.size()) == 0);
    CHECK(mf.stampAtOpen().size == content.size());
    CHECK(mf.stampAtOpen().mtimeUnixUs > 0);
    CHECK_FALSE(mf.changedOnDisk());

    // The quick hash covers the size and the first and last MiB.
    esses::Hash128Stream expected;
    expected.updateU64(content.size());
    expected.update(std::string_view(content).substr(0, 1u << 20));
    expected.update(std::string_view(content).substr(content.size() - (1u << 20)));
    CHECK(mf.stampAtOpen().quick == expected.digest());

    SumJob job{mf.bytes(), 0};
    REQUIRE(mf.guarded(&sumDecode, &job).ok());
    unsigned long long reference = 0;
    for (const char c : content) {
        reference += static_cast<unsigned char>(c);
    }
    CHECK(job.sum == reference);

    MappedFile moved = std::move(mf);
    CHECK(moved.size() == content.size());
    CHECK_FALSE(mf.isOpen()); // NOLINT(bugprone-use-after-move): moved-from state is specified
    moved.close();
    CHECK(moved.bytes().empty());
}

TEST_CASE("mapped file: empty and missing files", "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-empty");
    const fs::path empty = dir.path() / "empty.bin";
    writeFile(empty, "");
    esses::Result<MappedFile> r = MappedFile::open(empty);
    REQUIRE(r.ok());
    CHECK(r.value().size() == 0u);
    CHECK(r.value().bytes().empty());
    CHECK(r.value().stampAtOpen().size == 0u);

    const esses::Result<MappedFile> missing = MappedFile::open(dir.path() / "missing.bin");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == esses::ErrorCode::Io);
    REQUIRE(missing.error().context.path.has_value());
    CHECK(missing.error().context.path->filename() == "missing.bin");
}

TEST_CASE("mapped file: changedOnDisk detects a new mtime or size", "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-change");
    const fs::path p = dir.path() / "log.bin";
    writeFile(p, std::string(4096, 'x'));
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    const MappedFile& mf = r.value();
    CHECK_FALSE(mf.changedOnDisk());
    // Changing only the timestamp needs no write access to the data, so it works while mapped.
    fs::last_write_time(p, fs::last_write_time(p) - std::chrono::hours(1));
    CHECK(mf.changedOnDisk());
}

#if defined(ESSES_OS_WINDOWS)
TEST_CASE("mapped file: in-place writers get a sharing error while mapped", "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-share");
    const fs::path p = dir.path() / "log.bin";
    writeFile(p, std::string(4096, 'x'));
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    HANDLE w = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD err = GetLastError();
    CHECK(w == INVALID_HANDLE_VALUE);
    CHECK(err == ERROR_SHARING_VIOLATION);
    if (w != INVALID_HANDLE_VALUE) {
        CloseHandle(w);
    }
    // Readers are fine.
    esses::Result<MappedFile> second = MappedFile::open(p);
    CHECK(second.ok());
}

TEST_CASE("mapped file: a page fault inside the view becomes fail(Io)", "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-fault");
    const fs::path p = dir.path() / "log.bin";
    writeFile(p, std::string(8192, 'x'));
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    FaultJob job{r.value().bytes().data() + 4096};
    const esses::Result<void> g = r.value().guarded(&faultDecode, &job);
    REQUIRE_FALSE(g.ok());
    CHECK(g.error().code == esses::ErrorCode::Io);
    CHECK(g.error().message == "page fault while reading a mapped file");
    // Still usable afterwards.
    SumJob sum{r.value().bytes(), 0};
    CHECK(r.value().guarded(&sumDecode, &sum).ok());
    CHECK(sum.sum == 8192ull * 'x');
}
#else
TEST_CASE("mapped file: a page fault inside the view becomes fail(Io)", "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-fault");
    const fs::path p = dir.path() / "log.bin";
    const long page = ::sysconf(_SC_PAGESIZE);
    writeFile(p, std::string(static_cast<std::size_t>(page) * 4, 'x'));
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    fs::resize_file(p, 0); // a writer that ignores the sharing contract truncates the file
    TouchJob job{r.value().bytes().data() + page * 2, 0};
    const esses::Result<void> g = r.value().guarded(&touchDecode, &job);
    REQUIRE_FALSE(g.ok());
    CHECK(g.error().code == esses::ErrorCode::Io);
    CHECK(r.value().changedOnDisk());
}
#endif
