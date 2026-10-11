// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "json_read.hpp"

#include <esses/base/json_depth.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace esses::model::detail {

using nlohmann::json;

namespace {

std::string_view typeName(const json& j) noexcept {
    switch (j.type()) {
    case json::value_t::null:
        return "null";
    case json::value_t::boolean:
        return "a boolean";
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
        return "an integer";
    case json::value_t::number_float:
        return "a number";
    case json::value_t::string:
        return "a string";
    case json::value_t::array:
        return "an array";
    case json::value_t::object:
        return "an object";
    default:
        return "a value";
    }
}

[[noreturn]] void wrongType(const json& j, const std::string& pointer, std::string_view expected) {
    violate(pointer, "expected " + std::string(expected) + ", found " + std::string(typeName(j)));
}

bool isHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

void violate(std::string pointer, std::string message, ErrorCode code, std::string schemaId) {
    throw JsonViolation{code, std::move(pointer), std::move(message), std::move(schemaId)};
}

std::string pointerChild(std::string_view parent, std::string_view key) {
    std::string out(parent);
    out += '/';
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::string pointerIndex(std::string_view parent, std::size_t index) {
    std::string out(parent);
    out += '/';
    out += std::to_string(index);
    return out;
}

const std::string& asString(const json& j, std::string_view pointer) {
    if (!j.is_string()) {
        wrongType(j, std::string(pointer), "a string");
    }
    return j.get_ref<const std::string&>();
}

std::int64_t asInt(const json& j, const std::string& pointer) {
    if (j.is_number_unsigned()) {
        const auto u = j.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            violate(pointer, "integer out of range");
        }
        return static_cast<std::int64_t>(u);
    }
    if (!j.is_number_integer()) {
        wrongType(j, pointer, "an integer");
    }
    return j.get<std::int64_t>();
}

std::uint64_t asU64(const json& j, const std::string& pointer) {
    if (j.is_number_unsigned()) {
        return j.get<std::uint64_t>();
    }
    if (!j.is_number_integer()) {
        wrongType(j, pointer, "an integer");
    }
    const auto v = j.get<std::int64_t>();
    if (v < 0) {
        violate(pointer, "expected a non-negative integer");
    }
    return static_cast<std::uint64_t>(v);
}

std::uint32_t asU32(const json& j, const std::string& pointer) {
    const std::uint64_t v = asU64(j, pointer);
    if (v > std::numeric_limits<std::uint32_t>::max()) {
        violate(pointer, "integer out of the unsigned 32-bit range");
    }
    return static_cast<std::uint32_t>(v);
}

double asNumber(const json& j, const std::string& pointer) {
    if (!j.is_number()) {
        wrongType(j, pointer, "a number");
    }
    const double d = j.get<double>();
    if (!std::isfinite(d)) {
        violate(pointer, "number out of range");
    }
    return d;
}

bool asBool(const json& j, const std::string& pointer) {
    if (!j.is_boolean()) {
        wrongType(j, pointer, "a boolean");
    }
    return j.get<bool>();
}

Uuid asUuid(const json& j, const std::string& pointer) {
    const std::string& s = asString(j, pointer);
    bool shapeOk = s.size() == 36;
    for (std::size_t i = 0; shapeOk && i < s.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        shapeOk = dash ? s[i] == '-' : isHex(s[i]);
    }
    const auto u = shapeOk ? Uuid::tryParse(s) : std::nullopt;
    if (!u) {
        violate(pointer, "expected a UUID (8-4-4-4-12 hex digits), found '" + s + "'");
    }
    return *u;
}

ObjectReader::ObjectReader(const json& j, std::string pointer) : j_(j), pointer_(std::move(pointer)) {
    if (!j_.is_object()) {
        wrongType(j_, pointer_, "an object");
    }
}

const json* ObjectReader::optional(std::string_view key) {
    seen_.emplace(key);
    const auto it = j_.find(std::string(key));
    if (it == j_.end() || it->is_null()) {
        return nullptr;
    }
    return &*it;
}

const json& ObjectReader::required(std::string_view key) {
    const json* j = optional(key);
    if (j == nullptr) {
        violate(pointer_, "required member '" + std::string(key) + "' is missing");
    }
    return *j;
}

void ObjectReader::closed() const {
    for (auto it = j_.begin(); it != j_.end(); ++it) {
        if (seen_.find(it.key()) == seen_.end()) {
            violate(pointerChild(pointer_, it.key()), "unknown member '" + it.key() + "'");
        }
    }
}

Result<json> parseJson(std::string_view text) {
    // Refused before parsing: the canonical writer and every walker recurse over a parsed value.
    if (jsonNestingDepth(text) > kMaxJsonDepth) {
        return fail(ErrorCode::Format,
                    "JSON nested more than " + std::to_string(kMaxJsonDepth) + " levels deep; not read");
    }
    try {
        return json::parse(text.begin(), text.end());
    } catch (const json::parse_error& e) {
        ErrorContext ctx;
        ctx.offset = static_cast<std::uint64_t>(e.byte);
        return fail(ErrorCode::Format, std::string("JSON syntax error: ") + e.what(), std::move(ctx));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory parsing JSON");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Format, std::string("JSON error: ") + e.what());
    }
}

void checkSchemaId(const json& object, std::string_view name, int major, const std::string& objectPointer) {
    ObjectReader r(object, objectPointer);
    const std::string pointer = r.child("$schema");
    const std::string& id = asString(r.required("$schema"), pointer);
    std::string_view rest(id);
    constexpr std::string_view kUrn = "urn:esses:";
    constexpr std::string_view kBare = "esses:";
    constexpr std::string_view kHttps = "https://";
    if (rest.starts_with(kUrn)) {
        rest.remove_prefix(kUrn.size());
    } else if (rest.starts_with(kBare)) {
        rest.remove_prefix(kBare.size());
    } else if (rest.starts_with(kHttps)) {
        const auto slash = rest.rfind('/');
        rest = rest.substr(slash + 1);
        if (rest.ends_with(".json")) {
            rest.remove_suffix(5);
        }
        if (rest.ends_with(".schema")) {
            rest.remove_suffix(7);
        }
    } else {
        violate(pointer, "'" + id + "' is not an Esses schema id");
    }
    const auto dash = rest.rfind('-');
    const std::string_view docName = dash == std::string_view::npos ? rest : rest.substr(0, dash);
    const std::string_view digits =
        dash == std::string_view::npos ? std::string_view{} : rest.substr(dash + 1);
    if (docName != name || digits.empty() || digits.size() > 6 ||
        digits.find_first_not_of("0123456789") != std::string_view::npos) {
        violate(pointer, "'" + id + "' names another schema (expected " + std::string(name) + "-" +
                             std::to_string(major) + ")");
    }
    int found = 0;
    for (const char c : digits) {
        found = found * 10 + (c - '0');
    }
    if (found != major) {
        violate(pointer,
                "schema " + std::string(name) + "-" + std::to_string(found) +
                    " is not supported (this build reads " + std::string(name) + "-" + std::to_string(major) +
                    ")",
                ErrorCode::Unsupported, id);
    }
}

} // namespace esses::model::detail
