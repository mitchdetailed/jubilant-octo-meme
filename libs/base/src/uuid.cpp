// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/uuid.hpp>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace esses {
namespace {

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::mt19937_64& threadRng() {
    thread_local std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(),
                          rd(),
                          rd(),
                          rd(),
                          static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()),
                          static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id()))};
        return std::mt19937_64(seq);
    }();
    return rng;
}

} // namespace

std::string Uuid::str() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kDigits[b[i] >> 4]);
        out.push_back(kDigits[b[i] & 0x0Fu]);
    }
    return out;
}

std::optional<Uuid> Uuid::tryParse(std::string_view text) noexcept {
    if (text.size() == 38 && text.front() == '{' && text.back() == '}') {
        text = text.substr(1, 36);
    }
    if (text.size() != 36) {
        return std::nullopt;
    }
    Uuid u;
    std::size_t byte = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return std::nullopt;
            }
            ++i;
            continue;
        }
        const int hiNibble = hexValue(text[i]);
        const int loNibble = hexValue(text[i + 1]);
        if (hiNibble < 0 || loNibble < 0) {
            return std::nullopt;
        }
        u.b[byte++] = static_cast<std::uint8_t>((hiNibble << 4) | loNibble);
        i += 2;
    }
    return u;
}

Uuid Uuid::parse(std::string_view text) noexcept {
    return tryParse(text).value_or(Uuid{});
}

bool Uuid::isNil() const noexcept {
    return std::all_of(b.begin(), b.end(), [](std::uint8_t v) { return v == 0; });
}

Uuid uuidFromHash(Hash128 h, std::uint8_t version) noexcept {
    Uuid u;
    for (std::size_t i = 0; i < 8; ++i) {
        u.b[i] = static_cast<std::uint8_t>((h.hi >> (8 * (7 - i))) & 0xFFu);
        u.b[8 + i] = static_cast<std::uint8_t>((h.lo >> (8 * (7 - i))) & 0xFFu);
    }
    u.b[6] = static_cast<std::uint8_t>((u.b[6] & 0x0Fu) | ((version & 0x0Fu) << 4));
    u.b[8] = static_cast<std::uint8_t>((u.b[8] & 0x3Fu) | 0x80u);
    return u;
}

Uuid uuidRandom() {
    std::mt19937_64& rng = threadRng();
    const std::uint64_t a = rng();
    const std::uint64_t c = rng();
    return uuidFromHash(Hash128{c, a}, 4);
}

} // namespace esses
