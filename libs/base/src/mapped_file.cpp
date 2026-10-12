// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/int128.hpp>
#include <esses/base/mapped_file.hpp>
#include <esses/base/utf8.hpp>

#include <algorithm>
#include <limits>
#include <string>

#if defined(ESSES_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if !defined(ESSES_OS_WINDOWS) && defined(__GNUC__)
// SIG_DFL, SIG_IGN and MAP_FAILED expand to C-style casts in the libc headers.
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

namespace esses {
namespace {

constexpr std::size_t kQuickHashWindow = std::size_t{1} << 20; // first and last MiB

ErrorContext contextFor(const std::filesystem::path& path) {
    ErrorContext ctx;
    ctx.path = path;
    return ctx;
}

struct QuickHashJob {
    std::span<const std::byte> bytes;
    Hash128Stream* stream = nullptr;
};

// Plain routine for guarded(): owns no objects with destructors.
void quickHashDecode(void* ctx) {
    auto* job = static_cast<QuickHashJob*>(ctx);
    const std::size_t window = std::min(kQuickHashWindow, job->bytes.size());
    job->stream->updateU64(static_cast<std::uint64_t>(job->bytes.size()));
    job->stream->update(job->bytes.first(window));
    job->stream->update(job->bytes.last(window));
}

#if defined(ESSES_OS_WINDOWS)

std::string win32Error(const char* what, DWORD code) {
    return std::string(what) + " (Win32 error " + std::to_string(code) + ")";
}

std::int64_t fileTimeToUnixUs(const FILETIME& ft) noexcept {
    const std::uint64_t ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr std::int64_t kEpochDelta = 116444736000000000LL; // 100 ns ticks from 1601-01-01 to 1970-01-01
    return floorDiv(static_cast<std::int64_t>(ticks) - kEpochDelta, 10);
}

int pageFaultFilter(DWORD code, const EXCEPTION_POINTERS* info, const std::byte* begin,
                    const std::byte* end) noexcept {
    if (code != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->NumberParameters >= 2) {
        const ULONG_PTR addr = rec->ExceptionInformation[1];
        if (addr < reinterpret_cast<ULONG_PTR>(begin) || addr >= reinterpret_cast<ULONG_PTR>(end)) {
            return EXCEPTION_CONTINUE_SEARCH; // not our view
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects with destructors may live in this frame (__try/__except, C2712).
int runGuarded(void (*decode)(void*), void* ctx, const std::byte* begin, const std::byte* end) noexcept {
    __try {
        decode(ctx);
    } __except (pageFaultFilter(GetExceptionCode(), GetExceptionInformation(), begin, end)) {
        return 1;
    }
    return 0;
}

#else // POSIX

struct GuardFrame {
    sigjmp_buf jump;
    const std::byte* begin;
    const std::byte* end;
    GuardFrame* previous;
};

thread_local GuardFrame* tlsGuard = nullptr;
struct sigaction gPreviousBusAction{};
std::once_flag gBusHandlerOnce;

void onSigBus(int sig, siginfo_t* info, void* uctx) {
    const auto* addr = static_cast<const std::byte*>(info->si_addr);
    for (GuardFrame* g = tlsGuard; g != nullptr; g = g->previous) {
        if (addr >= g->begin && addr < g->end) {
            tlsGuard = g->previous;
            siglongjmp(g->jump, 1);
        }
    }
    // Not a fault inside a guarded view: chain to the previous disposition.
    if ((gPreviousBusAction.sa_flags & SA_SIGINFO) != 0 && gPreviousBusAction.sa_sigaction != nullptr) {
        gPreviousBusAction.sa_sigaction(sig, info, uctx);
        return;
    }
    if ((gPreviousBusAction.sa_flags & SA_SIGINFO) == 0 && gPreviousBusAction.sa_handler != SIG_DFL &&
        gPreviousBusAction.sa_handler != SIG_IGN) {
        gPreviousBusAction.sa_handler(sig);
        return;
    }
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGBUS, &dfl, nullptr); // returning re-executes the faulting access under the default action
}

void installBusHandler() {
    struct sigaction sa{};
    sa.sa_sigaction = &onSigBus;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, &gPreviousBusAction);
}

int runGuarded(void (*decode)(void*), void* ctx, const std::byte* begin, const std::byte* end) noexcept {
    std::call_once(gBusHandlerOnce, installBusHandler);
    GuardFrame frame;
    frame.begin = begin;
    frame.end = end;
    frame.previous = tlsGuard;
    if (sigsetjmp(frame.jump, 1) != 0) {
        return 1; // the handler restored tlsGuard
    }
    tlsGuard = &frame;
    decode(ctx);
    tlsGuard = frame.previous;
    return 0;
}

std::int64_t timespecToUnixUs(const struct timespec& ts) noexcept {
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000 + static_cast<std::int64_t>(ts.tv_nsec) / 1'000;
}

#endif

} // namespace

Result<FileStat> statFile(const std::filesystem::path& path) {
#if defined(ESSES_OS_WINDOWS)
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return fail(ErrorCode::Io, win32Error("cannot stat file", GetLastError()), contextFor(path));
    }
    FileStat st;
    st.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    st.mtimeUnixUs = fileTimeToUnixUs(data.ftLastWriteTime);
    return st;
#else
    struct stat sb{};
    if (::stat(path.c_str(), &sb) != 0) {
        return fail(ErrorCode::Io, std::string("cannot stat file: ") + std::strerror(errno),
                    contextFor(path));
    }
    FileStat st;
    st.size = static_cast<std::uint64_t>(sb.st_size);
#if defined(__APPLE__)
    st.mtimeUnixUs = timespecToUnixUs(sb.st_mtimespec);
#else
    st.mtimeUnixUs = timespecToUnixUs(sb.st_mtim);
#endif
    return st;
#endif
}

Result<MappedFile> MappedFile::open(const std::filesystem::path& path) {
    MappedFile mf;
    mf.path_ = path;
#if defined(ESSES_OS_WINDOWS)
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return fail(ErrorCode::Io, win32Error("cannot open file", GetLastError()), contextFor(path));
    }
    mf.fileHandle_ = file;
    LARGE_INTEGER size{};
    FILETIME lastWrite{};
    if (!GetFileSizeEx(file, &size) || !GetFileTime(file, nullptr, nullptr, &lastWrite)) {
        return fail(ErrorCode::Io, win32Error("cannot query file", GetLastError()), contextFor(path));
    }
    const auto fileSize = static_cast<std::uint64_t>(size.QuadPart);
    if (fileSize > std::numeric_limits<std::size_t>::max()) {
        return fail(ErrorCode::Unsupported, "file too large to map", contextFor(path));
    }
    mf.stamp_.size = fileSize;
    mf.stamp_.mtimeUnixUs = fileTimeToUnixUs(lastWrite);
    if (fileSize > 0) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) {
            return fail(ErrorCode::Io, win32Error("cannot create file mapping", GetLastError()),
                        contextFor(path));
        }
        mf.mappingHandle_ = mapping;
        const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr) {
            return fail(ErrorCode::Io, win32Error("cannot map view of file", GetLastError()),
                        contextFor(path));
        }
        mf.data_ = static_cast<const std::byte*>(view);
        mf.size_ = static_cast<std::size_t>(fileSize);
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return fail(ErrorCode::Io, std::string("cannot open file: ") + std::strerror(errno),
                    contextFor(path));
    }
    mf.fd_ = fd; // owned by mf from here (closed by close()); guarded() fstat()s it
    struct stat sb{};
    if (::fstat(fd, &sb) != 0) {
        const int err = errno;
        return fail(ErrorCode::Io, std::string("cannot stat file: ") + std::strerror(err), contextFor(path));
    }
    const auto fileSize = static_cast<std::uint64_t>(sb.st_size);
    mf.stamp_.size = fileSize;
