// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "soak_metrics.hpp"

#include <esses/base/read_file.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#else
#include <malloc.h>

#include <unistd.h>
#endif

// ASan's allocator statistics (GCC: __SANITIZE_ADDRESS__; Clang: __has_feature). Not with MSVC's ASan.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ESSES_SOAK_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(_MSC_VER)
#define ESSES_SOAK_ASAN 1
#endif
#if defined(ESSES_SOAK_ASAN) && !defined(_MSC_VER)
#include <sanitizer/allocator_interface.h>
#define ESSES_SOAK_HAVE_SANITIZER_ALLOCATOR 1
#endif

namespace esses::soak {

ProcessMemory sampleProcessMemory() {
    ProcessMemory m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                sizeof(pmc)) != 0) {
        m.residentBytes = pmc.WorkingSetSize;
        m.heapBytes =
            pmc.PrivateUsage; // the commit charge, unless the debug CRT below counts the heap itself
        m.heapSource = "private-bytes";
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        m.residentBytes = info.resident_size;
    }
#else
    // /proc/self/statm: size resident shared text lib data dt, in pages.
    if (const Result<std::string> statm = readFileHead(std::filesystem::path("/proc/self/statm"), 256);
        statm.ok()) {
        unsigned long long size = 0;
        unsigned long long resident = 0;
        if (std::sscanf(statm.value().c_str(), "%llu %llu", &size, &resident) == 2) {
            const long page = sysconf(_SC_PAGESIZE);
            m.residentBytes = resident * static_cast<unsigned long long>(page > 0 ? page : 4096);
        }
    }
#endif

#if defined(ESSES_SOAK_HAVE_SANITIZER_ALLOCATOR)
    m.heapBytes = __sanitizer_get_current_allocated_bytes();
    m.heapSource = "asan";
#elif defined(_MSC_VER) && defined(_DEBUG)
    _CrtMemState state{};
    _CrtMemCheckpoint(&state);
    m.heapBytes = state.lSizes[_NORMAL_BLOCK] + state.lSizes[_CLIENT_BLOCK];
    m.heapSource = "crt-debug";
#elif defined(__APPLE__)
    malloc_statistics_t stats{};
    malloc_zone_statistics(nullptr, &stats);
    m.heapBytes = stats.size_in_use;
    m.heapSource = "malloc-zones";
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 info = mallinfo2();
    m.heapBytes = info.uordblks + info.hblkhd;
    m.heapSource = "mallinfo2";
#endif
    return m;
}

GrowthVerdict judgeGrowth(const std::vector<double>& series, std::size_t warmup, double absSlack,
                          double relSlack) {
    GrowthVerdict v;
    v.samples = series.size();
    if (series.size() < warmup + 3) {
        return v;
    }
    const auto first = series.begin() + static_cast<std::ptrdiff_t>(warmup);
    const std::size_t judged = series.size() - warmup;
    const auto third = static_cast<std::ptrdiff_t>(std::max<std::size_t>(1, judged / 3));
    v.judged = true;
    v.early = *std::max_element(first, first + third);
    v.late = *std::min_element(series.end() - third, series.end());
    v.allowance = std::max(absSlack, relSlack * std::abs(v.early));
    v.grew = v.late > v.early + v.allowance;
    return v;
}

std::string describe(const std::string& name, const GrowthVerdict& v) {
    std::ostringstream out;
    out << name << ": ";
    if (!v.judged) {
        out << "not judged (" << v.samples << " samples)";
        return out.str();
    }
    out.setf(std::ios::fixed);
    out.precision(0);
    out << "early max " << v.early << ", late min " << v.late << ", allowance " << v.allowance << " over "
        << v.samples << " samples -> " << (v.grew ? "GREW" : "ok");
    return out.str();
}

} // namespace esses::soak
