// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MappedFile (02 section 1 behaviour 5) and its sharing policy, 02 section 8 AC-4a (AC-M4-10, E-5): a writer
// that replaces the file by rename-over succeeds while the view keeps the old bytes; an in-place truncating
// writer gets a sharing error (Windows; POSIX has no mandatory sharing, so guarded() and changedOnDisk()
// catch it instead); a fault inside guarded() is fail(Io); changedOnDisk() flips after a replace. The writers
// are written here with the operating system's own calls, not through AtomicFile, so the policy is checked
// against writers that are not ours. "mapped file: AC-4a on an SMB share" runs the same checks in
// ESSES_TEST_SMB_DIR (a directory on a share, Windows) and skips when it is not set (docs/milestones/M4.md
// section 6.2).
#include "test_support.hpp"

#include <esses/base/env.hpp>
#include <esses/base/mapped_file.hpp>
#include <esses/base/read_file.hpp>
#include <esses/base/utf8.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
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
#include <cstdio>

#include <unistd.h>
#endif

using esses::MappedFile;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// The file's bytes now ("" when it cannot be read), through readWholeFile (which shares delete access, so it
// never stands in a writer's way).
std::string readAll(const fs::path& p) {
    esses::Result<std::string> bytes = esses::readWholeFile(p);
    return bytes.ok() ? std::move(bytes).value() : std::string();
}

std::string viewOf(const MappedFile& mf) {
    return std::string(reinterpret_cast<const char*>(mf.bytes().data()), mf.size());
}

// Moves the file's modification time by `delta` through its attributes only: on Windows a handle with
// FILE_WRITE_ATTRIBUTES, which no share mode refuses, so it works while the file is mapped (std::filesystem's
// last_write_time asks for write access with some standard libraries); elsewhere std::filesystem.
bool shiftMtime(const fs::path& p, std::chrono::seconds delta) {
#if defined(ESSES_OS_WINDOWS)
    const HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILETIME ft{};
    bool done = GetFileTime(h, nullptr, nullptr, &ft) != 0;
    if (done) {
        ULARGE_INTEGER t{};
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        t.QuadPart = static_cast<ULONGLONG>(static_cast<LONGLONG>(t.QuadPart) +
                                            static_cast<LONGLONG>(delta.count()) * 10'000'000LL);
        ft.dwLowDateTime = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        done = SetFileTime(h, nullptr, nullptr, &ft) != 0;
    }
    CloseHandle(h);
    return done;
#else
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) {
        return false;
    }
    fs::last_write_time(p, t + delta, ec);
    return !ec;
#endif
}

// A log of `content` whose modification time lies an hour back, so that any later write is newer even on a
// file system whose timestamps are coarser than the time between two writes of a test.
void writeOldLog(const fs::path& p, const std::string& content) {
    writeFile(p, content);
    REQUIRE(shiftMtime(p, -std::chrono::hours(1)));
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

unsigned long long sumOf(const std::string& s) {
    unsigned long long sum = 0;
    for (const char c : s) {
        sum += static_cast<unsigned char>(c);
    }
    return sum;
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

// Runs one file-system step again for up to 500 ms while it is refused with ERROR_ACCESS_DENIED or
// ERROR_SHARING_VIOLATION: a virus scanner or the indexer may hold a file just written for a moment (as
// AtomicFile retries). The step's last Win32 error, 0 on success.
DWORD retrying(const std::function<DWORD()>& step) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
        const DWORD error = step();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) ||
            std::chrono::steady_clock::now() >= deadline) {
            return error;
        }
        Sleep(1);
    }
}

// The atomic writers of E-5, each replacing `target` by `replacement` (a finished file beside it).
enum class Writer : std::uint8_t {
    PosixRename,   // SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS | POSIX_SEMANTICS)
    ReplaceFile,   // ReplaceFileW
    RenameAwayMove // the target renamed aside first, then the replacement moved in (no replace)
};

const char* writerName(Writer w) {
    switch (w) {
    case Writer::PosixRename:
        return "POSIX-semantics rename";
    case Writer::ReplaceFile:
        return "ReplaceFileW";
    case Writer::RenameAwayMove:
        return "rename aside, then move in";
    }
    return "?";
}

// FileRenameInfoEx (Windows 10 1709+), spelled by its value: MinGW's headers declare it only for a newer
// _WIN32_WINNT than they default to.
const auto kFileRenameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);

