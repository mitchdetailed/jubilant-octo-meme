// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Typed reading of JSON documents with JSON-pointer error reporting (private to esses_model). A violation is
// thrown as JsonViolation inside the library and turned into Result at the public boundary by guardJson(), so
// no exception crosses the library boundary (01 section 6.2 rule 1).
#pragma once

#include <esses/base/result.hpp>
#include <esses/base/uuid.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace esses::model::detail {

struct JsonViolation {
    ErrorCode code = ErrorCode::Invalid;
    std::string pointer;
    std::string message;
    std::string schemaId; // set for Unsupported: the schema id found
};

[[noreturn]] void violate(std::string pointer, std::string message, ErrorCode code = ErrorCode::Invalid,
                          std::string schemaId = {});

std::string pointerChild(std::string_view parent, std::string_view key); // RFC 6901 escaping
std::string pointerIndex(std::string_view parent, std::size_t index);

// Scalar readers: each checks the JSON type and throws JsonViolation at `pointer` otherwise. asString takes
// the pointer as a string_view so that a temporary pointer (r.child("x")) is not bound to a reference
// parameter of a function returning a reference (GCC 13 -Wdangling-reference).
const std::string& asString(const nlohmann::json& j, std::string_view pointer);
std::int64_t asInt(const nlohmann::json& j, const std::string& pointer); // integer within int64
std::uint32_t asU32(const nlohmann::json& j, const std::string& pointer);
std::uint64_t asU64(const nlohmann::json& j, const std::string& pointer);
double asNumber(const nlohmann::json& j, const std::string& pointer); // any finite number
bool asBool(const nlohmann::json& j, const std::string& pointer);
Uuid asUuid(const nlohmann::json& j, const std::string& pointer); // 8-4-4-4-12 hex, either case

// Members of one object. optional() treats an explicit null like an absent member; closed() rejects every
// member that was not asked for.
class ObjectReader {
public:
    ObjectReader(const nlohmann::json& j, std::string pointer);

    const nlohmann::json* optional(std::string_view key);
    const nlohmann::json& required(std::string_view key);
    std::string child(std::string_view key) const { return pointerChild(pointer_, key); }
    const std::string& pointer() const noexcept { return pointer_; }
    const nlohmann::json& value() const noexcept { return j_; }
    void closed() const;

private:
    const nlohmann::json& j_;
    std::string pointer_;
    std::set<std::string, std::less<>> seen_;
};

// Parses JSON text. Format with context.offset on a syntax error.
Result<nlohmann::json> parseJson(std::string_view text);

// Checks "$schema" of the object at `pointer` against "<name>-<major>" in the forms urn:esses:, esses: and
// the https form. Throws Invalid (another schema, malformed) or Unsupported (another major; message names
// it).
void checkSchemaId(const nlohmann::json& object, std::string_view name, int major,
                   const std::string& pointer = {});

// Runs `fn` and turns JsonViolation, nlohmann exceptions and bad_alloc into a failed Result.
template <class F> auto guardJson(F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const JsonViolation& v) {
        ErrorContext ctx;
        ctx.detail = v.pointer;
        if (!v.schemaId.empty()) {
            ctx.schemaId = v.schemaId;
        }
        std::string where = v.pointer.empty() ? std::string("document root") : "'" + v.pointer + "'";
        return Failure{Error{v.code, v.message + " (at " + where + ")", std::move(ctx)}};
    } catch (const std::bad_alloc&) {
        return Failure{Error{ErrorCode::OutOfMemory, "out of memory reading JSON", {}}};
    } catch (const std::exception& e) {
        return Failure{Error{ErrorCode::Internal, std::string("unexpected JSON error: ") + e.what(), {}}};
    }
}

} // namespace esses::model::detail