#if defined(__APPLE__)
    mf.stamp_.mtimeUnixUs = timespecToUnixUs(sb.st_mtimespec);
#else
    mf.stamp_.mtimeUnixUs = timespecToUnixUs(sb.st_mtim);
#endif
    if (fileSize > 0) {
        void* view = ::mmap(nullptr, static_cast<std::size_t>(fileSize), PROT_READ, MAP_PRIVATE, fd, 0);
        if (view == MAP_FAILED) {
            const int err = errno;
            return fail(ErrorCode::Io, std::string("cannot map file: ") + std::strerror(err),
                        contextFor(path));
        }
        mf.data_ = static_cast<const std::byte*>(view);
        mf.size_ = static_cast<std::size_t>(fileSize);
    }
#endif
    mf.open_ = true;
    Hash128Stream stream;
    QuickHashJob job{mf.bytes(), &stream};
    if (Result<void> r = mf.guarded(&quickHashDecode, &job); !r.ok()) {
        return Failure{r.error()};
    }
    mf.stamp_.quick = stream.digest();
    return mf;
}

MappedFile::~MappedFile() {
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept {
    moveFrom(other);
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        moveFrom(other);
    }
    return *this;
}

void MappedFile::moveFrom(MappedFile& other) noexcept {
    path_ = std::move(other.path_);
    data_ = other.data_;
    size_ = other.size_;
    stamp_ = other.stamp_;
    open_ = other.open_;
#if defined(ESSES_OS_WINDOWS)
    fileHandle_ = other.fileHandle_;
    mappingHandle_ = other.mappingHandle_;
    other.fileHandle_ = nullptr;
    other.mappingHandle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.data_ = nullptr;
    other.size_ = 0;
    other.open_ = false;
}

