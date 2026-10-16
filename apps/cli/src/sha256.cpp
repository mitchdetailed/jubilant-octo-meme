// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "sha256.hpp"

#include "cli_common.hpp"

#include <esses/base/read_file.hpp>

#include <fstream>
#include <vector>

namespace esses::cli {
namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept {
    return (x >> n) | (x << (32 - n));
}

} // namespace

Sha256::Sha256() noexcept
    : h_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::block(const std::uint8_t* p) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) |
               (std::uint32_t{p[4 * i + 2]} << 8) | std::uint32_t{p[4 * i + 3]};
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(std::span<const std::byte> bytes) noexcept {
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    std::size_t n = bytes.size();
    bits_ += static_cast<std::uint64_t>(n) * 8u;
    if (used_ > 0) {
        while (n > 0 && used_ < 64) {
            buf_[used_++] = *p++;
            --n;
        }
        if (used_ == 64) {
            block(buf_.data());
            used_ = 0;
        }
    }
    while (n >= 64) {
        block(p);
        p += 64;
        n -= 64;
    }
    while (n > 0) {
        buf_[used_++] = *p++;
        --n;
    }
}

void Sha256::update(std::string_view bytes) noexcept {
    update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
}

std::array<std::uint8_t, 32> Sha256::digest() noexcept {
    const std::uint64_t bits = bits_;
    buf_[used_++] = 0x80;
    if (used_ > 56) {
        while (used_ < 64) {
            buf_[used_++] = 0;
        }
        block(buf_.data());
        used_ = 0;
    }
    while (used_ < 56) {
        buf_[used_++] = 0;
    }
    for (int i = 7; i >= 0; --i) {
        buf_[used_++] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    block(buf_.data());
    used_ = 0;
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
    return out;
}

std::string Sha256::hexDigest() {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto d = digest();
    std::string out;
    out.reserve(64);
    for (const std::uint8_t b : d) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string sha256Hex(std::string_view bytes) {
    Sha256 s;
    s.update(bytes);
    return s.hexDigest();
}

Result<std::string> sha256File(const std::filesystem::path& path) {
    // A directory opens as a stream on POSIX and libc++ reads it as an empty file: check the type first.
    if (Result<void> regular = requireRegularFile(path, u8(path)); !regular) {
        return Failure{regular.error()};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot open " + u8(path), std::move(ctx));
    }
    Sha256 s;
    std::vector<char> buf(std::size_t{1} << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            s.update(std::string_view(buf.data(), static_cast<std::size_t>(got)));
        }
    }
    if (in.bad()) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot read " + u8(path), std::move(ctx));
    }
    return s.hexDigest();
}

} // namespace esses::cli
