// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/hash.hpp>

#include <array>

// The only translation unit that compiles xxHash; everything is inlined and static here.
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace esses {
namespace {

Hash128 fromXxh(const XXH128_hash_t& h) noexcept {
    return Hash128{h.low64, h.high64};
}

std::array<std::byte, 8> le64(std::uint64_t v) noexcept {
    std::array<std::byte, 8> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }
    return out;
}

} // namespace

std::string Hash128::hex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(32, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        const std::uint64_t word = i < 8 ? hi : lo;
        const std::size_t shift = 8 * (7 - (i % 8));
        const auto byte = static_cast<unsigned>((word >> shift) & 0xFFu);
        out[2 * i] = kDigits[byte >> 4];
        out[2 * i + 1] = kDigits[byte & 0x0Fu];
    }
    return out;
}

Hash128 hash128(std::span<const std::byte> bytes) noexcept {
    return fromXxh(XXH3_128bits(bytes.data(), bytes.size()));
}

Hash128 hash128(std::string_view bytes) noexcept {
    return fromXxh(XXH3_128bits(bytes.data(), bytes.size()));
}

Hash128 hash128(std::initializer_list<std::string_view> parts) noexcept {
    Hash128Stream s;
    for (const std::string_view part : parts) {
        s.updatePart(part);
    }
    return s.digest();
}

struct Hash128Stream::State {
    XXH3_state_t xxh;
};

Hash128Stream::Hash128Stream() : state_(std::make_unique<State>()) {
    reset();
}
Hash128Stream::~Hash128Stream() = default;
Hash128Stream::Hash128Stream(Hash128Stream&&) noexcept = default;
Hash128Stream& Hash128Stream::operator=(Hash128Stream&&) noexcept = default;

void Hash128Stream::reset() noexcept {
    if (state_) {
        XXH3_128bits_reset(&state_->xxh);
    }
}

void Hash128Stream::update(std::span<const std::byte> bytes) noexcept {
    if (state_ && !bytes.empty()) {
        XXH3_128bits_update(&state_->xxh, bytes.data(), bytes.size());
    }
}

void Hash128Stream::update(std::string_view bytes) noexcept {
    if (state_ && !bytes.empty()) {
        XXH3_128bits_update(&state_->xxh, bytes.data(), bytes.size());
    }
}

void Hash128Stream::updateU64(std::uint64_t v) noexcept {
    const auto b = le64(v);
    update(std::span<const std::byte>(b));
}

void Hash128Stream::updatePart(std::string_view part) noexcept {
    updateU64(static_cast<std::uint64_t>(part.size()));
    update(part);
}

Hash128 Hash128Stream::digest() const noexcept {
    if (!state_) {
        return Hash128{};
    }
    return fromXxh(XXH3_128bits_digest(&state_->xxh));
}

} // namespace esses
