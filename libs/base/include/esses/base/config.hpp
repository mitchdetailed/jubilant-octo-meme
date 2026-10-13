// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Common configuration for every esses_base header: platform detection and the namespace convention.
#pragma once

// Feature macros without a value: test them with defined() only (an integral value is what clang-tidy's
// modernize-macro-to-enum flags, on Linux and macOS).
#if defined(_WIN32)
#define ESSES_OS_WINDOWS
#elif defined(__APPLE__)
#define ESSES_OS_MACOS
#define ESSES_OS_POSIX
#else
#define ESSES_OS_LINUX
#define ESSES_OS_POSIX
#endif

#if defined(__SIZEOF_INT128__)
#define ESSES_HAVE_INT128
#endif

// The esses_base primitives live directly in namespace `esses` (01 sections 5-6, outline section 1.1).
// 02_DATA_MODEL.md section 1 writes them as `esses::base::...`; this alias makes both spellings valid.
// Consequence: no other code may open `namespace esses::base { ... }`.
namespace esses {
namespace base = ::esses;
} // namespace esses
