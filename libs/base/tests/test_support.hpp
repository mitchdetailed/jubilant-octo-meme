// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers for the esses_base tests.
#pragma once

#include <esses/base/assert.hpp>
#include <esses/base/env.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace esses::test {

// Property-test seed (08 section 6.2): ESSES_TEST_SEED when set, else a fixed default.
inline std::uint64_t testSeed() {
    if (const auto v = getEnv(envName("TEST_SEED")); v && !v->empty()) {
        return std::stoull(*v, nullptr, 0);
    }
    return 0x5EED'0000'2026'1010ull;
}

// Records assertion failures instead of aborting, for the duration of the scope.
class RecordingAssertHandler {
public:
    RecordingAssertHandler() : previous_(setAssertHandler(&record)) { count() = 0; }
    ~RecordingAssertHandler() { setAssertHandler(previous_); }
    RecordingAssertHandler(const RecordingAssertHandler&) = delete;
    RecordingAssertHandler& operator=(const RecordingAssertHandler&) = delete;
    static int failures() { return count().load(); }

private:
    static std::atomic<int>& count() {
        static std::atomic<int> c{0};
        return c;
    }
    static void record(const char*, const char*, int, const char*) { ++count(); }
    AssertHandler previous_;
};

// Polls `condition` every millisecond for up to `timeout`; returns its final value.
inline bool waitFor(const std::function<bool()>& condition,
                    std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A fresh, empty directory under the system temp directory, removed on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("esses-base-tests-" + tag + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace esses::test
