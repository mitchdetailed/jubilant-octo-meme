// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers of the M4 reload package's esses_formats tests (tests/m4/reload_*.cpp; also used by
// tests/ld_chunk_provider_tests.cpp): writing a file, moving a file's modification time through its
// attributes only (which works while the file is mapped, on every toolchain), and replacing a file the way an
// atomic writer does (Windows: a POSIX-semantics rename, which a mapping does not refuse; POSIX: rename(2)).
#pragma once

#include <esses/base/config.hpp>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
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
#endif

namespace esses::formats::reload_test {

inline void writeBytes(const std::filesystem::path& p, std::string_view content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Moves the file's modification time by `delta`: on Windows through a handle with FILE_WRITE_ATTRIBUTES only,
// which no share mode refuses (std::filesystem::last_write_time asks for write access with some standard
// libraries, which a mapped file refuses); elsewhere through std::filesystem.
inline bool shiftMtime(const std::filesystem::path& p, std::chrono::seconds delta) {
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
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return false;
    }
    std::filesystem::last_write_time(p, t + delta, ec);
    return !ec;
#endif
}

// Replaces `target` by `replacement` atomically (the writer 02 section 1 behaviour 5 lets through while the
// target is mapped).
inline bool renameOver(const std::filesystem::path& replacement, const std::filesystem::path& target) {
#if defined(ESSES_OS_WINDOWS)
    const HANDLE h = CreateFileW(replacement.c_str(), DELETE | SYNCHRONIZE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::wstring name = std::filesystem::absolute(target).native();
    const std::size_t nameBytes = name.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(info->FileName, name.c_str(), nameBytes + sizeof(wchar_t));
    // FileRenameInfoEx (Windows 10 1709+) by its value: MinGW's headers declare it only for a newer
    // _WIN32_WINNT than they default to.
    const auto renameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    bool done = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!done) { // a scanner may hold the new file for a moment
        done = SetFileInformationByHandle(h, renameInfoEx, info, static_cast<DWORD>(buffer.size())) != 0;
        if (done || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        Sleep(1);
    }
    CloseHandle(h);
    return done;
#else
    return std::rename(replacement.c_str(), target.c_str()) == 0;
#endif
}

} // namespace esses::formats::reload_test
