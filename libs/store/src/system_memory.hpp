// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Physical RAM for the default ChunkCache budget (D-011: max(1 GiB, 25 % of RAM)). Private to esses_store;
// a candidate for esses_base when a second user appears.
#pragma once

#include <cstdint>

namespace esses::store::detail {

// Total physical memory in bytes, or 0 when the platform query fails.
std::uint64_t physicalMemoryBytes() noexcept;

} // namespace esses::store::detail