DWORD posixRename(const fs::path& replacement, const fs::path& target) {
    const HANDLE h = CreateFileW(replacement.c_str(), DELETE | SYNCHRONIZE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    const std::wstring name = fs::absolute(target).native();
    const std::size_t nameBytes = name.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(info->FileName, name.c_str(), nameBytes + sizeof(wchar_t));
    const BOOL done =
        SetFileInformationByHandle(h, kFileRenameInfoEx, info, static_cast<DWORD>(buffer.size()));
    const DWORD error = done != 0 ? 0 : GetLastError();
    CloseHandle(h);
    return error;
}

// 0 when the replacement is in place, else the Win32 error.
DWORD replaceBy(Writer w, const fs::path& replacement, const fs::path& target) {
    switch (w) {
    case Writer::PosixRename:
        return retrying([&] { return posixRename(replacement, target); });
    case Writer::ReplaceFile:
        return retrying([&] {
            return ReplaceFileW(target.c_str(), replacement.c_str(), nullptr, 0, nullptr, nullptr) != 0
                       ? DWORD{0}
                       : GetLastError();
        });
    case Writer::RenameAwayMove: {
        fs::path aside = target;
        aside += L".old";
        if (const DWORD error = retrying([&] {
                return MoveFileExW(target.c_str(), aside.c_str(), 0) != 0 ? DWORD{0} : GetLastError();
            });
            error != 0) {
            return error;
        }
        return retrying([&] {
            return MoveFileExW(replacement.c_str(), target.c_str(), 0) != 0 ? DWORD{0} : GetLastError();
        });
    }
    }
    return ERROR_INVALID_PARAMETER;
}

// An in-place writer: CreateFileW with write access and every share mode, as a logger download or a sim that
// rewrites its session would open the file; 0 when it opened (the handle is closed at once), else the error.
DWORD openForWriting(const fs::path& p, DWORD disposition) {
    const HANDLE h =
        CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    CloseHandle(h);
    return 0;
}

// The rename-over checks of AC-4a in `dir`: the view keeps the old bytes (and guarded() still reads them)
// whatever the writer did; when it succeeded the path holds the new file, changedOnDisk() flips and a new
// mapping sees the new bytes, and when it was refused nothing changed. Returns the writer's Win32 error (0 on
// success).
DWORD renameOverCase(const fs::path& dir, Writer w) {
    const fs::path target = dir / "session.ld";
    const fs::path replacement = dir / "session.ld.tmp";
    const std::string before(4096 + 17, 'o');
    const std::string after(5000, 'n');
    writeOldLog(target, before);
    esses::Result<MappedFile> r = MappedFile::open(target);
    REQUIRE(r.ok());
    const MappedFile& mf = r.value();
    REQUIRE_FALSE(mf.changedOnDisk());
    writeFile(replacement, after);
    const DWORD error = replaceBy(w, replacement, target);
    INFO(writerName(w) << ": Win32 error " << error);
    CHECK(viewOf(mf) == before);
    SumJob sum{mf.bytes(), 0};
    CHECK(mf.guarded(&sumDecode, &sum).ok());
    CHECK(sum.sum == sumOf(before));
    if (error == 0) {
        CHECK(readAll(target) == after);
        CHECK(mf.changedOnDisk());
        esses::Result<MappedFile> again = MappedFile::open(target);
        REQUIRE(again.ok());
        CHECK(viewOf(again.value()) == after);
        CHECK(again.value().stampAtOpen().size == after.size());
        CHECK_FALSE(again.value().stampAtOpen().quick == mf.stampAtOpen().quick);
    } else {
        CHECK(readAll(target) == before);
        CHECK_FALSE(mf.changedOnDisk());
    }
    std::error_code ec;
    fs::remove(replacement, ec);
    return error;
}

// The in-place checks of AC-4a in `dir`: every truncating or rewriting open fails with a sharing violation
// while the file is mapped (the file, the view and the stamp untouched) and goes through once the mapping is
// closed.
void inPlaceCase(const fs::path& dir) {
    const fs::path p = dir / "inplace.ld";
    const std::string content(8192, 'x');
    writeOldLog(p, content);
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    MappedFile& mf = r.value();
    for (const DWORD disposition :
         {DWORD{CREATE_ALWAYS}, DWORD{TRUNCATE_EXISTING}, DWORD{OPEN_EXISTING}, DWORD{OPEN_ALWAYS}}) {
        INFO("CreateFileW(GENERIC_WRITE), creation disposition " << disposition);
        CHECK(openForWriting(p, disposition) == DWORD{ERROR_SHARING_VIOLATION});
    }
    {
        std::ofstream rewrite(p, std::ios::binary | std::ios::trunc);
        CHECK_FALSE(rewrite.is_open());
    }
    CHECK(readAll(p) == content);
    CHECK(viewOf(mf) == content);
    CHECK_FALSE(mf.changedOnDisk());
    CHECK(MappedFile::open(p).ok()); // readers are fine
    // The refusal is the mapping's: once it is closed, the same writer goes through.
    mf.close();
    CHECK(retrying([&] { return openForWriting(p, TRUNCATE_EXISTING); }) == 0);
    CHECK(fs::file_size(p) == 0u);
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
    CHECK(job.sum == sumOf(content));

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
    REQUIRE(shiftMtime(p, -std::chrono::hours(1)));
    CHECK(mf.changedOnDisk());
}

TEST_CASE("mapped file: changedOnDisk is true once the path is gone", "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-gone");
    const fs::path p = dir.path() / "log.bin";
    writeFile(p, std::string(4096, 'g'));
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    const MappedFile& mf = r.value();
    REQUIRE_FALSE(mf.changedOnDisk());
    // Renamed aside (what a deleted file, a moved folder or a dropped share look like to a re-stat): the view
    // keeps its bytes, the path can no longer be stat'ed.
    std::error_code ec;
    fs::rename(p, dir.path() / "aside.bin", ec);
    REQUIRE_FALSE(ec);
    CHECK(mf.changedOnDisk());
    CHECK(viewOf(mf) == std::string(4096, 'g'));
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

// AC-4a (AC-M4-10) on NTFS: each rename-over writer that the sharing policy lets through.
TEST_CASE("mapped file: AC-4a a rename-over writer succeeds while the old bytes stay mapped",
          "[unit][mappedfile]") {
    for (const Writer w : {Writer::PosixRename, Writer::ReplaceFile, Writer::RenameAwayMove}) {
        DYNAMIC_SECTION(writerName(w)) {
            esses::test::TempDir dir("mapped-rename");
            CHECK(renameOverCase(dir.path(), w) == 0);
        }
    }
}

// E-5's answer for the classic replace: MoveFileExW(MOVEFILE_REPLACE_EXISTING) (and std::filesystem::rename,
// which uses it) does not replace a file that any handle holds open, whatever that handle's share mode, so it
// cannot replace a mapped log; the writer gets ERROR_ACCESS_DENIED and nothing changes. Either outcome keeps
// the view whole: the test pins that and reports which outcome this system gave.
TEST_CASE("mapped file: AC-4a a classic MoveFileExW replace changes nothing when it is refused",
          "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-classic");
    const fs::path target = dir.path() / "session.ld";
    const fs::path replacement = dir.path() / "session.ld.tmp";
    const std::string before(4096, 'o');
    const std::string after(4500, 'n');
    writeOldLog(target, before);
    esses::Result<MappedFile> r = MappedFile::open(target);
    REQUIRE(r.ok());
    writeFile(replacement, after);
    const DWORD error = retrying([&] {
        return MoveFileExW(replacement.c_str(), target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0
                   ? DWORD{0}
                   : GetLastError();
    });
    WARN("E-5: MoveFileExW(MOVEFILE_REPLACE_EXISTING) over a mapped file: "
         << (error == 0 ? std::string("replaced") : "refused, Win32 error " + std::to_string(error)));
    CHECK(viewOf(r.value()) == before);
    if (error == 0) {
        CHECK(readAll(target) == after);
        CHECK(r.value().changedOnDisk());
    } else {
        CHECK(error == DWORD{ERROR_ACCESS_DENIED});
        CHECK(readAll(target) == before);
        CHECK(readAll(replacement) == after);
        CHECK_FALSE(r.value().changedOnDisk());
    }
}

TEST_CASE("mapped file: AC-4a an in-place truncating writer gets a sharing error", "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-inplace");
    inPlaceCase(dir.path());
}

