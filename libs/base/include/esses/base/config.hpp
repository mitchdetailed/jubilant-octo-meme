// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Common configuration for every esses_base header: platform detection and the namespace convention.
#pragma once

#if defined(_WIN32)
#define ESSES_OS_WINDOWS 1
#elif defined(__APPLE__)
#define ESSES_OS_MACOS 1
#define ESSES_OS_POSIX 1
#else
#define ESSES_OS_LINUX 1
#define ESSES_OS_POSIX 1
#endif

#if defined(__SIZEOF_INT128__)
#define ESSES_HAVE_INT128 1
#endif

// The esses_base primitives live directly in namespace `esses` (01 sections 5-6, outline section 1.1).
// 02_DATA_MODEL.md section 1 writes them as `esses::base::...`; this alias makes both spellings valid.
// Consequence: no other code may open `namespace esses::base { ... }`.
namespace esses {
namespace base = ::esses;
} // namespace esses
