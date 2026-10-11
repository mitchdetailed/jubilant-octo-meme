// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Reserved name, not yet implemented (M1, with esses_project). Until then esses_model uses
// model::OpaqueJson and a private canonical writer (libs/model/src/json_canonical.hpp) as stand-ins.
//
// JsonDoc (01 section 2.1, 08 section 6.3) wraps nlohmann-json and owns the single canonical serialisation of
// 02 section 6.5 rule 1: "$schema" then "schemaVersion" first, all other keys sorted by UTF-8 code units,
// 2-space indent, LF, UTF-8 without BOM, no trailing whitespace, shortest round-trip numbers (std::to_chars),
// -0 written as 0, non-finite numbers rejected. M0 phase A has no JSON library (nlohmann-json is vendored in
// phase B, cmake/third_party.cmake option ESSES_WITH_NLOHMANN_JSON), so this header only reserves the name.
// Planned interface (to be implemented in phase B, src/json_doc.cpp, private dependency
// esses::nlohmann_json):
//
//   class JsonDoc {
//   public:
//     static Result<JsonDoc> parse(std::string_view utf8);            // Format with offset on syntax errors
//     static Result<JsonDoc> load(const std::filesystem::path&);       // Io / Format
//     std::string canonical() const;                                   // the algorithm above
//     Result<void> saveAtomic(const std::filesystem::path&) const;     // AtomicFile + canonical()
//     std::string_view schemaId() const;  int schemaVersion() const;
//   };
#pragma once

namespace esses {
class JsonDoc; // reserved, implemented in M1
} // namespace esses