void MappedFile::close() noexcept {
#if defined(ESSES_OS_WINDOWS)
    if (data_ != nullptr) {
        UnmapViewOfFile(data_);
    }
    if (mappingHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mappingHandle_));
    }
    if (fileHandle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(fileHandle_));
    }
    mappingHandle_ = nullptr;
    fileHandle_ = nullptr;
#else
    if (data_ != nullptr) {
        ::munmap(const_cast<std::byte*>(data_), size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
#endif
    data_ = nullptr;
    size_ = 0;
    open_ = false;
}

bool MappedFile::changedOnDisk() const {
    const Result<FileStat> st = statFile(path_);
    if (!st.ok()) {
        return true;
    }
    return st->size != stamp_.size || st->mtimeUnixUs != stamp_.mtimeUnixUs;
}

Result<void> MappedFile::guarded(void (*decode)(void* ctx), void* ctx) const noexcept {
    if (decode == nullptr) {
        return fail(ErrorCode::Invalid, "guarded: no decode routine");
    }
    const bool faulted = runGuarded(decode, ctx, data_, data_ + size_) != 0;
#if defined(ESSES_OS_WINDOWS)
    const bool truncated = false; // an in-place writer cannot shrink the file: it gets a sharing error
#else
    // POSIX leaves reads of a shrunken mapping unspecified (macOS serves resident pages without SIGBUS), so a
    // file that is now shorter than the view fails the call even when no signal was raised.
    struct stat sb{};
    const bool truncated = !faulted && size_ > 0 && fd_ >= 0 && ::fstat(fd_, &sb) == 0 &&
                           static_cast<std::uint64_t>(sb.st_size) < static_cast<std::uint64_t>(size_);
#endif
    if (faulted || truncated) {
        try {
            return fail(ErrorCode::Io,
                        faulted ? "page fault while reading a mapped file"
                                : "the mapped file was truncated while it was being read",
                        contextFor(path_));
        } catch (...) {
            return Failure{Error{ErrorCode::OutOfMemory, "bad_alloc", {}}};
        }
    }
    return ok();
}

} // namespace esses
