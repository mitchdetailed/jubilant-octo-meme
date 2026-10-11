// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers for the .ld/.ldx core tests: temporary directories, writer -> reader shortcuts, sample
// builders and float comparisons.
#pragma once

#include "ld/ld_file.hpp"
#include "ld/ld_writer.hpp"

#include <esses/base/env.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace esses::formats::ld::test {

// Property-test seed (08 section 6.2): ESSES_TEST_SEED when set, else a fixed default.
inline std::uint64_t testSeed() {
    if (const auto v = getEnv(envName("TEST_SEED")); v && !v->empty()) {
        return std::stoull(*v, nullptr, 0);
    }
    return 0x5EED'0000'2026'1010ull;
}

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("esses-formats-tests-" + tag + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline std::shared_ptr<const std::vector<std::byte>> share(std::vector<std::byte> bytes) {
    return std::make_shared<const std::vector<std::byte>>(std::move(bytes));
}

// Writer -> reader in memory; REQUIREs both steps to succeed.
inline std::shared_ptr<const LdFile> roundTrip(const LdWriteInput& in, const LdWriteOptions& wopt = {},
                                               const LdOptions& ropt = {}) {
    auto bytes = LdWriter::build(in, wopt);
    REQUIRE(bytes.ok());
    auto file = LdFile::fromBuffer(share(std::move(*bytes)), ropt);
    REQUIRE(file.ok());
    return *file;
}

inline std::shared_ptr<const LdFile> parseBytes(std::vector<std::byte> bytes, const LdOptions& ropt = {}) {
    auto file = LdFile::fromBuffer(share(std::move(bytes)), ropt);
    REQUIRE(file.ok());
    return *file;
}

inline LdWriteChannel intChannel(std::string name, std::uint16_t code, std::uint16_t width,
                                 std::uint16_t rate, std::vector<std::int64_t> values) {
    LdWriteChannel c;
    c.name = std::move(name);
    c.typeCode = code;
    c.width = width;
    c.rateHz = rate;
    c.interpByte = layout::kInterpPrevious;
    c.rawInts = std::move(values);
    return c;
}

inline LdWriteChannel floatChannel(std::string name, std::uint16_t code, std::uint16_t width,
                                   std::uint16_t rate, std::vector<double> values) {
    LdWriteChannel c;
    c.name = std::move(name);
    c.typeCode = code;
    c.width = width;
    c.rateHz = rate;
    c.rawFloats = std::move(values);
    return c;
}

inline std::vector<std::int64_t> ramp(std::int64_t n, std::int64_t start = 0, std::int64_t step = 1) {
    std::vector<std::int64_t> v(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] = start + i * step;
    }
    return v;
}

inline const LdChannel* channelNamed(const LdFile& f, std::string_view name) {
    for (const LdChannel& c : f.channels()) {
        if (c.loggedName == name) {
            return &c;
        }
    }
    return nullptr;
}

inline std::vector<double> decodeAll(const LdFile& f, std::uint32_t ch) {
    auto v = f.decodeAsDouble(ch, 0, f.channels()[ch].sampleCount);
    REQUIRE(v.ok());
    return *v;
}

// Distance in float ulps between two doubles rounded to float (0 when both are NaN).
inline std::uint64_t f32UlpDistance(double a, double b) {
    const float fa = static_cast<float>(a);
    const float fb = static_cast<float>(b);
    if (std::isnan(fa) && std::isnan(fb)) {
        return 0;
    }
    if (std::isnan(fa) || std::isnan(fb)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto key = [](float f) {
        const auto u = std::bit_cast<std::uint32_t>(f);
        return (u & 0x80000000u) != 0
                   ? static_cast<std::int64_t>(0x80000000u) - static_cast<std::int64_t>(u & 0x7FFFFFFFu)
                   : static_cast<std::int64_t>(0x80000000u) + static_cast<std::int64_t>(u);
    };
    const std::int64_t d = key(fa) - key(fb);
    return static_cast<std::uint64_t>(d < 0 ? -d : d);
}

// One float ulp at |v| (the spacing of floats around v), as a double.
inline double f32Ulp(double v) {
    const float f = std::abs(static_cast<float>(v));
    if (!std::isfinite(f)) {
        return std::numeric_limits<double>::infinity();
    }
    return static_cast<double>(std::nextafter(f, std::numeric_limits<float>::infinity()) - f);
}

inline bool hasCode(const LdFile& f, std::string_view code) {
    return f.log().contains(code);
}

inline std::string codeDetail(const LdFile& f, std::string_view code) {
    for (const auto& e : f.log().entries()) {
        if (e.code == code) {
            return e.ctx.detail.value_or(std::string());
        }
    }
    return {};
}

} // namespace esses::formats::ld::test
