// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/thread_role.hpp>

namespace esses {

namespace {
thread_local ThreadRole::Kind tlsRole = ThreadRole::Normal;
} // namespace

void ThreadRole::set(Kind kind) noexcept {
    tlsRole = kind;
}

ThreadRole::Kind ThreadRole::current() noexcept {
    return tlsRole;
}

} // namespace esses
