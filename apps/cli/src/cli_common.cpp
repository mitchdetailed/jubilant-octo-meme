// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "cli_common.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/base/json_depth.hpp>
#include <esses/base/utf8.hpp>
#include <esses/model/opaque_json.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <system_error>

namespace esses::cli {

std::string u8(const std::filesystem::path& path) {
    return pathToUtf8(path);
}

std::filesystem::path p8(std::string_view utf8) {
    return pathFromUtf8(utf8);
}

Result<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot open " + u8(path), std::move(ctx));
    }
    std::string text;
    try {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (const std::ios_base::failure&) {
        // libstdc++'s filebuf throws a failed read (a directory: EISDIR) whatever the exception mask
        in.setstate(std::ios_base::badbit);
    }
    if (in.bad()) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot read " + u8(path), std::move(ctx));
    }
    return text;
}

Result<void> writeFile(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            ErrorContext ctx;
            ctx.path = path.parent_path();
            return fail(ErrorCode::Io, "cannot create " + u8(path.parent_path()) + ": " + ec.message(),
                        std::move(ctx));
        }
    }
    return AtomicFile::write(path, bytes);
}

Result<nlohmann::json> parseJson(std::string_view text) {
    // Refused before parsing: the schema validator and the canonical writer recurse over a parsed value.
    if (jsonNestingDepth(text) > kMaxJsonDepth) {
        return fail(ErrorCode::Format,
                    "JSON nested more than " + std::to_string(kMaxJsonDepth) + " levels deep; not read");
    }
    nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded()) {
        // Locate the error offset with the throwing parser's message, without letting it escape.
        try {
            const nlohmann::json again = nlohmann::json::parse(text.begin(), text.end()); // throws here
            (void)again;
        } catch (const nlohmann::json::parse_error& e) {
            ErrorContext ctx;
            ctx.offset = e.byte;
            return fail(ErrorCode::Format, std::string("JSON syntax error: ") + e.what(), std::move(ctx));
        } catch (const std::exception& e) {
            return fail(ErrorCode::Format, std::string("JSON syntax error: ") + e.what());
        }
        return fail(ErrorCode::Format, "JSON syntax error");
    }
    return j;
}

Result<nlohmann::json> loadJson(const std::filesystem::path& path) {
    auto text = readFile(path);
    if (!text) {
        return Failure{text.error()};
    }
    auto j = parseJson(*text);
    if (!j) {
        Error e = j.error();
        e.context.path = path;
        e.message = u8(path) + ": " + e.message;
        return Failure{std::move(e)};
    }
    return j;
}

