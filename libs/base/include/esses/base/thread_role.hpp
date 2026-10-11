// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ThreadRole (01 section 5.3 rule 4, D-123 (e)): a thread_local flag. Only the interactive application sets
// NoBlock on its GUI thread; debug builds assert that Future::get/take (and later Column::require) are never
// called on a NoBlock thread. esses-render, the offscreen renderer and tests may block.
#pragma once

#include <esses/base/config.hpp>

#include <cstdint>

namespace esses {

class ThreadRole {
public:
    enum Kind : std::uint8_t { Normal = 0, NoBlock = 1 };

    static void set(Kind kind) noexcept;
    static Kind current() noexcept;
    static bool isNoBlock() noexcept { return current() == NoBlock; }

    // Sets a role for the lifetime of the scope and restores the previous one.
    class Scope {
    public:
        explicit Scope(Kind kind) noexcept : previous_(current()) { set(kind); }
        ~Scope() { set(previous_); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Kind previous_;
    };
};

} // namespace esses
