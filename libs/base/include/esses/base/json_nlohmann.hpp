// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The nlohmann-json side of JsonDoc (01 section 2.1: "nlohmann-json behind JsonDoc"), for the libraries that
// read and write documents with nlohmann-json as a *private* dependency (esses_model, esses_project, ...).
// Including this header requires linking esses::nlohmann_json; it never appears in another public header
// (no JSON library type crosses a library boundary).
//
//   parseJson / readFile             text or file -> nlohmann value (depth limit, Format with byte offset)
//   canonicalJson / compactCanonicalJson / canonicalKeyLess
//                                    the one canonical writer of 02 section 6.5 rule 1 (json_doc.hpp)
//   toDoc / fromDoc                  nlohmann value <-> JsonDoc
//   typed readers                    as*, ObjectReader, checkSchemaId: a violation is thrown as
//                                    JsonViolation inside a library and turned into a Result at its public
//                                    boundary by guardJson(), so no exception crosses the boundary (01
//                                    section 6.2 rule 1)
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/uuid.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace esses::jsonio {

// ---- text and files
// ------------------------------------------------------------------------------------------

// Parses JSON text. Format with context.offset on a syntax error; Format when nested more than kMaxJsonDepth
// levels (refused before parsing: the writer and every walker recurse over a parsed value).
Result<nlohmann::json> parseJson(std::string_view text);

// The bytes of a file. Io with context.path when it cannot be opened or read.
Result<std::string> readFile(const std::filesystem::path& path);

// ---- the canonical writer (02 section 6.5 rule 1)
// ----------------------------------------------------------------

// Errors: Invalid ("non-finite number at <pointer>", context.detail = the pointer) when a number is NaN or
// infinite; Invalid when nested more than kMaxJsonDepth levels.
Result<std::string> canonicalJson(const nlohmann::json& value);        // pretty, trailing LF
Result<std::string> compactCanonicalJson(const nlohmann::json& value); // no white space, no LF

// Member order: "$schema" < "schemaVersion" < every other key in UTF-8 code-unit order.
bool canonicalKeyLess(std::string_view a, std::string_view b) noexcept;

// ---- JsonDoc
// ------------------------------------------------------------------------------------------------------

// compactCanonicalJson() errors.
Result<JsonDoc> toDoc(const nlohmann::json& value);
// The value of a JsonDoc (null for the absent value). Never fails: the text was produced by the writer.
nlohmann::json fromDoc(const JsonDoc& doc);

// ---- typed reading
// ----------------------------------------------------------------------------------------------

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
int asInt32(const nlohmann::json& j, const std::string& pointer);        // integer within int
std::uint32_t asU32(const nlohmann::json& j, const std::string& pointer);
std::uint64_t asU64(const nlohmann::json& j, const std::string& pointer);
double asNumber(const nlohmann::json& j, const std::string& pointer); // any finite number
bool asBool(const nlohmann::json& j, const std::string& pointer);
Uuid asUuid(const nlohmann::json& j, const std::string& pointer); // 8-4-4-4-12 hex, either case

// Members of one object. optional() treats an explicit null like an absent member; closed() rejects every
// member that was not asked for; unread() lists them (unknown members a document preserves).
class ObjectReader {
public:
    ObjectReader(const nlohmann::json& j, std::string pointer);

    const nlohmann::json* optional(std::string_view key);
    const nlohmann::json& required(std::string_view key);
    // Marks a member as read without looking at it.
    void skip(std::string_view key) { seen_.emplace(key); }
    std::string child(std::string_view key) const { return pointerChild(pointer_, key); }
    const std::string& pointer() const noexcept { return pointer_; }
    const nlohmann::json& value() const noexcept { return j_; }
    void closed() const;
    // The members that were not asked for, as an object (empty object when none).
    nlohmann::json unread() const;

private:
    const nlohmann::json& j_;
    std::string pointer_;
    std::set<std::string, std::less<>> seen_;
};

// Checks "$schema" of the object at `pointer` against "<name>-<major>" in the forms urn:esses:, esses: and
// the https form (02 section 6.6 rule 1, D-072). Throws Invalid (another schema, malformed) or Unsupported
// (another major; the message names it, schemaId carries the id found).
void checkSchemaId(const nlohmann::json& object, std::string_view name, int major,
                   const std::string& pointer = {});

// Runs `fn` and turns JsonViolation, nlohmann exceptions and bad_alloc into a failed Result: the message ends
// with " (at '<pointer>')" (or " (at document root)") and context.detail carries the pointer.
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

} // namespace esses::jsonio