namespace {

bool hasNonFinite(const nlohmann::json& j) {
    if (j.is_number_float()) {
        return !std::isfinite(j.get<double>());
    }
    if (j.is_structured()) {
        for (const auto& v : j) {
            if (hasNonFinite(v)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

Result<std::string> canonicalText(const nlohmann::json& value) {
    if (hasNonFinite(value)) {
        return fail(ErrorCode::Invalid, "a non-finite number cannot be written to JSON");
    }
    std::string compact;
    try {
        compact = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Invalid, std::string("JSON serialisation failed: ") + e.what());
    }
    auto opaque = model::OpaqueJson::parse(compact);
    if (!opaque) {
        return Failure{opaque.error()};
    }
    return opaque->canonical();
}

namespace {

bool keepByte(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case ' ':
    case '.':
    case '_':
    case '-':
    case '(':
    case ')':
    case '+':
    case ',':
    case '=':
    case '@':
    case '[':
    case ']':
    case '{':
    case '}':
    case '#':
    case '&':
    case '!':
    case '\'':
    case '~':
        return true;
    default:
        return false;
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

} // namespace

std::string percentEncode(std::string_view utf8) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        const bool last = i + 1 == utf8.size();
        if (keepByte(c) && !(last && (c == ' ' || c == '.'))) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hexValue(text[i + 1]);
            const int lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::vector<std::string> splitList(std::string_view text, char separator) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(separator, start);
        const std::string_view part =
            text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!part.empty()) {
            out.emplace_back(part);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

namespace {

constexpr std::uint64_t kMaxRatePart = std::numeric_limits<std::uint32_t>::max();
// Mantissas are below 10^19, so a power of ten beyond 10^29 between num and den makes the reduced num or den
// at least 10^11: this bound refuses no rate that fits 32 bits.
constexpr std::int64_t kMaxRateExponent = 29;
constexpr std::size_t kMaxSignificantDigits = 19; // below 10^19 < 2^64

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// The exact value of an unsigned decimal: mantissa * 10^exp10, the mantissa without trailing zeros (0: zero).
struct ExactDecimal {
    std::uint64_t mantissa = 0;
    std::int64_t exp10 = 0;
};

// Digits with an optional '.' and fraction and an optional exponent (e or E, an optional sign, digits).
Result<ExactDecimal> parseExactDecimal(std::string_view s) {
    if (s.empty()) {
        return fail(ErrorCode::Invalid, "a number is missing");
    }
    const std::string quoted = "'" + std::string(s) + "'";
    if (s.front() == '-') {
        return fail(ErrorCode::Invalid, quoted + " is negative");
    }
    const auto notANumber = [&] { return fail(ErrorCode::Invalid, quoted + " is not a decimal number"); };
    std::string digits; // the integer and fraction digits, without the point
    std::int64_t fractionDigits = 0;
    std::size_t i = 0;
    for (; i < s.size() && isDigit(s[i]); ++i) {
        digits.push_back(s[i]);
    }
    if (i < s.size() && s[i] == '.') {
        for (++i; i < s.size() && isDigit(s[i]); ++i) {
            digits.push_back(s[i]);
            ++fractionDigits;
        }
    }
    if (digits.empty()) {
        return notANumber();
    }
    std::int64_t exponent = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        const bool negative = i < s.size() && s[i] == '-';
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            ++i;
        }
        if (i == s.size() || !isDigit(s[i])) {
            return notANumber();
        }
        std::int32_t e = 0;
        const std::from_chars_result r = std::from_chars(s.data() + i, s.data() + s.size(), e);
        if (r.ec == std::errc::result_out_of_range) {
            return fail(ErrorCode::Invalid, "the exponent of " + quoted + " is out of range");
        }
        if (r.ec != std::errc()) {
            return notANumber();
        }
        i = static_cast<std::size_t>(r.ptr - s.data());
        exponent = negative ? -static_cast<std::int64_t>(e) : static_cast<std::int64_t>(e);
    }
    if (i != s.size()) {
        return notANumber();
    }
    const std::size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos) {
        return ExactDecimal{};
    }
    const std::size_t last = digits.find_last_not_of('0');
    const std::string_view significant = std::string_view(digits).substr(first, last - first + 1);
    if (significant.size() > kMaxSignificantDigits) {
        return fail(ErrorCode::Invalid, quoted + " has more than " + std::to_string(kMaxSignificantDigits) +
                                            " significant digits");
    }
    ExactDecimal d;
    const std::from_chars_result m =
        std::from_chars(significant.data(), significant.data() + significant.size(), d.mantissa);
    if (m.ec != std::errc() || m.ptr != significant.data() + significant.size()) {
        return notANumber(); // not reached: at most 19 digits
    }
    d.exp10 = exponent - fractionDigits + static_cast<std::int64_t>(digits.size() - 1 - last);
    return d;
}

} // namespace

Result<Rate> parseRate(std::string_view text) {
    const std::size_t slash = text.find('/');
    Result<ExactDecimal> num = parseExactDecimal(text.substr(0, slash));
    if (!num) {
        return Failure{num.error()};
    }
    ExactDecimal den{1, 0};
    if (slash != std::string_view::npos) {
        Result<ExactDecimal> d = parseExactDecimal(text.substr(slash + 1));
        if (!d) {
            return Failure{d.error()};
        }
        den = *d;
    }
    if (den.mantissa == 0) {
        return fail(ErrorCode::Invalid, "the denominator is zero");
    }
    if (num->mantissa == 0) {
        return fail(ErrorCode::Invalid, "the rate is zero");
    }
    const auto outOfRange = [] {
        return fail(ErrorCode::Invalid, "num and den do not both fit 32 bits once reduced");
    };
    // num/den = (mantissa(num) / mantissa(den)) * 10^exp10, applied one factor of ten at a time: the fraction
    // stays in lowest terms and the side that is multiplied never shrinks, so once it passes 32 bits the rate
    // is out of range (and every product stays within 64 bits).
    std::int64_t exp10 = num->exp10 - den.exp10;
    if (exp10 > kMaxRateExponent || exp10 < -kMaxRateExponent) {
        return outOfRange();
    }
    std::uint64_t a = num->mantissa;
    std::uint64_t b = den.mantissa;
    const std::uint64_t g = std::gcd(a, b);
    a /= g;
    b /= g;
    for (; exp10 > 0; --exp10) {
        if (a > kMaxRatePart) {
            return outOfRange();
        }
        const std::uint64_t g10 = std::gcd(b, std::uint64_t{10});
        b /= g10;
        a *= 10 / g10;
    }
    for (; exp10 < 0; ++exp10) {
        if (b > kMaxRatePart) {
            return outOfRange();
        }
        const std::uint64_t g10 = std::gcd(a, std::uint64_t{10});
        a /= g10;
        b *= 10 / g10;
    }
    if (a > kMaxRatePart || b > kMaxRatePart) {
        return outOfRange();
    }
    return Rate{static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)};
}

} // namespace esses::cli
