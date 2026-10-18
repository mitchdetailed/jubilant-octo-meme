// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// A test seam of Journal::clear (internal; used by libs/project/tests only): a hook that runs once clear()
// has listed the journal folder and before it deletes anything, so a test can write another session's file in
// between and check that the clear leaves it (D-190).
#pragma once

#include <functional>

namespace esses::project::detail {

// Sets the hook (empty: none) and returns the previous one. Process-wide and unsynchronised: a test sets it
// while no other thread clears a journal, and restores the previous hook afterwards.
std::function<void()> setJournalClearHook(std::function<void()> hook);

} // namespace esses::project::detail
