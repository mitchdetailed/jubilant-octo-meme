// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "expect_channels.hpp"

#include "cli_common.hpp"

#include <esses/base/utf8.hpp>

#include <fast_float/fast_float.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>

namespace esses::cli {
namespace {

constexpr std::string_view kHeader = "index,timeUs,value,valid";

// cp1252 bytes 0x80..0x9F; the five undefined bytes map to the C1 control of the same value, as the oracle
// does (tools/ld_ref_decode.py _cp1252).
constexpr char32_t kCp1252High[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                      0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

std::string cp1252Of(std::string_view bytes) {
    std::string out;
    for (const char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
        } else if (b < 0xA0) {
            utf8::append(out, kCp1252High[b - 0x80]);
        } else {
            utf8::append(out, static_cast<char32_t>(b));
        }
    }
    return out;
}

std::string_view rtrimSpaces(std::string_view s) {
    while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view stripDuplicateSuffix(std::string_view s) {
    // " (n)" with n >= 2
    if (s.size() < 4 || s.back() != ')') {
        return s;
    }
    const std::size_t open = s.rfind(" (");
    if (open == std::string_view::npos || open + 2 >= s.size() - 1) {
        return s;
    }
    for (std::size_t i = open + 2; i + 1 < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return s;
        }
    }
    return s.substr(0, open);
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

template <class T> bool parseInt(std::string_view s, T& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool parseDouble(std::string_view s, double& out) {
    if (s == "nan" || s == "-nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (s == "inf") {
        out = std::numeric_limits<double>::infinity();
        return true;
    }
    if (s == "-inf") {
        out = -std::numeric_limits<double>::infinity();
        return true;
    }
    const auto r = fast_float::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::uint64_t orderedKey64(double d) noexcept {
    const auto u = std::bit_cast<std::uint64_t>(d);
    return (u & 0x8000000000000000ull) != 0 ? 0x8000000000000000ull - (u & 0x7FFFFFFFFFFFFFFFull)
                                            : 0x8000000000000000ull + u;
}

std::uint64_t orderedKey32(float f) noexcept {
    const auto u = std::bit_cast<std::uint32_t>(f);
    return (u & 0x80000000u) != 0 ? std::uint64_t{0x80000000u} - (u & 0x7FFFFFFFu)
                                  : std::uint64_t{0x80000000u} + u;
}

} // namespace

std::optional<std::pair<std::uint32_t, std::string>> parseExpectedFileName(std::string_view fileName) {
    if (fileName.size() < 6 || fileName.substr(fileName.size() - 4) != ".csv") {
        return std::nullopt;
    }
    const std::string_view stem = fileName.substr(0, fileName.size() - 4);
    const std::size_t us = stem.find('_');
    if (us == 0 || us == std::string_view::npos) {
        return std::nullopt;
    }
    std::uint32_t index = 0;
    if (!parseInt(stem.substr(0, us), index)) {
        return std::nullopt;
    }
    return std::make_pair(index, percentDecode(stem.substr(us + 1)));
}

std::string expectedFileName(std::uint32_t oracleIndex, std::string_view name) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03u_", static_cast<unsigned>(oracleIndex));
    return std::string(buf) + percentEncode(name) + ".csv";
}

Result<std::vector<ExpectedSample>> parseExpectedCsv(std::string_view text) {
    std::vector<ExpectedSample> out;
    std::size_t pos = 0;
    std::size_t line = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view row = text.substr(pos, end - pos);
        pos = end + 1;
        ++line;
        if (!row.empty() && row.back() == '\r') {
            row.remove_suffix(1);
        }
        if (line == 1) {
            if (row != kHeader) {
                return fail(ErrorCode::Format, "expected the header '" + std::string(kHeader) + "'");
            }
            continue;
        }
        if (row.empty()) {
            continue;
        }
        std::string_view f[4];
        std::size_t start = 0;
        int n = 0;
        for (; n < 4; ++n) {
            const std::size_t comma = row.find(',', start);
            if (n < 3 && comma == std::string_view::npos) {
                break;
            }
            f[n] = row.substr(start, n < 3 ? comma - start : std::string_view::npos);
            start = comma + 1;
        }
        ExpectedSample s;
        if (n != 4 || !parseInt(f[0], s.index) || !parseInt(f[1], s.timeUs) || !parseDouble(f[2], s.value) ||
            (f[3] != "0" && f[3] != "1")) {
            ErrorContext c;
            c.offset = line;
            return fail(ErrorCode::Format,
                        "malformed row " + std::to_string(line) + ": '" + std::string(row) + "'",
                        std::move(c));
        }
        s.valid = f[3] == "1";
        if (!out.empty() && s.index <= out.back().index) {
            ErrorContext c;
            c.offset = line;
            return fail(ErrorCode::Format, "row " + std::to_string(line) + ": indices must ascend",
                        std::move(c));
        }
        out.push_back(s);
    }
    if (line == 0) {
        return fail(ErrorCode::Format, "empty file");
    }
    return out;
}

Result<ExpectedChannel> readExpectedChannel(const std::filesystem::path& file) {
    const auto parsed = parseExpectedFileName(u8(file.filename()));
    if (!parsed) {
        return fail(ErrorCode::Format, u8(file.filename()) + ": not <NNN>_<name>.csv");
    }
    auto text = readFile(file);
    if (!text) {
        return Failure{text.error()};
    }
    auto samples = parseExpectedCsv(*text);
    if (!samples) {
        Error e = samples.error();
        e.message = u8(file) + ": " + e.message;
        e.context.path = file;
        return Failure{std::move(e)};
    }
    ExpectedChannel ch;
    ch.file = file;
    ch.oracleIndex = parsed->first;
    ch.name = parsed->second;
    ch.samples = std::move(samples).value();
    return ch;
}

Result<std::vector<ExpectedChannel>> readExpectedChannels(const std::filesystem::path& fixtureDir) {
    std::vector<ExpectedChannel> out;
    const std::filesystem::path dir = fixtureDir / "expected.channels";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".csv") {
            continue;
        }
        auto ch = readExpectedChannel(it->path());
        if (!ch) {
            return Failure{ch.error()};
        }
        out.push_back(std::move(ch).value());
    }
    std::sort(out.begin(), out.end(), [](const ExpectedChannel& a, const ExpectedChannel& b) {
        return a.oracleIndex < b.oracleIndex;
    });
    return out;
}

