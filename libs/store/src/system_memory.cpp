// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "system_memory.hpp"

#include <esses/base/config.hpp>

#include <cstddef>

#if defined(ESSES_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(ESSES_OS_MACOS)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

namespace esses::store::detail {

std::uint64_t physicalMemoryBytes() noexcept {
#if defined(ESSES_OS_WINDOWS)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        return static_cast<std::uint64_t>(status.ullTotalPhys);
    }
    return 0;
#elif defined(ESSES_OS_MACOS)
    std::uint64_t bytes = 0;
    std::size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0) {
        return bytes;
    }
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize);
    }
    return 0;
#endif
}

} // namespace esses::store::detail
