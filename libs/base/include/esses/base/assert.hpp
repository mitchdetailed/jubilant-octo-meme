// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ESSES_ASSERT (01 section 6.2 rule 7): active in debug builds and when ESSES_FORCE_ASSERTS is defined
// (CI release-with-asserts). A failed assertion calls the installed handler; the default handler prints the
// assertion and aborts. Tests may install a handler that records and returns, so the release-mode fallback
// path after an assertion (for example `fail(Internal, ...)`) can be exercised in debug builds too.
#pragma once

#include <esses/base/config.hpp>

namespace esses {

using AssertHandler = void (*)(const char* expression, const char* file, int line, const char* message);

// Installs `handler` (nullptr restores the default) and returns the previous handler. Thread-safe.
AssertHandler setAssertHandler(AssertHandler handler) noexcept;

namespace detail {
void assertFailed(const char* expression, const char* file, int line, const char* message) noexcept;
[[noreturn]] void fatal(const char* what, const char* file, int line) noexcept;
} // namespace detail

} // namespace esses

#if !defined(NDEBUG) || defined(ESSES_FORCE_ASSERTS)
#define ESSES_ASSERTS_ENABLED 1
#define ESSES_ASSERT(cond)                                                                                   \
    ((cond) ? static_cast<void>(0) : ::esses::detail::assertFailed(#cond, __FILE__, __LINE__, nullptr))
#define ESSES_ASSERT_MSG(cond, msg)                                                                          \
    ((cond) ? static_cast<void>(0) : ::esses::detail::assertFailed(#cond, __FILE__, __LINE__, (msg)))
#else
#define ESSES_ASSERTS_ENABLED 0
#define ESSES_ASSERT(cond) static_cast<void>(sizeof(!(cond)))
#define ESSES_ASSERT_MSG(cond, msg) static_cast<void>(sizeof(!(cond)))
#endif

// Unconditional: programming errors that cannot continue (for example reading the value of a failed Result).
#define ESSES_FATAL(what) ::esses::detail::fatal((what), __FILE__, __LINE__)
