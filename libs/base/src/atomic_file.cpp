// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/atomic_file.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/uuid.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
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
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace esses {
namespace fs = std::filesystem;

namespace {

ErrorContext contextFor(const fs::path& path) {
    ErrorContext ctx;
    ctx.path = path;
    return ctx;
}

fs::path tempPathFor(const fs::path& target) {
    fs::path tmp = target;
    tmp += pathFromUtf8(".tmp-" + uuidRandom().str().substr(0, 8));
    return tmp;
}

#if defined(ESSES_OS_WINDOWS)
// How long a refused rename is retried. While other handles on the target open and close, the rename can be
// refused for a moment with ERROR_ACCESS_DENIED even when every one of them shares delete access, and with
// ERROR_SHARING_VIOLATION while a reader that does not share it (a std::ifstream) has the target open:
// measured with twelve processes each replacing a file while a thread of its own read it in a loop, a few
// renames in a thousand were refused, and each went through within six retries.
constexpr auto kRenameRetryFor = std::chrono::milliseconds(500);

bool renameOverPosix(HANDLE file, const fs::path& target) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(target, ec);
    if (ec) {
        return false;
    }
    const std::wstring& name = absolute.native();
    const std::size_t nameBytes = name.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(info->FileName, name.c_str(), nameBytes + sizeof(wchar_t)); // includes the terminating NUL
    const auto deadline = std::chrono::steady_clock::now() + kRenameRetryFor;
    for (;;) {
        if (SetFileInformationByHandle(file, FileRenameInfoEx, info, static_cast<DWORD>(buffer.size())) !=
            0) {
            return true;
        }
        const DWORD code = GetLastError();
        if ((code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION) ||
            std::chrono::steady_clock::now() >= deadline) {
            SetLastError(code);
            return false;
        }
        Sleep(1);
    }
}
#endif

} // namespace

Result<void> AtomicFile::write(const fs::path& target, std::string_view text) {
    return write(target, std::as_bytes(std::span<const char>(text.data(), text.size())));
}

Result<void> AtomicFile::write(const fs::path& target, std::span<const std::byte> bytes) {
    if (target.empty() || !target.has_filename()) {
        return fail(ErrorCode::Invalid, "atomic write: target has no file name", contextFor(target));
    }
    const fs::path tmp = tempPathFor(target);
#if defined(ESSES_OS_WINDOWS)
    // DELETE access lets the handle rename itself over the target (POSIX semantics, below). Sharing read and
    // delete access lets the renaming handle below be opened beside this one.
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return fail(ErrorCode::Io,
                    "atomic write: cannot create temporary file (Win32 error " +
                        std::to_string(GetLastError()) + ")",
                    contextFor(tmp));
    }
    const auto abandon = [&](const char* what) -> Result<void> {
        const DWORD code = GetLastError();
        CloseHandle(h);
        DeleteFileW(tmp.c_str());
        return fail(ErrorCode::Io,
                    std::string("atomic write: ") + what + " (Win32 error " + std::to_string(code) + ")",
                    contextFor(target));
    };
    std::size_t done = 0;
    while (done < bytes.size()) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 30));
        DWORD written = 0;
        if (!WriteFile(h, bytes.data() + done, chunk, &written, nullptr) || written == 0) {
            return abandon("write failed");
        }
        done += written;
    }
    if (!FlushFileBuffers(h)) {
        return abandon("flush failed");
    }
    // The file is renamed into place through a second handle that holds delete access only and shares
    // everything, and the writing handle is closed first. The renaming handle stays open for a moment after
    // the rename; a reader that opens the target in that moment meets neither write access nor an exclusive
    // share mode, so readWholeFile() (which shares read, write and delete) and MappedFile (read and delete)
    // are not refused with a sharing violation. When the second handle cannot be opened, the writing handle
    // renames the file, and such a reader can be refused for that moment.
    if (const HANDLE renamer = CreateFileW(tmp.c_str(), DELETE | SYNCHRONIZE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        renamer != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = renamer;
    }
    // Rename over the target with POSIX semantics (Windows 10 1709+, NTFS): this succeeds even while a reader
    // holds the target open with FILE_SHARE_DELETE or has it mapped (MappedFile), which a classic
    // MoveFileExW replace refuses with ERROR_ACCESS_DENIED. The reader keeps the old bytes (02 section 1
    // behaviour 5). File systems without POSIX rename fall back to MoveFileExW.
    if (renameOverPosix(h, target)) {
        CloseHandle(h);
        return ok();
    }
    CloseHandle(h);
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(tmp.c_str());
        return fail(ErrorCode::Io,
                    "atomic write: cannot replace the target (Win32 error " + std::to_string(code) + ")",
                    contextFor(target));
    }
    return ok();
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        return fail(ErrorCode::Io,
                    std::string("atomic write: cannot create temporary file: ") + std::strerror(errno),
                    contextFor(tmp));
    }
    const auto abandon = [&](const char* what, int err) -> Result<void> {
        ::close(fd);
        ::unlink(tmp.c_str());
        return fail(ErrorCode::Io, std::string("atomic write: ") + what + ": " + std::strerror(err),
                    contextFor(target));
    };
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return abandon("write failed", errno);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        return abandon("fsync failed", errno);
    }
    if (::close(fd) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        return fail(ErrorCode::Io, std::string("atomic write: close failed: ") + std::strerror(err),
                    contextFor(target));
    }
    if (std::rename(tmp.c_str(), target.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        return fail(ErrorCode::Io,
                    std::string("atomic write: cannot replace the target: ") + std::strerror(err),
                    contextFor(target));
    }
    const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd >= 0) {
        static_cast<void>(::fsync(dfd)); // best effort: make the rename durable
        ::close(dfd);
    }
    return ok();
#endif
}

} // namespace esses