std::string markInvalid(std::string_view csv,
                        const std::vector<std::pair<std::uint64_t, std::uint64_t>>& invalid) {
    std::string out;
    out.reserve(csv.size());
    std::size_t pos = 0;
    bool first = true;
    while (pos < csv.size()) {
        std::size_t end = csv.find('\n', pos);
        const bool hasNl = end != std::string_view::npos;
        if (!hasNl) {
            end = csv.size();
        }
        std::string row(csv.substr(pos, end - pos));
        pos = end + 1;
        if (!first && !row.empty()) {
            std::uint64_t index = 0;
            const std::size_t comma = row.find(',');
            if (comma != std::string::npos && parseInt(std::string_view(row).substr(0, comma), index)) {
                for (const auto& [a, b] : invalid) {
                    if (index >= a && index < b) {
                        if (!row.empty() && row.back() == '1') {
                            row.back() = '0';
                        }
                        break;
                    }
                }
            }
        }
        first = false;
        out += row;
        if (hasNl) {
            out.push_back('\n');
        }
    }
    return out;
}

Result<Tolerance> parseTolerance(const nlohmann::json& tol) {
    Tolerance t;
    const auto one = [&](const char* key, std::uint64_t& field) -> Result<void> {
        if (!tol.is_object() || !tol.contains(key) || !tol[key].is_string()) {
            return fail(ErrorCode::Invalid, std::string("oracle.tolerance.") + key + " missing");
        }
        const std::string s = tol[key].get<std::string>();
        if (s == "exact") {
            field = 0;
            return ok();
        }
        if (s.size() > 3 && s.substr(s.size() - 3) == "ulp" &&
            parseInt(std::string_view(s).substr(0, s.size() - 3), field)) {
            return ok();
        }
        return fail(ErrorCode::Invalid,
                    std::string("oracle.tolerance.") + key + " '" + s + "' is not exact or <n>ulp");
    };
    if (auto r = one("F32", t.f32Ulps); !r) {
        return Failure{r.error()};
    }
    if (auto r = one("F64", t.f64Ulps); !r) {
        return Failure{r.error()};
    }
    return t;
}

std::uint64_t ulpDistanceF32(double a, double b) noexcept {
    const auto fa = static_cast<float>(a);
    const auto fb = static_cast<float>(b);
    if (std::isnan(fa) || std::isnan(fb)) {
        return std::isnan(fa) && std::isnan(fb) ? 0 : std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t ka = orderedKey32(fa);
    const std::uint64_t kb = orderedKey32(fb);
    return ka > kb ? ka - kb : kb - ka;
}

std::uint64_t ulpDistanceF64(double a, double b) noexcept {
    if (std::isnan(a) || std::isnan(b)) {
        return std::isnan(a) && std::isnan(b) ? 0 : std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t ka = orderedKey64(a);
    const std::uint64_t kb = orderedKey64(b);
    return ka > kb ? ka - kb : kb - ka;
}

bool namesCompatible(std::string_view readerName, std::string_view oracleName) {
    const std::string_view oracle = rtrimSpaces(oracleName);
    const std::string_view reader = stripDuplicateSuffix(readerName);
    if (rtrimSpaces(reader) == oracle || rtrimSpaces(readerName) == oracle) {
        return true;
    }
    if (!oracle.empty() && startsWith(reader, oracle)) {
        return true; // long name (aux block) extending a truncated 32-byte field
    }
    const std::string asCp1252 = cp1252Of(reader);
    const std::string_view moj = rtrimSpaces(asCp1252);
    return moj == oracle || (!oracle.empty() && startsWith(moj, oracle));
}

} // namespace esses::cli
