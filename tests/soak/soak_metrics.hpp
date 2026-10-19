// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The soak's measurements and its growth rule (AC-M4-03 "unbounded growth"; D-213).
//   * sampleProcessMemory(): the resident set (Windows working set; Linux /proc/self/statm through
//     esses::readFileHead; macOS task_info) and the bytes the process's heap holds for it, from the most
//     exact source the build has: the sanitizer allocator (ASan), the MSVC debug CRT's normal and client
//     blocks, glibc's mallinfo2, macOS's malloc zones, else on Windows the private bytes (heapSource names
//     it; 0 and "" when none).
//   * judgeGrowth(): a series sampled once per cycle in the same state of the workflow grows when, after
//     `warmup` samples, the smallest of its last third exceeds the largest of its first third by more than
//     max(absSlack, relSlack x that largest value). A steady leak raises every late sample above every early
//     one; caches that fill and stop, noise and one-off spikes do not (the early maximum and the late minimum
//     absorb them). Fewer than warmup + 3 samples are not judged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esses::soak {

struct ProcessMemory {
    std::uint64_t residentBytes = 0; // 0 when unavailable
    std::uint64_t heapBytes = 0;     // 0 when unavailable
    const char* heapSource = ""; // "asan", "crt-debug", "mallinfo2", "malloc-zones", "private-bytes" or ""
};
ProcessMemory sampleProcessMemory();

struct GrowthVerdict {
    bool judged = false;
    bool grew = false;
    double early = 0.0;     // the largest of the first third (after the warm-up)
    double late = 0.0;      // the smallest of the last third
    double allowance = 0.0; // max(absSlack, relSlack x early)
    std::size_t samples = 0;
};
GrowthVerdict judgeGrowth(const std::vector<double>& series, std::size_t warmup, double absSlack,
                          double relSlack);
// "<name>: not judged (n samples)" or "<name>: early <e>, late <l>, allowance <a> -> ok|GREW".
std::string describe(const std::string& name, const GrowthVerdict& v);

} // namespace esses::soak
