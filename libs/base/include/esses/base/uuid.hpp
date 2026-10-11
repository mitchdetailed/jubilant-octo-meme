// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Uuid (02 section 1): random v4 for user-created objects, deterministic v5-style ids from Hash128 for logged
// objects (D-013, D-014).
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/hash.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace esses {

struct Uuid {
    std::array<std::uint8_t, 16> b{};

    // Canonical lower-case 8-4-4-4-12 form.
    std::string str() const;
    // Accepts the canonical form in either case, optionally wrapped in braces. Malformed input yields the nil
    // Uuid; use tryParse() to tell a malformed string from a nil one.
    static Uuid parse(std::string_view text) noexcept;
    static std::optional<Uuid> tryParse(std::string_view text) noexcept;

    bool isNil() const noexcept;
    std::uint8_t version() const noexcept { return static_cast<std::uint8_t>(b[6] >> 4); }

    friend auto operator<=>(const Uuid&, const Uuid&) = default; // byte-wise
    friend bool operator==(const Uuid&, const Uuid&) = default;
};

// Deterministic id: the 16 bytes of h in canonical order (hi big-endian, then lo big-endian) with the version
// nibble set to `version` and the RFC 4122 variant bits (10xx) set.
Uuid uuidFromHash(Hash128 h, std::uint8_t version = 5) noexcept;
// Random v4 (user-created objects only). Uses a per-thread generator seeded from std::random_device.
Uuid uuidRandom();

} // namespace esses

template <> struct std::hash<esses::Uuid> {
    std::size_t operator()(const esses::Uuid& u) const noexcept {
        std::uint64_t v = 0xcbf29ce484222325ull; // FNV-1a over the 16 bytes
        for (const std::uint8_t byte : u.b) {
            v = (v ^ byte) * 0x100000001b3ull;
        }
        return static_cast<std::size_t>(v);
    }
};
