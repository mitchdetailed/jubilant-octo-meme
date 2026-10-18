// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The data-less Scope (03 section 11 item 12): with nothing open every lookup misses and references are
// I-UNRESOLVED. A derived scope is data-backed unless it says otherwise.

#include <esses/maths/scope.hpp>

#include <typeinfo>

namespace esses::maths {

std::optional<ScopeEntry> Scope::lookup(std::string_view /*nameCf*/, const ChannelKey& /*requester*/) const {
    return std::nullopt;
}

bool Scope::dataBacked() const noexcept {
    return typeid(*this) != typeid(Scope);
}

} // namespace esses::maths