// The SMB half of AC-4a (docs/milestones/M4.md section 6.2): ESSES_TEST_SMB_DIR names a writable directory on
// an SMB share (for example \\server\share\esses-test). The in-place refusal must hold there as on NTFS; of
// the rename-over writers at least one must succeed with the old bytes kept, and each writer's result is
// reported (a refused writer must have changed nothing).
TEST_CASE("mapped file: AC-4a on an SMB share (ESSES_TEST_SMB_DIR)", "[unit][mappedfile][smb]") {
    const std::string share = esses::getEnv(esses::envName("TEST_SMB_DIR")).value_or(std::string());
    if (share.empty()) {
        SKIP("ESSES_TEST_SMB_DIR is not set (the SMB case of docs/milestones/M4.md section 6.2)");
    }
    const fs::path root = esses::pathFromUtf8(share) /
                          ("esses-ac4a-" + std::to_string(GetCurrentProcessId()) + "-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(root / "inplace", ec);
    REQUIRE_FALSE(ec);
    int replaced = 0;
    for (const Writer w : {Writer::PosixRename, Writer::ReplaceFile, Writer::RenameAwayMove}) {
        const fs::path dir = root / ("writer" + std::to_string(static_cast<int>(w)));
        fs::create_directories(dir, ec);
        const DWORD error = renameOverCase(dir, w);
        WARN("SMB: " << writerName(w) << ": "
                     << (error == 0 ? std::string("replaced")
                                    : "refused, Win32 error " + std::to_string(error)));
        replaced += error == 0 ? 1 : 0;
    }
    CHECK(replaced > 0);
    inPlaceCase(root / "inplace");
    fs::remove_all(root, ec);
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
    // Linux raises SIGBUS for the removed page; macOS may serve the page it already had resident (the open's
    // quick hash read every page) without a signal, which guarded() catches by re-checking the file size.
    TouchJob job{r.value().bytes().data() + page * 2, 0};
    const esses::Result<void> g = r.value().guarded(&touchDecode, &job);
    REQUIRE_FALSE(g.ok());
    CHECK(g.error().code == esses::ErrorCode::Io);
    CHECK(r.value().changedOnDisk());
}

TEST_CASE("mapped file: a truncation is fail(Io) even when no removed page is touched",
          "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-shrunk");
    const fs::path p = dir.path() / "log.bin";
    const long page = ::sysconf(_SC_PAGESIZE);
    writeFile(p, std::string(static_cast<std::size_t>(page) * 4, 'x'));
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    TouchJob before{r.value().bytes().data(), 0};
    REQUIRE(r.value().guarded(&touchDecode, &before).ok());
    fs::resize_file(p, static_cast<std::uintmax_t>(page)); // the first page is still part of the file
    TouchJob job{r.value().bytes().data(), 0};
    const esses::Result<void> g = r.value().guarded(&touchDecode, &job);
    REQUIRE_FALSE(g.ok());
    CHECK(g.error().code == esses::ErrorCode::Io);
    CHECK(g.error().message == "the mapped file was truncated while it was being read");
    // Growing the file back is not a truncation: the view's bytes are all inside the file again.
    fs::resize_file(p, static_cast<std::uintmax_t>(page) * 4);
    CHECK(r.value().guarded(&touchDecode, &job).ok());
}

