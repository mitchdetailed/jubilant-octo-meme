// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/assert.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace esses {
namespace {

void defaultAssertHandler(const char* expression, const char* file, int line, const char* message) {
    std::fprintf(stderr, "ESSES_ASSERT failed: %s (%s:%d)%s%s\n", expression, file, line,
                 message != nullptr ? ": " : "", message != nullptr ? message : "");
    std::fflush(stderr);
    std::abort();
}

std::atomic<AssertHandler> gAssertHandler{&defaultAssertHandler};

} // namespace

AssertHandler setAssertHandler(AssertHandler handler) noexcept {
    return gAssertHandler.exchange(handler != nullptr ? handler : &defaultAssertHandler);
}

namespace detail {

void assertFailed(const char* expression, const char* file, int line, const char* message) noexcept {
    gAssertHandler.load()(expression, file, line, message);
}

void fatal(const char* what, const char* file, int line) noexcept {
    std::fprintf(stderr, "ESSES_FATAL: %s (%s:%d)\n", what, file, line);
    std::fflush(stderr);
    std::abort();
}

} // namespace detail
} // namespace esses
