// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Priority bands and task keys of the TaskPool (D-031, D-052, D-123, 01 sections 5.1 and 5.4).
// esses::store uses this Priority through `using Priority = esses::Priority;`; there is no second enum.
#pragma once

#include <esses/base/config.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace esses {

enum class Priority : std::uint8_t { Interactive = 0, Visible = 1, Background = 2 };
inline constexpr std::size_t kPriorityBands = 3;

std::string_view priorityName(Priority p) noexcept;

// Hierarchical, '/'-separated key (01 section 5.4). The prefix decides how equal keys coalesce:
//   share keys    chunk/ summary/ slab/ ranges/ distidx/ maths/ open/ (and every other non-empty key):
//                 a submit equal to a queued or running task's key returns that task's Future; the task is
//                 cancelled only when every registered requester has cancelled; chunk/ and summary/ ignore
//                 requester cancellation entirely and stop only through cancelAll(<prefix>).
//   replace keys  trace/ geom/ report/: at most one running and one pending task per key; a new submit
//                 replaces the pending task (its Future resolves to Cancelled), never the running one.
//   empty key     no coalescing at all.
// Unknown prefixes coalesce like share keys, which is D-031's generic "request coalescing by key".
struct TaskKey {
    enum class Kind : std::uint8_t { None, Share, ShareOwnedBySource, Replace };

    std::string text;

    bool empty() const noexcept { return text.empty(); }
    Kind kind() const noexcept;
    friend bool operator==(const TaskKey&, const TaskKey&) = default;
};

} // namespace esses
