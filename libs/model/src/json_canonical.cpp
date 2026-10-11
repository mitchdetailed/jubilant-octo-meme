// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "json_canonical.hpp"

#include <esses/base/json_depth.hpp>
#include <esses/base/utf8.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <system_error>
#include <vector>

namespace esses::model::detail {
namespace {

using nlohmann::json;

struct NonFiniteNumber {
    std::string pointer;
};
struct TooDeep { // nesting beyond kMaxJsonDepth: write() recurses once per level
    std::string pointer;
};

int keyRank(std::string_view k) noexcept {
    if (k == "$schema") {
        return 0;
    }
    if (k == "schemaVersion") {
        return 1;
    }
    return 2;
}

void appendEscapedPointerToken(std::string& out, std::string_view token) {
    for (const char c : token) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
}

void writeString(std::string& out, std::string_view s) {
    std::string sanitized;
    if (!utf8::isValid(s)) {
        sanitized = utf8::sanitize(s);
        s = sanitized;
    }
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 0x0F];
            } else {
                out += ch;
            }
            break;
        }
    }
    out += '"';
}

void writeNumber(std::string& out, const json& j, const std::string& pointer) {
    std::array<char, 64> buf{};
    std::to_chars_result r{};
    if (j.is_number_unsigned()) {
        r = std::to_chars(buf.data(), buf.data() + buf.size(), j.get<std::uint64_t>());
    } else if (j.is_number_integer()) {
        r = std::to_chars(buf.data(), buf.data() + buf.size(), j.get<std::int64_t>());
    } else {
        const double d = j.get<double>();
        if (!std::isfinite(d)) {
            throw NonFiniteNumber{pointer};
        }
        if (d == 0.0) { // -0 and +0
            out += '0';
            return;
        }
        r = std::to_chars(buf.data(), buf.data() + buf.size(), d); // shortest round trip
    }
    out.append(buf.data(), r.ptr);
}

std::vector<const std::string*> sortedKeys(const json& obj) {
    std::vector<const std::string*> keys;
    keys.reserve(obj.size());
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        keys.push_back(&it.key());
    }
    std::sort(keys.begin(), keys.end(),
              [](const std::string* a, const std::string* b) { return canonicalKeyLess(*a, *b); });
    return keys;
}

// Recursion bounded like write(): a nesting deeper than the limit counts as multi-line (write() then
// refuses it anyway).
bool isMultiline(const json& arr, std::size_t depth = 0) {
    if (depth > kMaxJsonDepth) {
        return true;
    }
    return std::any_of(arr.begin(), arr.end(), [depth](const json& e) {
        return e.is_object() || (e.is_array() && isMultiline(e, depth + 1));
    });
}

void indent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 2u, ' ');
}

void write(std::string& out, const json& j, int depth, bool pretty, std::string& pointer) {
    if (static_cast<std::size_t>(depth) > kMaxJsonDepth) {
        throw TooDeep{pointer};
    }
    switch (j.type()) {
    case json::value_t::null:
    case json::value_t::discarded:
        out += "null";
        return;
    case json::value_t::boolean:
        out += j.get<bool>() ? "true" : "false";
        return;
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
    case json::value_t::number_float:
        writeNumber(out, j, pointer);
        return;
    case json::value_t::string:
        writeString(out, j.get_ref<const std::string&>());
        return;
    case json::value_t::binary: { // never produced by the parser; written as an array of byte values
        json arr = json::array();
        for (const auto byte : j.get_binary()) {
            arr.push_back(byte);
        }
        write(out, arr, depth, pretty, pointer);
        return;
    }
    case json::value_t::array: {
        if (j.empty()) {
            out += "[]";
            return;
        }
        const bool multi = pretty && isMultiline(j);
        out += '[';
        std::size_t index = 0;
        for (const json& e : j) {
            const std::size_t mark = pointer.size();
            pointer += '/';
            pointer += std::to_string(index);
            if (index != 0) {
                out += ',';
                if (pretty && !multi) {
                    out += ' ';
                }
            }
            if (multi) {
                out += '\n';
                indent(out, depth + 1);
            }
            write(out, e, depth + 1, pretty, pointer);
            pointer.resize(mark);
            ++index;
        }
        if (multi) {
            out += '\n';
            indent(out, depth);
        }
        out += ']';
        return;
    }
    case json::value_t::object: {
        if (j.empty()) {
            out += "{}";
            return;
        }
        out += '{';
        bool first = true;
        for (const std::string* key : sortedKeys(j)) {
            if (!first) {
                out += ',';
            }
            first = false;
            if (pretty) {
                out += '\n';
                indent(out, depth + 1);
            }
            writeString(out, *key);
            out += pretty ? ": " : ":";
            const std::size_t mark = pointer.size();
            pointer += '/';
            appendEscapedPointerToken(pointer, *key);
            write(out, j.at(*key), depth + 1, pretty, pointer);
            pointer.resize(mark);
        }
        if (pretty) {
            out += '\n';
            indent(out, depth);
        }
        out += '}';
        return;
    }
    }
}

Result<std::string> serialise(const json& value, bool pretty) {
    try {
        std::string out;
        std::string pointer;
        write(out, value, 0, pretty, pointer);
        if (pretty) {
            out += '\n';
        }
        return out;
    } catch (const NonFiniteNumber& e) {
        ErrorContext ctx;
        ctx.detail = e.pointer;
        return fail(ErrorCode::Invalid, "non-finite number at '" + e.pointer + "'", std::move(ctx));
    } catch (const TooDeep& e) {
        ErrorContext ctx;
        ctx.detail = e.pointer.substr(0, 256);
        return fail(ErrorCode::Invalid,
                    "JSON nested more than " + std::to_string(kMaxJsonDepth) + " levels deep; not written",
                    std::move(ctx));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory writing JSON");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("JSON serialisation failed: ") + e.what());
    }
}

} // namespace

bool canonicalKeyLess(std::string_view a, std::string_view b) noexcept {
    const int ra = keyRank(a);
    const int rb = keyRank(b);
    if (ra != rb) {
        return ra < rb;
    }
    // std::string_view compares through char_traits<char>, which orders like unsigned char: UTF-8 code units.
    return a < b;
}

Result<std::string> canonicalJson(const json& value) {
    return serialise(value, true);
}

Result<std::string> compactCanonicalJson(const json& value) {
    return serialise(value, false);
}

Result<OpaqueJson> toOpaque(const json& value) {
    auto text = compactCanonicalJson(value);
    if (!text) {
        return Failure{text.error()};
    }
    return OpaqueJsonAccess::make(std::move(text).value());
}

json fromOpaque(const OpaqueJson& value) {
    if (value.compact().empty()) {
        return {};
    }
    return json::parse(value.compact(), nullptr, false); // produced by compactCanonicalJson: always valid
}

} // namespace esses::model::detail
