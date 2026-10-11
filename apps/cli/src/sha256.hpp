// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SHA-256 (FIPS 180-4) for fixture-1's input and companion hashes (05 section 9.1). Fixture identity only;
// not a security primitive. Written from the standard.
#pragma once

#include <esses/base/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace esses::cli {

class Sha256 {
public:
    Sha256() noexcept;
    void update(std::span<const std::byte> bytes) noexcept;
    void update(std::string_view bytes) noexcept;
    std::array<std::uint8_t, 32> digest() noexcept; // finalises; call once
    std::string hexDigest();                        // 64 lower-case hex digits; finalises

private:
    void block(const std::uint8_t* p) noexcept;
    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t used_ = 0;
    std::uint64_t bits_ = 0;
};

std::string sha256Hex(std::string_view bytes);
// Streams the file in 1 MiB blocks. Io when it cannot be read.
Result<std::string> sha256File(const std::filesystem::path& path);

} // namespace esses::cli
