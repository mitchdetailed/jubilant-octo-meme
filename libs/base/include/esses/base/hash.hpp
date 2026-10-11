// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Hash128: xxh3-128 (vendored xxHash 0.8, seed 0). hash128(parts) is the only way to derive a deterministic
// id in the code base (02 section 1 behaviour 3): each part is prefixed with its byte length as a
// little-endian uint64, then the concatenation is hashed, so {"ab","c"} and {"a","bc"} differ and every
// platform agrees.
#pragma once

#include <esses/base/config.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace esses {

struct Hash128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    // 32 lower-case hex digits, hi first: xxHash's canonical (big-endian) representation.
    std::string hex() const;
    friend constexpr bool operator==(const Hash128&, const Hash128&) = default;
    friend constexpr std::strong_ordering operator<=>(const Hash128& a, const Hash128& b) noexcept {
        if (const auto c = a.hi <=> b.hi; c != 0) {
            return c;
        }
        return a.lo <=> b.lo;
    }
};

Hash128 hash128(std::span<const std::byte> bytes) noexcept;
Hash128 hash128(std::string_view bytes) noexcept;
Hash128 hash128(std::initializer_list<std::string_view> parts) noexcept; // length-prefixed concatenation

// Incremental xxh3-128 over several buffers; equal to hash128() of their concatenation.
class Hash128Stream {
public:
    Hash128Stream();
    ~Hash128Stream();
    Hash128Stream(Hash128Stream&&) noexcept;
    Hash128Stream& operator=(Hash128Stream&&) noexcept;
    Hash128Stream(const Hash128Stream&) = delete;
    Hash128Stream& operator=(const Hash128Stream&) = delete;

    void reset() noexcept;
    void update(std::span<const std::byte> bytes) noexcept;
    void update(std::string_view bytes) noexcept;
    void updateU64(std::uint64_t v) noexcept;        // 8 bytes, little-endian
    void updatePart(std::string_view part) noexcept; // length prefix + bytes, as hash128(parts)
    Hash128 digest() const noexcept;                 // does not reset

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace esses

template <> struct std::hash<esses::Hash128> {
    std::size_t operator()(const esses::Hash128& h) const noexcept {
        return static_cast<std::size_t>(h.lo ^ (h.hi * 0x9E3779B97F4A7C15ull));
    }
};
