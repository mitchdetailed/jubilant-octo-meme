// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/read_file.hpp>
#include <esses/base/utf8.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>
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
#include <fstream>
#include <ios>
#endif

namespace esses {

namespace {

constexpr std::size_t kReadBlock = std::size_t{1} << 16;

std::string nameOf(const std::filesystem::path& path, std::string_view what) {
    if (!what.empty()) {
        return std::string(what);
    }
    return "'" + pathToUtf8(path) + "'";
}

Failure failAt(const std::filesystem::path& path, ErrorCode code, std::string message) {
    ErrorContext ctx;
    ctx.path = path;
    return Failure{Error{code, std::move(message), std::move(ctx)}};
}

// The size is only a hint for the allocation: the read loops go to the end (or the limit), whatever the
// length is by then.
void reserveFor(std::string& text, std::uintmax_t size, std::size_t limit) {
    const std::uintmax_t want = std::min<std::uintmax_t>(size, limit);
    if (want < text.max_size()) {
        text.reserve(static_cast<std::size_t>(want));
    }
}

// requireRegularFile(); `missing` is set when it failed because nothing is at `path`.
Result<void> checkRegularFile(const std::filesystem::path& path, std::string_view what, bool& missing) {
    missing = false;
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    // libstdc++ and libc++ also set `ec` for a path that does not exist; the type tells that case apart.
    if (status.type() == std::filesystem::file_type::not_found) {
        missing = true;
        return failAt(path, ErrorCode::Io, "cannot open " + nameOf(path, what));
    }
    if (ec) {
        return failAt(path, ErrorCode::Io, "cannot stat " + nameOf(path, what) + ": " + ec.message());
    }
    if (!std::filesystem::is_regular_file(status)) {
        return failAt(path, ErrorCode::Io, nameOf(path, what) + " is not a regular file");
    }
    return ok();
}

#if defined(ESSES_OS_WINDOWS)
// How long a file that is not found, or is being deleted, is looked for again (read_file.hpp).
constexpr auto kMissingRetryFor = std::chrono::milliseconds(500);

class HandleCloser {
public:
    explicit HandleCloser(HANDLE handle) noexcept : handle_(handle) {}
    ~HandleCloser() { CloseHandle(handle_); }
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;

private:
    HANDLE handle_;
};

// The checks and the open of readRegularFile() on Windows. Every share flag (read_file.hpp): neither a writer
// nor the handle that renamed the file into place refuses the open, and an AtomicFile rename over the file
// proceeds while it is open. A file that is not found, or is being deleted (ERROR_ACCESS_DENIED), is looked
// for again until kMissingRetryFor has passed; a sharing violation is reported at once.
Result<HANDLE> openRegularFile(const std::filesystem::path& path, std::string_view what) {
    const auto deadline = std::chrono::steady_clock::now() + kMissingRetryFor;
    for (;;) {
        bool missing = false;
        if (Result<void> regular = checkRegularFile(path, what, missing); !regular) {
            if (!missing || std::chrono::steady_clock::now() >= deadline) {
                return Failure{regular.error()};
            }
        } else {
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                return file;
            }
            const DWORD code = GetLastError();
            const bool gone =
                code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_ACCESS_DENIED;
            if (!gone || std::chrono::steady_clock::now() >= deadline) {
                return failAt(path, ErrorCode::Io, "cannot open " + nameOf(path, what));
            }
        }
        Sleep(1);
    }
}
#endif

// At most `limit` bytes of the file at `path`, after the requireRegularFile() checks.
Result<std::string> readRegularFile(const std::filesystem::path& path, std::string_view what,
                                    std::size_t limit) {
    try {
#if defined(ESSES_OS_WINDOWS)
        const Result<HANDLE> opened = openRegularFile(path, what);
        if (!opened) {
            return Failure{opened.error()};
        }
        const HANDLE file = *opened;
        const HandleCloser closer(file);
        std::string text;
        if (LARGE_INTEGER size{}; GetFileSizeEx(file, &size) && size.QuadPart >= 0) {
            reserveFor(text, static_cast<std::uintmax_t>(size.QuadPart), limit);
        }
        std::vector<char> buffer(std::min(kReadBlock, limit));
        while (text.size() < limit) {
            const auto want = static_cast<DWORD>(std::min(buffer.size(), limit - text.size()));
            DWORD got = 0;
            if (!ReadFile(file, buffer.data(), want, &got, nullptr)) {
                return failAt(path, ErrorCode::Io, "cannot read " + nameOf(path, what));
            }
            if (got == 0) {
                break; // the end of the file
            }
            text.append(buffer.data(), got);
        }
        return text;
#else
        bool missing = false;
        if (Result<void> regular = checkRegularFile(path, what, missing); !regular) {
            return Failure{regular.error()};
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return failAt(path, ErrorCode::Io, "cannot open " + nameOf(path, what));
        }
        std::string text;
        std::error_code ec;
        if (const std::uintmax_t size = std::filesystem::file_size(path, ec); !ec) {
            reserveFor(text, size, limit);
        }
        // istream::read, unlike istreambuf_iterator, catches what the file buffer throws and sets badbit (the
        // exception mask is left empty), so a failed read is reported, never thrown.
        std::vector<char> buffer(std::min(kReadBlock, limit));
        while (in && text.size() < limit) {
            const std::size_t want = std::min(buffer.size(), limit - text.size());
            in.read(buffer.data(), static_cast<std::streamsize>(want));
            if (const std::streamsize got = in.gcount(); got > 0) {
                text.append(buffer.data(), static_cast<std::size_t>(got));
            }
        }
        if (in.bad()) {
            return failAt(path, ErrorCode::Io, "cannot read " + nameOf(path, what));
        }
        return text;
#endif
    } catch (const std::bad_alloc&) {
        return failAt(path, ErrorCode::OutOfMemory, "out of memory reading " + nameOf(path, what));
    } catch (const std::exception&) {
        // std::ios_base::failure from a standard library that lets one through read()
        return failAt(path, ErrorCode::Io, "cannot read " + nameOf(path, what));
    }
}

} // namespace

Result<void> requireRegularFile(const std::filesystem::path& path, std::string_view what) {
    bool missing = false;
    return checkRegularFile(path, what, missing);
}

Result<std::string> readWholeFile(const std::filesystem::path& path, std::string_view what) {
    return readRegularFile(path, what, std::numeric_limits<std::size_t>::max());
}

Result<std::string> readFileHead(const std::filesystem::path& path, std::size_t maxBytes,
                                 std::string_view what) {
    return readRegularFile(path, what, maxBytes);
}

} // namespace esses
