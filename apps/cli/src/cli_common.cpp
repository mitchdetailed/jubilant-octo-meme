// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "cli_common.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/base/json_depth.hpp>
#include <esses/base/utf8.hpp>
#include <esses/model/opaque_json.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
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
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

} // namespace esses::cli
