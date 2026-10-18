// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Scope (03 section 3.3, D-013): what a 'Name' in an expression resolves to. MathGraph builds one per
// (DataSource, layer) and requester, following ChannelResolver's order (mappings -> aliases -> the shadowing
// chain head -> logged channel -> numeric/text detail -> constant: own module, local, other enabled global
// modules in project order with W-CONST-AMBIGUOUS). Every lookup records a ResolutionTrace for Channel
// Status.
//
// The base class is the data-less scope of the Maths Editor with nothing open (03 section 11 item 12): every
// lookup misses, so references are I-UNRESOLVED. MathGraph derives the data-backed scope; a derived scope is
// data-backed (dataBacked(), so a miss is E-UNKNOWN-CHANNEL) unless it overrides dataBacked().
//
// What an entry carries (M2 contract additions to 03 section 3.3, libs/maths/README.md):
//   type        Channel: Kind::Series with the channel's base unit, dimension, rate (Rate{} for a
//               timestamped series without a nominal rate) and `integer` for I32/enum channels. Constant and
//               DetailNumber: Kind::Scalar in the unit of `number`. DetailText: Kind::Text.
//   interp      the channel's interpolation (a Channel Editor override wins over the descriptor); compile()
//               resamples the input with it, except the first argument of smooth (always Linear, 03 6.4)
//   dtype       the channel's dtype (an F64 input makes the plan F64, 03 section 6.5)
//   number      Constant, DetailNumber and Setup: the value in type.unit (compile() folds it); give it in the
//               base unit of its quantity so that a reference without [unit] reads base units like a channel
//   text        DetailText: the value (compile() folds text comparisons)
//   diags       what the lookup itself reports (W-CONST-AMBIGUOUS, W-CONST-SHADOWED, W-ALIAS-FALLBACK); the
//               type checker adds them with the span of the reference
#pragma once

#include <esses/maths/channel_key.hpp>
#include <esses/maths/diagnostics.hpp>
#include <esses/maths/types.hpp>
#include <esses/model/resolver.hpp>
#include <esses/store/types.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace esses::maths {

class FunctionRegistry;

struct ScopeEntry {
    enum What : std::uint8_t { Channel, Constant, DetailNumber, DetailText, Setup };
    What what = Channel;
    Type type;
    ChannelKey key; // Channel: the chain head the reference reads
    std::string resolvedName;
    model::ResolutionTrace trace;
    // M2 contract additions (see the file comment).
    store::Interp interp = store::Interp::Linear;
    store::Dtype dtype = store::Dtype::F32;
    std::optional<double> number;
    std::string text;
    Diagnostics diags;
};

class Scope {
public:
    Scope(const FunctionRegistry& functions, const units::UnitRegistry& units) noexcept
        : functions_(&functions), units_(&units) {}
    Scope(const Scope&) = default;
    Scope& operator=(const Scope&) = default;
    Scope(Scope&&) = default;
    Scope& operator=(Scope&&) = default;
    virtual ~Scope() = default;

    // `nameCf` is case-folded; `requester` is the node asking (self-maths reads the chain element before it).
    // The base (data-less) scope resolves nothing.
    virtual std::optional<ScopeEntry> lookup(std::string_view nameCf, const ChannelKey& requester) const;

    // True when a miss means the name does not exist (E-UNKNOWN-CHANNEL) rather than "no data loaded"
    // (I-UNRESOLVED). The base class answers false for itself and true for every derived class.
    virtual bool dataBacked() const noexcept;

    const FunctionRegistry& functions() const noexcept { return *functions_; }
    const units::UnitRegistry& units() const noexcept { return *units_; }

private:
    const FunctionRegistry* functions_;
    const units::UnitRegistry* units_;
};

} // namespace esses::maths