// AC-4a's POSIX equivalent of the rename-over writer: rename(2) over the mapped file succeeds, and the
// mapping keeps the old file (its inode) and bytes.
TEST_CASE("mapped file: AC-4a a rename-over writer succeeds while the old bytes stay mapped",
          "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-rename");
    const fs::path target = dir.path() / "session.ld";
    const fs::path replacement = dir.path() / "session.ld.tmp";
    const std::string before(4096 + 17, 'o');
    const std::string after(5000, 'n');
    writeOldLog(target, before);
    esses::Result<MappedFile> r = MappedFile::open(target);
    REQUIRE(r.ok());
    const MappedFile& mf = r.value();
    REQUIRE_FALSE(mf.changedOnDisk());
    writeFile(replacement, after);
    REQUIRE(std::rename(replacement.c_str(), target.c_str()) == 0);
    CHECK(viewOf(mf) == before);
    SumJob sum{mf.bytes(), 0};
    CHECK(mf.guarded(&sumDecode, &sum).ok());
    CHECK(sum.sum == sumOf(before));
    CHECK(readAll(target) == after);
    CHECK(mf.changedOnDisk());
    esses::Result<MappedFile> again = MappedFile::open(target);
    REQUIRE(again.ok());
    CHECK(viewOf(again.value()) == after);
}

// POSIX has no mandatory share modes, so an in-place writer is not refused: the change is caught instead, by
// changedOnDisk() (the size or mtime differs) before a decode and by guarded() when the file shrank under it.
TEST_CASE("mapped file: AC-4a an in-place writer is seen by changedOnDisk and guarded",
          "[unit][mappedfile]") {
    esses::test::TempDir dir("mapped-inplace");
    const fs::path p = dir.path() / "inplace.ld";
    const long page = ::sysconf(_SC_PAGESIZE);
    const std::string content(static_cast<std::size_t>(page) * 2, 'x');
    writeOldLog(p, content);
    esses::Result<MappedFile> r = MappedFile::open(p);
    REQUIRE(r.ok());
    REQUIRE_FALSE(r.value().changedOnDisk());
    writeFile(p, std::string(content.size(), 'y'));                     // the same size, rewritten in place
    CHECK(r.value().changedOnDisk());                                   // its mtime is newer
    writeFile(p, std::string(static_cast<std::size_t>(page) / 2, 'z')); // now shorter than the view
    TouchJob job{r.value().bytes().data(), 0};
    const esses::Result<void> g = r.value().guarded(&touchDecode, &job);
    REQUIRE_FALSE(g.ok());
    CHECK(g.error().code == esses::ErrorCode::Io);
}
#endif
